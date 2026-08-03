#include "Os2Pack2Decoder.h"
#include "Os2Pack2Tables.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace peare {
namespace {

const int kSymbolCount = 433;
const int kLeafLimit = 0x06C4;
const int kRecordCount = 900;
const int kFastCount = 512;
const int kScratchCount = 1536;
const int kIntermediateBlockLimit = 0x3008;
const int kHistorySize = 0xCFDC;
const int kHistorySeedOffset = 0xC022;
const qsizetype kMaxPack2Output = qsizetype(512) * 1024 * 1024;

quint16 readLe16(const QByteArray& data, qsizetype offset, bool* ok = nullptr)
{
    const bool valid = offset >= 0 && offset + 2 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

void appendLe16(QByteArray* output, quint16 value)
{
    output->append(char(value & 0xFF));
    output->append(char(value >> 8));
}

struct HuffTree {
    std::array<quint16, kRecordCount> weights;
    std::array<quint16, kRecordCount> childOne;
    std::array<quint16, kRecordCount> childZero;
    std::array<quint16, kFastCount> fast;
    std::array<quint8, kFastCount> fastLength;
    quint16 root;

    HuffTree() : root(0)
    {
        weights.fill(0);
        childOne.fill(0);
        childZero.fill(0);
        fast.fill(0);
        fastLength.fill(0);
    }
};

quint16 nodeWeight(const std::array<quint16, kRecordCount>& weights, quint16 node)
{
    const int index = int(node) / 4;
    return index >= 0 && index < kRecordCount ? weights[size_t(index)] : 0;
}

void sortIdsExact(std::array<quint16, kScratchCount>* values,
                  const std::array<quint16, kRecordCount>& weights,
                  int initialLeft, int initialRight)
{
    if (initialRight <= initialLeft) return;
    std::vector<std::pair<int, int> > ranges;
    ranges.push_back(std::make_pair(initialLeft, initialRight));
    while (!ranges.empty()) {
        int left = ranges.back().first;
        int right = ranges.back().second;
        ranges.pop_back();
        while (right > left) {
            if (right - left <= 0x10) {
                for (int current = left + 1; current <= right; ++current) {
                    const quint16 key = (*values)[size_t(current)];
                    const quint16 keyWeight = nodeWeight(weights, key);
                    int position = left;
                    while (position < current &&
                           nodeWeight(weights, (*values)[size_t(position)]) < keyWeight) {
                        ++position;
                    }
                    if (position < current) {
                        std::memmove(&(*values)[size_t(position + 1)],
                                     &(*values)[size_t(position)],
                                     size_t(current - position) * sizeof(quint16));
                        (*values)[size_t(position)] = key;
                    }
                }
                break;
            }

            int lower = left;
            int upper = right;
            const quint16 pivot = nodeWeight(weights, (*values)[size_t((left + right) / 2)]);
            while (true) {
                while (nodeWeight(weights, (*values)[size_t(lower)]) < pivot) ++lower;
                while (nodeWeight(weights, (*values)[size_t(upper)]) > pivot) --upper;
                if (upper >= lower) {
                    std::swap((*values)[size_t(lower)], (*values)[size_t(upper)]);
                    ++lower;
                    --upper;
                }
                if (upper < lower) break;
            }

            const int rightLength = right - lower;
            const int leftLength = upper - left;
            if (rightLength <= leftLength) {
                if (right > lower) ranges.push_back(std::make_pair(lower, right));
                right = upper;
            } else {
                if (left < upper) ranges.push_back(std::make_pair(left, upper));
                left = lower;
            }
        }
    }
}

void makeFastTable(HuffTree* tree)
{
    for (int prefix = 0; prefix < kFastCount; ++prefix) {
        quint16 bits = quint16(prefix << 7);
        quint16 node = tree->root;
        int depth = 0;
        while (node >= kLeafLimit && depth < 9) {
            const int index = int(node) / 4;
            const bool one = (bits & 0x8000U) != 0;
            node = one ? tree->childOne[size_t(index)] : tree->childZero[size_t(index)];
            bits = quint16(bits << 1);
            ++depth;
        }
        tree->fast[size_t(prefix)] = node;
        tree->fastLength[size_t(prefix)] = quint8(depth);
    }
}

HuffTree buildTree(const std::array<quint16, kSymbolCount>& leafWeights,
                   const std::array<quint16, kFastCount>* previousFast)
{
    HuffTree tree;
    for (int i = 0; i < kSymbolCount; ++i) tree.weights[size_t(i)] = leafWeights[size_t(i)];

    std::array<quint16, kScratchCount> scratch;
    scratch.fill(0);
    if (previousFast) {
        std::copy(previousFast->begin(), previousFast->end(), scratch.begin());
    }

    int count = 0;
    int oneCount = 0;
    quint16 zeroLeaf = 0;
    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        const quint16 node = quint16(symbol * 4);
        const quint16 weight = leafWeights[size_t(symbol)];
        if (weight == 0) {
            zeroLeaf = node;
        } else if (weight == 1) {
            const quint16 displaced = scratch[size_t(oneCount)];
            scratch[size_t(count++)] = displaced;
            scratch[size_t(oneCount++)] = node;
        } else {
            scratch[size_t(count++)] = node;
        }
    }
    if (count == 0) return tree;
    if (count == 1) {
        scratch[size_t(count++)] = scratch[size_t(oneCount)];
        scratch[size_t(oneCount++)] = zeroLeaf;
        tree.weights[size_t(zeroLeaf / 4)] = 1;
    }

    sortIdsExact(&scratch, tree.weights, oneCount, count - 1);

    int pointer = 0;
    int remaining = count;
    quint16 nextNode = kLeafLimit;
    while (remaining != 2) {
        --remaining;
        const quint16 first = scratch[size_t(pointer)];
        const quint16 second = scratch[size_t(pointer + 1)];
        ++pointer;
        const quint16 combined = quint16(nodeWeight(tree.weights, first) +
                                          nodeWeight(tree.weights, second));

        int lower = pointer + 1;
        int upper = count;
        while (upper > lower) {
            const int middle = (upper + lower) / 2;
            if (nodeWeight(tree.weights, scratch[size_t(middle)]) >= combined)
                upper = middle;
            else
                lower = middle + 1;
        }
        const int insertion = (upper + lower) / 2;
        const int words = insertion - pointer;
        if (words > 0) {
            std::memmove(&scratch[size_t(pointer - 1)], &scratch[size_t(pointer)],
                         size_t(words) * sizeof(quint16));
        }
        scratch[size_t(insertion - 1)] = nextNode;

        const int nodeIndex = int(nextNode) / 4;
        tree.weights[size_t(nodeIndex)] = combined;
        tree.childOne[size_t(nodeIndex)] = first;
        tree.childZero[size_t(nodeIndex)] = second;
        nextNode = quint16(nextNode + 4);
    }

    const quint16 first = scratch[size_t(pointer)];
    const quint16 second = scratch[size_t(pointer + 1)];
    tree.root = nextNode;
    const int rootIndex = int(nextNode) / 4;
    tree.weights[size_t(rootIndex)] = quint16(nodeWeight(tree.weights, first) +
                                               nodeWeight(tree.weights, second));
    tree.childOne[size_t(rootIndex)] = first;
    tree.childZero[size_t(rootIndex)] = second;
    makeFastTable(&tree);
    return tree;
}

std::array<quint16, kSymbolCount> arrayFrom(const quint16* values)
{
    std::array<quint16, kSymbolCount> result;
    for (int i = 0; i < kSymbolCount; ++i) result[size_t(i)] = values[i];
    return result;
}

struct StaticTrees {
    HuffTree frequency;
    HuffTree secondary;
};

StaticTrees makeStaticTrees()
{
    StaticTrees result;
    result.frequency = buildTree(arrayFrom(os2pack2data::kFrequencyWeights), nullptr);
    result.secondary = buildTree(arrayFrom(os2pack2data::kSecondaryWeights),
                                 &result.frequency.fast);
    return result;
}

const StaticTrees& staticTrees()
{
    static const StaticTrees value = makeStaticTrees();
    return value;
}

class BitReader {
public:
    BitReader(const QByteArray& data, qsizetype start, qsizetype logicalEnd)
        : data_(data), start_(start), logicalEnd_(logicalEnd), position_(start),
          buffer_(0), bitCount_(0), failed_(false)
    {
    }

