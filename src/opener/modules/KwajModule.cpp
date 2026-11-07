#include "KwajModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"

#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <vector>
#include <utility>

namespace peare {
namespace {

constexpr qsizetype kFixedHeaderSize = 14;
constexpr quint32 kMaxDecodedSize = 512u * 1024u * 1024u;
const QByteArray kSignature("KWAJ\x88\xF0\x27\xD1", 8);

quint16 le16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool readBoundedString(const QByteArray& data, qsizetype* position,
                       qsizetype limit, int maximum, QByteArray* value)
{
    value->clear();
    for (int i = 0; i <= maximum; ++i) {
        if (*position >= limit) return false;
        const char ch = data.at((*position)++);
        if (ch == '\0') return true;
        if (i == maximum) return false;
        value->append(ch);
    }
    return false;
}

struct KwajHeader {
    quint16 method = 0;
    quint16 dataOffset = 0;
    quint16 flags = 0;
    bool hasLength = false;
    quint32 length = 0;
    QString name;
};

bool parseHeader(const QByteArray& data, KwajHeader* header, QString* error)
{
    if (data.size() < kFixedHeaderSize || data.left(8) != kSignature) {
        *error = QStringLiteral("Invalid KWAJ signature or truncated header");
        return false;
    }
    header->method = le16(data, 8);
    header->dataOffset = le16(data, 10);
    header->flags = le16(data, 12);
    if (header->method > 4 || (header->flags & 0xFFC0u) != 0 ||
        header->dataOffset < kFixedHeaderSize || header->dataOffset > data.size()) {
        *error = QStringLiteral("Unsupported KWAJ method, flags, or data offset");
        return false;
    }

    qsizetype position = kFixedHeaderSize;
    const qsizetype limit = data.size();
    const auto invalidHeader = [error]() {
        *error = QStringLiteral("Invalid KWAJ optional header");
        return false;
    };
    if ((header->flags & 0x01u) != 0) {
        if (position + 4 > limit) return invalidHeader();
        header->hasLength = true;
        header->length = le32(data, position);
        position += 4;
        if (header->length > kMaxDecodedSize) {
            *error = QStringLiteral("KWAJ declared size exceeds safety limit");
            return false;
        }
    }
    if ((header->flags & 0x02u) != 0) {
        if (position + 2 > limit) return invalidHeader();
        position += 2;
    }
    if ((header->flags & 0x04u) != 0) {
        if (position + 2 > limit) return invalidHeader();
        const quint16 length = le16(data, position);
        position += 2;
        if (position + length > limit) return invalidHeader();
        position += length;
    }
    QByteArray baseName;
    QByteArray extension;
    if ((header->flags & 0x08u) != 0 &&
        !readBoundedString(data, &position, limit, 8, &baseName))
        return invalidHeader();
    if ((header->flags & 0x10u) != 0 &&
        !readBoundedString(data, &position, limit, 3, &extension))
        return invalidHeader();
    if ((header->flags & 0x20u) != 0) {
        if (position + 2 > limit) return invalidHeader();
        const quint16 length = le16(data, position);
        position += 2;
        if (position + length > limit) return invalidHeader();
        position += length;
    }
    if (!baseName.isEmpty()) {
        header->name = QString::fromLatin1(baseName);
        if (!extension.isEmpty())
            header->name += QLatin1Char('.') + QString::fromLatin1(extension);
    }
    return true;

}

class BitReader {
public:
    BitReader(const QByteArray& data, qsizetype start)
        : data_(data), position_(start), bitCount_(0), bits_(0) {}

    bool read(unsigned count, unsigned* value)
    {
        if (count > 24 || !value) return false;
        while (bitCount_ < count) {
            if (position_ >= data_.size()) return false;
            bits_ = (bits_ << 8) | uchar(data_.at(position_++));
            bitCount_ += 8;
        }
        bitCount_ -= count;
        *value = count == 0 ? 0 : unsigned((bits_ >> bitCount_) & ((quint64(1) << count) - 1));
        return true;
    }

private:
    const QByteArray& data_;
    qsizetype position_;
    unsigned bitCount_;
    quint64 bits_;
};

class HuffmanTree {
public:
    bool build(const std::vector<unsigned>& lengths)
    {
        unsigned maximum = 0;
        std::vector<unsigned> counts(16, 0);
        for (unsigned length : lengths) {
            if (length > 15) return false;
            if (length != 0) {
                ++counts[length];
                maximum = std::max(maximum, length);
            }
        }
        if (maximum == 0) return false;
        unsigned code = 0;
        std::vector<unsigned> nextCode(16, 0);
        for (unsigned length = 1; length <= 15; ++length) {
            code = (code + counts[length - 1]) << 1;
            if (code + counts[length] > (1u << length)) return false;
            nextCode[length] = code;
        }
        codes_.clear();
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length != 0)
                codes_[quint32((length << 16) | nextCode[length]++)] = symbol;
        }
        maximumLength_ = maximum;
        return true;
    }

    bool decode(BitReader* input, unsigned* symbol) const
    {
        unsigned code = 0;
        for (unsigned length = 1; length <= maximumLength_; ++length) {
            unsigned bit = 0;
            if (!input->read(1, &bit)) return false;
            code = (code << 1) | bit;
            const auto found = codes_.find(quint32((length << 16) | code));
            if (found != codes_.end()) {
                *symbol = found->second;
                return true;
            }
        }
        return false;
    }

private:
    unsigned maximumLength_ = 0;
    std::map<quint32, unsigned> codes_;
};

bool readTreeLengths(BitReader* input, unsigned type, unsigned count,
                     std::vector<unsigned>* lengths)
{
    lengths->resize(count);
    if (type == 0) {
        unsigned width = 0;
        while ((1u << width) < count) ++width;
        std::fill(lengths->begin(), lengths->end(), width);
        return true;
    }
    if (type == 3) {
        for (unsigned& length : *lengths) {
            if (!input->read(4, &length)) return false;
        }
        return true;
    }

    unsigned previous = 0;
    if (!input->read(4, &previous)) return false;
    (*lengths)[0] = previous;
    for (unsigned index = 1; index < count; ++index) {
        unsigned selector = 0;
        if (type == 1) {
            if (!input->read(1, &selector)) return false;
            if (selector == 0) {
                (*lengths)[index] = previous;
                continue;
            }
            if (!input->read(1, &selector)) return false;
            if (selector == 0) {
                if (previous == 15) return false;
                (*lengths)[index] = ++previous;
                continue;
            }
            if (!input->read(4, &previous)) return false;
        } else {
            if (!input->read(2, &selector)) return false;
            if (selector == 3) {
                if (!input->read(4, &previous)) return false;
            } else {
                const int updated = int(previous) + int(selector) - 1;
                if (updated < 0 || updated > 15) return false;
                previous = unsigned(updated);
            }
        }
        (*lengths)[index] = previous;
    }
    return true;
}

bool decodeLzss(const QByteArray& input, qsizetype start, quint32 expectedSize,
                bool hasSize, int initialOffset, QByteArray* output, QString* error)
{
    std::array<uchar, 4096> dictionary{};
    dictionary.fill(0x20);
    int dictionaryPos = 4096 - initialOffset;
    qsizetype inputPos = start;
    quint16 flags = 0;
    output->clear();
    if (hasSize) output->reserve(int(expectedSize));
    while (!hasSize || quint32(output->size()) < expectedSize) {
        flags >>= 1;
        if ((flags & 0x0100u) == 0) {
            if (inputPos >= input.size()) break;
            flags = quint16(uchar(input.at(inputPos++))) | 0xFF00u;
        }
        if ((flags & 1u) != 0) {
            if (inputPos >= input.size()) {
                *error = QStringLiteral("Truncated KWAJ literal stream");
                return false;
            }
            const uchar value = uchar(input.at(inputPos++));
            output->append(char(value));
            dictionary[dictionaryPos] = value;
            dictionaryPos = (dictionaryPos + 1) & 0x0FFF;
        } else {
            if (inputPos + 2 > input.size()) {
                *error = QStringLiteral("Truncated KWAJ dictionary reference");
                return false;
            }
            const uchar low = uchar(input.at(inputPos++));
            const uchar highLength = uchar(input.at(inputPos++));
            int sourcePos = int(low) | ((int(highLength) & 0xF0) << 4);
            int length = (int(highLength) & 0x0F) + 3;
            if (hasSize) length = std::min(length, int(expectedSize - quint32(output->size())));
            for (int i = 0; i < length; ++i) {
                const uchar value = dictionary[sourcePos];
                sourcePos = (sourcePos + 1) & 0x0FFF;
                output->append(char(value));
                dictionary[dictionaryPos] = value;
                dictionaryPos = (dictionaryPos + 1) & 0x0FFF;
            }
        }
        if (quint32(output->size()) > kMaxDecodedSize) {
            *error = QStringLiteral("KWAJ output exceeds safety limit");
            return false;
        }
    }
    if (hasSize && quint32(output->size()) != expectedSize) {
        *error = QStringLiteral("KWAJ output does not match declared size");
        return false;
    }
    return true;
}