    int decode(const HuffTree& tree)
    {
        fill();
        if (failed_) return -1;
        const int prefix = int(buffer_ >> 7);
        quint16 node = tree.fast[size_t(prefix)];
        const int length = tree.fastLength[size_t(prefix)];
        consume(length);
        while (node >= kLeafLimit) {
            quint16 bits = buffer_;
            const int oldCount = bitCount_;
            --bitCount_;
            if (oldCount == 0) {
                bits = quint16(nextByte()) << 8;
                bitCount_ = 7;
                if (failed_) return -1;
            }
            const int index = int(node) / 4;
            node = (bits & 0x8000U) ? tree.childOne[size_t(index)]
                                     : tree.childZero[size_t(index)];
            buffer_ = quint16(bits << 1);
        }
        return int(node) / 4;
    }

    bool readPrefix(int* mode, int* extra)
    {
        fill();
        if (failed_) return false;
        const quint16 bits = buffer_;
        if ((bits & 0x8000U) == 0) {
            *mode = 0;
            *extra = int((bits >> 11) & 0x0F);
            consume(5);
        } else if ((bits & 0x4000U) == 0) {
            *mode = 1;
            *extra = int((bits >> 8) & 0x3F);
            consume(8);
        } else {
            *mode = 2;
            *extra = int((bits >> 7) & 0x7F);
            consume(9);
        }
        return true;
    }

    qsizetype consumed() const
    {
        return (position_ - start_) - bitCount_ / 8;
    }

    bool failed() const { return failed_; }

private:
    uchar nextByte()
    {
        if (position_ < logicalEnd_ && position_ < data_.size())
            return uchar(data_.at(position_++));
        // UNPACK2 keeps a 16-bit lookahead and can fetch one byte past the
        // logical stream, then remove that byte from its consumed count.
        if (position_ < logicalEnd_ + 16) {
            ++position_;
            return 0;
        }
        failed_ = true;
        return 0;
    }

    void fill()
    {
        while (bitCount_ <= 8 && !failed_) {
            const uchar value = nextByte();
            const int shift = 8 - bitCount_;
            buffer_ = quint16(buffer_ | (quint16(value) << shift));
            bitCount_ += 8;
        }
    }

    void consume(int count)
    {
        buffer_ = quint16(buffer_ << count);
        bitCount_ -= count;
    }

    const QByteArray& data_;
    qsizetype start_;
    qsizetype logicalEnd_;
    qsizetype position_;
    quint16 buffer_;
    int bitCount_;
    bool failed_;
};

std::array<quint16, kSymbolCount> scaleWeights(
    const std::array<quint16, kSymbolCount>& frequencies, quint16 factorZero,
    quint16 factorOne)
{
    std::array<quint16, kSymbolCount> result;
    quint16 maximum = 0;
    for (int i = 0; i < kSymbolCount; ++i) {
        quint16 value = 0;
        if (frequencies[size_t(i)] != 0) {
            const quint16 factor = os2pack2data::kSymbolClass[i] ? factorOne : factorZero;
            value = quint16(quint32(frequencies[size_t(i)]) * factor);
        }
        result[size_t(i)] = value;
        if (value > maximum) maximum = value;
    }
    if (maximum > 0xFF) {
        const quint16 normalizer = quint16(0xFFFFU / maximum);
        for (int i = 0; i < kSymbolCount; ++i) {
            const quint16 oldValue = result[size_t(i)];
            if (oldValue != 0) {
                quint16 value = quint16((quint32(oldValue) * normalizer) >> 8);
                if (value == 0) value = 1;
                result[size_t(i)] = value;
            }
        }
    }
    return result;
}

bool decodeFrame(const QByteArray& data, qsizetype start, qsizetype end,
                 QByteArray* intermediate, qsizetype* consumed, QString* error)
{
    bool ok = false;
    const quint16 target = readLe16(data, start, &ok);
    if (!ok) { *error = QStringLiteral("Truncated PACK2 frame header"); return false; }
    if (target == 0xFFFF) {
        const quint16 length = readLe16(data, start + 2, &ok);
        if (!ok || start + 4 + length > end) {
            *error = QStringLiteral("Truncated stored PACK2 frame");
            return false;
        }
        *intermediate = data.mid(start + 4, length);
        *consumed = 4 + length;
        return true;
    }
    if (start + 6 > end) { *error = QStringLiteral("Truncated PACK2 entropy header"); return false; }

    const quint16 factorA = uchar(data.at(start + 2));
    const quint16 factorB = uchar(data.at(start + 3));
    const quint16 factorC = uchar(data.at(start + 4));
    const quint16 factorD = uchar(data.at(start + 5));
    BitReader bits(data, start + 6, end);
    const StaticTrees& fixed = staticTrees();

    std::array<quint16, kSymbolCount> frequencies;
    frequencies.fill(0);
    int frequencyIndex = 0;
    while (frequencyIndex < kSymbolCount) {
        const int symbol = bits.decode(fixed.frequency);
        if (symbol < 0) { *error = QStringLiteral("Truncated PACK2 frequency table"); return false; }
        if (symbol == 0x100) {
            for (int i = 0; i < 16 && frequencyIndex < kSymbolCount; ++i)
                frequencies[size_t(frequencyIndex++)] = 0;
        } else if (symbol < 0x100) {
            frequencies[size_t(frequencyIndex++)] = quint16(symbol);
        } else {
            *error = QStringLiteral("Invalid PACK2 frequency symbol");
            return false;
        }
    }

    const std::array<quint16, kSymbolCount> primaryWeights =
        scaleWeights(frequencies, factorA, factorB);
    HuffTree primary = buildTree(primaryWeights, &fixed.secondary.fast);
    HuffTree contextual;
    if (factorC == factorB && factorD == factorA) {
        contextual = primary;
    } else if (factorC == 0 && factorD == 0) {
        contextual = buildTree(primaryWeights, &primary.fast);
    } else {
        contextual = buildTree(scaleWeights(frequencies, factorD, factorC), &primary.fast);
    }

    intermediate->clear();
    intermediate->reserve(target);
    int state = 0;
    int context = 0;
    quint16 previousStateOne = 0;
    quint16 previousModeZero = 0;
    quint16 previousModeOne = 0;
    quint16 previousModeTwo = 0;
    quint16 previousStateSix = 0;
    std::array<quint16, 48> wordTable;
    std::array<quint16, 48> tokenTable;
    wordTable.fill(0);
    tokenTable.fill(0);
    int wordPointer = 32;
    int tokenPointer = 32;

    while (intermediate->size() < target) {
        if (state != 0) {
            if (state < 3) {
                int mode = 0;
                int extra = 0;
                if (state == 2 && !bits.readPrefix(&mode, &extra)) {
                    *error = QStringLiteral("Truncated PACK2 distance prefix");
                    return false;
                }
                int symbol = bits.decode(fixed.secondary);
                if (symbol < 0) { *error = QStringLiteral("Truncated PACK2 value code"); return false; }
                if (state == 1) {
                    if (symbol == 0x100) symbol = previousStateOne;
                    else previousStateOne = quint16(symbol);
                    intermediate->append(char(symbol & 0xFF));
                } else {
                    quint16 value = 0;
                    if (mode == 0) {
                        if (symbol == 0x100) symbol = previousModeZero;
                        else previousModeZero = quint16(symbol);
                        value = quint16(((symbol + 0x10) << 4) + extra);
                    } else if (mode == 1) {
                        if (symbol == 0x100) symbol = previousModeOne;
                        else previousModeOne = quint16(symbol);
                        value = quint16(((symbol + 0x44) << 6) + extra);
                    } else {
                        if (symbol == 0x100) symbol = previousModeTwo;
                        else previousModeTwo = quint16(symbol);
                        value = quint16(((symbol + 0xA2) << 7) + extra);
                    }
                    appendLe16(intermediate, value);
                }
                state = 0;
            } else {
                int symbol = bits.decode(fixed.secondary);
                if (symbol < 0) { *error = QStringLiteral("Truncated PACK2 long-token value"); return false; }
                ++state;
                if (state == 6) {
                    state = 0;
                    if (symbol == 0x100) symbol = previousStateSix;
                    else previousStateSix = quint16(symbol);
                } else {
                    symbol = symbol == 0x100 ? 0 : symbol + 1;
                }
                intermediate->append(char(symbol & 0xFF));
            }
            continue;
        }

        const HuffTree& mainTree = context == 0 ? primary : contextual;
        int symbol = bits.decode(mainTree);
        if (symbol < 0 || symbol >= kSymbolCount) {
            *error = QStringLiteral("Invalid PACK2 main symbol");
            return false;
        }
        context = os2pack2data::kSymbolClass[symbol];

        if (symbol <= 0xFF) {
            intermediate->append(char(symbol));
        } else if (symbol < 0x181) {
            const int tag = symbol - 0x100;
            intermediate->append(char(0x9E));
            intermediate->append(char(tag));
            state = os2pack2data::kTokenExtraMode[tag];
            if (state != 0) {
                const int oldPointer = tokenPointer;
                --tokenPointer;
                if (oldPointer == 0) {
                    for (int i = 0; i < 16; ++i) tokenTable[size_t(32 + i)] = tokenTable[size_t(i)];
                    tokenPointer = 31;
                }
                tokenTable[size_t(tokenPointer)] = quint16(intermediate->size() - 1);
            }
        } else if (symbol < 0x191) {
            const int distance = symbol - 0x17F;
            const int source = intermediate->size() - distance;
            if (source < 0 || source + 2 > intermediate->size()) {
                *error = QStringLiteral("Invalid PACK2 recent-word reference");
                return false;
            }
            const quint16 value = quint16(uchar(intermediate->at(source))) |
                                  (quint16(uchar(intermediate->at(source + 1))) << 8);
            appendLe16(intermediate, value);
            const int oldPointer = wordPointer;
            --wordPointer;
            if (oldPointer == 0) {
                for (int i = 0; i < 16; ++i) wordTable[size_t(32 + i)] = wordTable[size_t(i)];
                wordPointer = 31;
            }
            wordTable[size_t(wordPointer)] = value;
        } else if (symbol < 0x1A1) {
            const int index = symbol - 0x191;
            const quint16 value = wordTable[size_t(wordPointer + index)];
            appendLe16(intermediate, value);
            for (int i = index - 1; i >= 0; --i)
                wordTable[size_t(wordPointer + i + 1)] = wordTable[size_t(wordPointer + i)];
            wordTable[size_t(wordPointer)] = value;
        } else {
            const int index = symbol - 0x1A1;
            const int oldPosition = tokenTable[size_t(tokenPointer + index)];
            if (oldPosition < 0 || oldPosition >= intermediate->size()) {
                *error = QStringLiteral("Invalid PACK2 recent-token reference");
                return false;
            }
            const uchar tag = uchar(intermediate->at(oldPosition));
            int tokenBytes = 2;
            if (tag == 0x80) tokenBytes = 4;
            else if (tag & 0x40) tokenBytes = 3;
            if (oldPosition + tokenBytes > intermediate->size()) {
                *error = QStringLiteral("Truncated PACK2 recent-token reference");
                return false;
            }
            QByteArray previous = intermediate->mid(oldPosition, tokenBytes);
            intermediate->append(char(0x9E));
            const quint16 newPosition = quint16(intermediate->size());
            intermediate->append(previous);
            for (int i = index - 1; i >= 0; --i)
                tokenTable[size_t(tokenPointer + i + 1)] = tokenTable[size_t(tokenPointer + i)];
            tokenTable[size_t(tokenPointer)] = newPosition;
        }

        if (intermediate->size() > 0xFFFF) {
            *error = QStringLiteral("PACK2 intermediate stream overflow");
            return false;
        }
    }

    if (intermediate->size() != target) {
        *error = QStringLiteral("PACK2 intermediate-size mismatch");
        return false;
    }
    *consumed = 6 + bits.consumed();
    if (*consumed <= 0 || start + *consumed > end) {
        *error = QStringLiteral("Invalid PACK2 frame consumption");
        return false;
    }
    return true;
}

QByteArray initialHistory()
{
    QByteArray result(kHistorySize, '\0');
    std::memset(result.data() + 0xBC62, 0x20, 0x140);
    std::memset(result.data() + 0xBDA2, 0xFF, 0x140);
    std::memset(result.data() + 0xBEE2, 0x00, 0x140);
    std::memcpy(result.data() + kHistorySeedOffset, os2pack2data::kHistorySeed,
                sizeof(os2pack2data::kHistorySeed));
    return result;
}

bool expandLzBlock(const QByteArray& source, const QByteArray& history,
                   QByteArray* output, QString* error)
{
    QByteArray window = history;
    const int base = window.size();
    int position = 0;
    while (position < source.size()) {
        const uchar value = uchar(source.at(position));
        if (value != 0x9E) {
            if (window.size() - base >= kIntermediateBlockLimit) {
                *error = QStringLiteral("PACK2 LZ output overflow"); return false;
            }
            window.append(char(value));
            ++position;
            continue;
        }
        if (position + 1 >= source.size()) {
            *error = QStringLiteral("Truncated PACK2 LZ escape"); return false;
        }
        const uchar tag = uchar(source.at(position + 1));
        if (tag == 0x40) {
            window.append(char(0x9E));
            position += 2;
            continue;
        }

        int length = 0;
        int distanceMinusOne = 0;
        if (tag == 0x80) {
            if (position + 4 >= source.size()) {
                *error = QStringLiteral("Truncated PACK2 long LZ token"); return false;
            }
            length = uchar(source.at(position + 2)) + 0x43;
            distanceMinusOne = uchar(source.at(position + 3)) |
                               (int(uchar(source.at(position + 4))) << 8);
            position += 5;
        } else if (tag & 0x40) {
            if (position + 3 >= source.size()) {
                *error = QStringLiteral("Truncated PACK2 medium LZ token"); return false;
            }
            length = (tag & 0x3F) + 3;
            distanceMinusOne = uchar(source.at(position + 2)) |
                               (int(uchar(source.at(position + 3))) << 8);
            position += 4;
        } else {
            if (position + 2 >= source.size()) {
                *error = QStringLiteral("Truncated PACK2 short LZ token"); return false;
            }
            length = tag + 3;
            distanceMinusOne = uchar(source.at(position + 2));
            position += 3;
        }
        const int distance = distanceMinusOne + 1;
        if (distance > window.size()) {
            *error = QStringLiteral("Invalid PACK2 LZ distance"); return false;
        }
        if (window.size() - base + length > kIntermediateBlockLimit) {
            *error = QStringLiteral("PACK2 LZ output overflow"); return false;
        }
        for (int i = 0; i < length; ++i) window.append(window.at(window.size() - distance));
    }
    *output = window.mid(base);
    return true;
}

} // namespace