bool decodeLzh(const QByteArray& input, qsizetype start, quint32 expectedSize,
               bool hasSize, QByteArray* output, QString* error)
{
    BitReader bits(input, start);
    std::array<unsigned, 5> types{};
    for (unsigned& type : types) {
        if (!bits.read(4, &type) || type > 3) {
            *error = QStringLiteral("Invalid KWAJ method 3 tree header");
            return false;
        }
    }
    unsigned padding = 0;
    if (!bits.read(4, &padding)) {
        *error = QStringLiteral("Truncated KWAJ method 3 tree header");
        return false;
    }
    const unsigned symbolCounts[5] = {16, 16, 32, 64, 256};
    HuffmanTree trees[5];
    for (unsigned i = 0; i < 5; ++i) {
        std::vector<unsigned> lengths;
        if (!readTreeLengths(&bits, types[i], symbolCounts[i], &lengths) ||
            !trees[i].build(lengths)) {
            *error = QStringLiteral("Invalid KWAJ method 3 Huffman tree");
            return false;
        }
    }

    std::array<uchar, 4096> dictionary{};
    dictionary.fill(0x20);
    int position = 4096 - 17;
    output->clear();
    if (hasSize) output->reserve(int(expectedSize));
    unsigned selectedTree = 0;
    while (!hasSize || quint32(output->size()) < expectedSize) {
        unsigned code = 0;
        if (!trees[selectedTree].decode(&bits, &code)) break;
        if (code != 0) {
            unsigned offsetHigh = 0;
            unsigned offsetLow = 0;
            if (!trees[3].decode(&bits, &offsetHigh) || !bits.read(6, &offsetLow)) {
                *error = QStringLiteral("Truncated KWAJ method 3 match");
                return false;
            }
            const unsigned distance = (offsetHigh << 6) | offsetLow;
            const unsigned length = code + 2;
            for (unsigned i = 0; i < length; ++i) {
                if (hasSize && quint32(output->size()) >= expectedSize) break;
                const int source = (position - int(distance)) & 0x0FFF;
                const uchar value = dictionary[source];
                output->append(char(value));
                dictionary[position] = value;
                position = (position + 1) & 0x0FFF;
            }
            selectedTree = 0;
        } else {
            unsigned literalCount = 0;
            if (!trees[2].decode(&bits, &literalCount)) {
                *error = QStringLiteral("Truncated KWAJ method 3 literal run");
                return false;
            }
            selectedTree = literalCount == 31 ? 0 : 1;
            for (unsigned i = 0; i <= literalCount; ++i) {
                if (hasSize && quint32(output->size()) >= expectedSize) break;
                unsigned symbol = 0;
                if (!trees[4].decode(&bits, &symbol)) {
                    *error = QStringLiteral("Truncated KWAJ method 3 literals");
                    return false;
                }
                output->append(char(symbol));
                dictionary[position] = uchar(symbol);
                position = (position + 1) & 0x0FFF;
            }
        }
        if (quint32(output->size()) > kMaxDecodedSize) {
            *error = QStringLiteral("KWAJ output exceeds safety limit");
            return false;
        }
    }
    if (hasSize && quint32(output->size()) != expectedSize) {
        *error = QStringLiteral("KWAJ output does not match declared size");
        return false;
    }
    return true;
}