bool decompressOs2Pack2(const QByteArray& data, qsizetype start, qsizetype end,
                        quint32 expectedSize, QByteArray* decoded, QString* error)
{
    if (!decoded || !error) return false;
    decoded->clear();
    if (expectedSize > quint32(kMaxPack2Output)) {
        *error = QStringLiteral("PACK2 member exceeds 512 MiB safety limit");
        return false;
    }
    if (start < 0 || end > data.size() || start + 8 > end) {
        *error = QStringLiteral("Truncated PACK2 compression header");
        return false;
    }
    if (data.mid(start + 4, 4) != QByteArrayLiteral("fT19")) {
        *error = QStringLiteral("Unsupported PACK2 compression method");
        return false;
    }

    qsizetype position = start + 8;
    QByteArray history = initialHistory();
    int frameCount = 0;
    while (quint32(decoded->size()) < expectedSize) {
        if (frameCount != 0) {
            if (position + 4 > end || data.mid(position, 4) != QByteArrayLiteral("fT19")) {
                *error = QStringLiteral("Missing PACK2 frame marker");
                return false;
            }
            position += 4;
        }

        QByteArray intermediate;
        qsizetype frameConsumed = 0;
        if (!decodeFrame(data, position, end, &intermediate, &frameConsumed, error)) return false;
        position += frameConsumed;
        ++frameCount;
        if (frameCount > 65536) {
            *error = QStringLiteral("Too many PACK2 frames");
            return false;
        }

        int blockPosition = 0;
        while (blockPosition < intermediate.size()) {
            if (blockPosition + 2 > intermediate.size()) {
                *error = QStringLiteral("Truncated PACK2 block header"); return false;
            }
            const int blockLength = uchar(intermediate.at(blockPosition)) |
                                    (int(uchar(intermediate.at(blockPosition + 1))) << 8);
            blockPosition += 2;
            if (blockLength <= 0 || blockLength > kIntermediateBlockLimit ||
                blockPosition + blockLength > intermediate.size()) {
                *error = QStringLiteral("Invalid PACK2 block length"); return false;
            }
            const QByteArray block = intermediate.mid(blockPosition, blockLength);
            blockPosition += blockLength;
            QByteArray expanded;
            if (uchar(block.at(0)) == 0) {
                expanded = block.mid(1);
            } else if (!expandLzBlock(block.mid(1), history, &expanded, error)) {
                return false;
            }
            if (decoded->size() > int(expectedSize) - expanded.size()) {
                *error = QStringLiteral("PACK2 decoded-size overflow"); return false;
            }
            decoded->append(expanded);
            QByteArray combined = history;
            combined.append(expanded);
            history = combined.right(kHistorySize);
        }
    }

    if (quint32(decoded->size()) != expectedSize) {
        *error = QStringLiteral("PACK2 size mismatch: expected %1, decoded %2")
                     .arg(expectedSize).arg(decoded->size());
        return false;
    }
    return true;
}

} // namespace peare