bool decodeMszip(const QByteArray& input, qsizetype start, quint32 expectedSize,
                 bool hasSize, QByteArray* output, QString* error)
{
    output->clear();
    if (hasSize) output->reserve(int(expectedSize));
    qsizetype position = start;
    bool terminated = false;
    while (position + 2 <= input.size()) {
        const quint16 blockSize = le16(input, position);
        position += 2;
        if (blockSize == 0) {
            terminated = true;
            break;
        }
        if (blockSize < 2 || position + blockSize > input.size() ||
            input.at(position) != 'C' || input.at(position + 1) != 'K') {
            *error = QStringLiteral("Invalid KWAJ MSZIP block");
            return false;
        }
        if (hasSize && quint32(output->size()) >= expectedSize) {
            *error = QStringLiteral("KWAJ MSZIP stream exceeds declared size");
            return false;
        }
        std::vector<std::uint8_t> decoded;
        const std::size_t historySize = std::min<std::size_t>(output->size(), 32768);
        std::vector<std::uint8_t> history;
        history.reserve(historySize);
        for (std::size_t i = output->size() - historySize; i < std::size_t(output->size()); ++i)
            history.push_back(std::uint8_t(output->at(qsizetype(i))));
        std::string decodeError;
        const auto* packed = reinterpret_cast<const std::uint8_t*>(
            input.constData() + position + 2);
        const std::size_t packedSize = std::size_t(blockSize - 2);
        bool decodedBlock = false;
        if (hasSize) {
            const std::size_t remaining = std::size_t(expectedSize - quint32(output->size()));
            const std::size_t wanted = std::min<std::size_t>(32768u, remaining);
            decodedBlock = compression::inflateRawExact(
                packed, packedSize, wanted, &history, &decoded, &decodeError);
        } else {
            compression::MemoryInflateInput blockInput(packed, packedSize);
            decodedBlock = compression::inflateRawStream(
                blockInput, 32768, &history, &decoded, &decodeError) &&
                blockInput.consumed() == packedSize;
        }
        if (!decodedBlock || decoded.size() > 32768 || decoded.empty()) {
            *error = QStringLiteral("KWAJ MSZIP decompression failed");
            return false;
        }
        position += blockSize;
        output->append(reinterpret_cast<const char*>(decoded.data()), int(decoded.size()));
        if (quint32(output->size()) > kMaxDecodedSize) {
            *error = QStringLiteral("KWAJ output exceeds safety limit");
            return false;
        }
    }
    if (!terminated || position != input.size()) {
        *error = QStringLiteral("Invalid KWAJ MSZIP terminator or trailing data");
        return false;
    }
    if (hasSize && quint32(output->size()) != expectedSize) {
        *error = QStringLiteral("KWAJ output does not match declared size");
        return false;
    }
    return true;
}

} // namespace

std::unique_ptr<KwajModule> KwajModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<KwajModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::KWAJ;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<KwajModule> KwajModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<KwajModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::KWAJ;
    module->info_.description = QStringLiteral("Microsoft KWAJ compressed file");

    KwajHeader header;
    QString error;
    if (!parseHeader(data, &header, &error)) {
        module->info_.error = error;
        return module;
    }

    QByteArray decoded;
    const quint32 expectedSize = header.hasLength ? header.length : 0;
    bool ok = false;
    switch (header.method) {
    case 0:
        decoded = data.mid(header.dataOffset);
        ok = !header.hasLength || quint32(decoded.size()) == expectedSize;
        break;
    case 1:
        decoded = data.mid(header.dataOffset);
        for (char& byte : decoded) byte = char(uchar(byte) ^ 0xFFu);
        ok = !header.hasLength || quint32(decoded.size()) == expectedSize;
        break;
    case 2:
        ok = decodeLzss(data, header.dataOffset, expectedSize, header.hasLength,
                        18, &decoded, &error);
        break;
    case 3:
        ok = decodeLzh(data, header.dataOffset, expectedSize, header.hasLength,
                       &decoded, &error);
        break;
    case 4:
        ok = decodeMszip(data, header.dataOffset, expectedSize, header.hasLength,
                         &decoded, &error);
        break;
    default:
        break;
    }
    if (!ok) {
        module->info_.error = error.isEmpty()
            ? QStringLiteral("KWAJ output does not match declared size") : error;
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("KWAJ_FILE");
    entry.isEmbeddedFile = true;
    entry.name = header.name.isEmpty() ? QFileInfo(logicalName).fileName() : header.name;
    if (entry.name.isEmpty()) entry.name = QStringLiteral("KWAJ");
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = header.dataOffset;
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::KWAJ;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.data = std::move(decoded);
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
