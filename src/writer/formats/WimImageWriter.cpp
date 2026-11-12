#include "WimImageWriter.h"
#include "../../opener/fs/BlockDecompressor.h"
#include "../../opener/compression/peare/lzx_frontends.h"

#include <QCryptographicHash>
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QXmlStreamWriter>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>

namespace peare {
namespace writer {
namespace {

const qint64 kHeaderSize = 208;
const quint32 kDirectoryAttribute = 0x10;
const quint8 kResourceMetadata = 0x02;
const quint8 kResourceCompressed = 0x04;
const quint32 kWimXpress = 0x00020000u;
const quint32 kWimLzx = 0x00040000u;
const quint32 kWimCompression = 0x00000002u;
const quint32 kDefaultChunkSize = 32768;
const quint32 kDefaultIntegrityChunkSize = 10485760;

void put16(QByteArray* out, quint16 value);
void put32(QByteArray* out, quint32 value);
quint32 get32(const QByteArray& in, qsizetype pos);
quint64 get64(const QByteArray& in, qsizetype pos);

class WimBitWriter {
public:
    void write(quint32 value, unsigned count) {
        for (unsigned bit = count; bit != 0; --bit) {
            if ((value >> (bit - 1)) & 1u)
                word_ |= quint16(1u << (15u - used_));
            if (++used_ == 16) {
                put16(&bytes_, word_);
                used_ = 0;
                word_ = 0;
            }
        }
    }

    QByteArray finish() {
        if (used_) {
            put16(&bytes_, word_);
            put16(&bytes_, 0);
        } else {
            put16(&bytes_, 0);
        }
        return bytes_;
    }

private:
    QByteArray bytes_;
    quint16 word_ = 0;
    unsigned used_ = 0;
};

QByteArray encodeXpressLiterals(const QByteArray& input) {
    struct Token {
        quint16 symbol;
        quint16 distanceSuffix;
        quint8 distanceBits;
    };
    struct Node {
        quint64 frequency;
        int left;
        int right;
        int symbol;
    };
    struct QueueItem {
        quint64 frequency;
        quint64 order;
        int node;
        bool operator>(const QueueItem& other) const {
            return frequency != other.frequency ? frequency > other.frequency
                                                : order > other.order;
        }
    };
    std::vector<Token> tokens;
    std::array<quint64, 512> frequencies = {{0}};
    std::unordered_map<quint32, std::vector<std::size_t>> candidates;
    const auto keyAt = [&](std::size_t pos) {
        return quint32(quint8(input.at(qsizetype(pos)))) |
               (quint32(quint8(input.at(qsizetype(pos + 1)))) << 8) |
               (quint32(quint8(input.at(qsizetype(pos + 2)))) << 16);
    };
    std::size_t pos = 0;
    while (pos < std::size_t(input.size())) {
        std::size_t bestLength = 0;
        std::size_t bestDistance = 0;
        if (pos + 2 < std::size_t(input.size())) {
            const quint32 key = keyAt(pos);
            const auto found = candidates.find(key);
            if (found != candidates.end()) {
                unsigned checked = 0;
                for (auto candidate = found->second.rbegin();
                     candidate != found->second.rend() && checked < 64; ++candidate, ++checked) {
                    const std::size_t distance = pos - *candidate;
                    if (distance > 32768) break;
                    std::size_t length = 0;
                    const std::size_t limit = std::min<std::size_t>(17,
                        std::size_t(input.size()) - pos);
                    while (length < limit &&
                           input.at(qsizetype(*candidate + length)) ==
                               input.at(qsizetype(pos + length))) ++length;
                    if (length > bestLength) {
                        bestLength = length;
                        bestDistance = distance;
                        if (length == limit) break;
                    }
                }
            }
        }
        if (bestLength == 3 && bestDistance == 1) bestLength = 0;
        const std::size_t consumed = bestLength >= 3 ? bestLength : 1;
        if (bestLength >= 3) {
            unsigned distanceBits = 0;
            while ((std::size_t(1) << (distanceBits + 1)) <= bestDistance)
                ++distanceBits;
            const quint16 symbol = quint16(256u + distanceBits * 16u +
                                           unsigned(bestLength - 3));
            tokens.push_back(Token{symbol,
                quint16(bestDistance - (std::size_t(1) << distanceBits)),
                quint8(distanceBits)});
            ++frequencies[symbol];
        } else {
            const quint16 symbol = quint8(input.at(qsizetype(pos)));
            tokens.push_back(Token{symbol, 0, 0});
            ++frequencies[symbol];
        }
        for (std::size_t i = 0; i < consumed; ++i) {
            const std::size_t indexed = pos + i;
            if (indexed + 2 < std::size_t(input.size())) {
                std::vector<std::size_t>& list = candidates[keyAt(indexed)];
                list.push_back(indexed);
                if (list.size() > 256) list.erase(list.begin(), list.begin() + 128);
            }
        }
        pos += consumed;
    }
    ++frequencies[256];

    std::vector<Node> nodes;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
    quint64 order = 0;
    for (int symbol = 0; symbol < 512; ++symbol) {
        if (!frequencies[std::size_t(symbol)]) continue;
        nodes.push_back(Node{frequencies[std::size_t(symbol)], -1, -1, symbol});
        queue.push(QueueItem{frequencies[std::size_t(symbol)], order++,
                             int(nodes.size() - 1)});
    }
    if (queue.size() == 1) {
        int dummy = 0;
        while (frequencies[std::size_t(dummy)] != 0) ++dummy;
        nodes.push_back(Node{0, -1, -1, dummy});
        queue.push(QueueItem{0, order++, int(nodes.size() - 1)});
    }
    while (queue.size() > 1) {
        const QueueItem first = queue.top();
        queue.pop();
        const QueueItem second = queue.top();
        queue.pop();
        nodes.push_back(Node{first.frequency + second.frequency,
                             first.node, second.node, -1});
        queue.push(QueueItem{first.frequency + second.frequency, order++,
                             int(nodes.size() - 1)});
    }
    std::array<quint8, 512> lengths = {{0}};
    std::function<void(int, unsigned)> assignLengths = [&](int node, unsigned depth) {
        if (nodes[std::size_t(node)].symbol >= 0) {
            lengths[std::size_t(nodes[std::size_t(node)].symbol)] =
                quint8(depth == 0 ? 1 : depth);
            return;
        }
        assignLengths(nodes[std::size_t(node)].left, depth + 1);
        assignLengths(nodes[std::size_t(node)].right, depth + 1);
    };
    assignLengths(queue.top().node, 0);
    std::array<quint32, 16> counts = {{0}};
    for (quint8 length : lengths) {
        if (length > 15) return QByteArray();
        if (length) ++counts[length];
    }
    std::array<quint32, 16> nextCodes = {{0}};
    quint32 code = 0;
    for (unsigned length = 1; length <= 15; ++length) {
        code = (code + counts[length - 1]) << 1;
        nextCodes[length] = code;
    }
    std::array<quint32, 512> codes = {{0}};
    for (unsigned symbol = 0; symbol < 512; ++symbol)
        if (lengths[symbol]) codes[symbol] = nextCodes[lengths[symbol]]++;

    QByteArray output(256, '\0');
    for (unsigned symbol = 0; symbol < 512; symbol += 2) {
        output[int(symbol / 2)] = char(lengths[symbol] | (lengths[symbol + 1] << 4));
    }
    WimBitWriter bits;
    for (const Token& token : tokens) {
        bits.write(codes[token.symbol], lengths[token.symbol]);
        bits.write(token.distanceSuffix, token.distanceBits);
    }
    bits.write(codes[256], lengths[256]);
    QByteArray payload = bits.finish();
    output.append(payload);
    return output;
}

QByteArray encodeLzxUncompressed(const QByteArray& input) {
    WimBitWriter bits;
    bits.write(3, 3);
    bits.write(0, 1);
    bits.write(quint32(input.size() >> 8), 8);
    bits.write(quint32(input.size() & 0xff), 8);
    QByteArray output = bits.finish();
    put32(&output, 1);
    put32(&output, 1);
    put32(&output, 1);
    output.append(input);
    return output;
}

QByteArray encodeWimChunk(quint32 compressionFlag, const QByteArray& input) {
    if (compressionFlag == kWimXpress) return encodeXpressLiterals(input);
    if (compressionFlag == kWimLzx) return encodeLzxUncompressed(input);
    return input;
}

void put16(QByteArray* out, quint16 value) {
    out->append(char(value & 0xff));
    out->append(char((value >> 8) & 0xff));
}

void put32(QByteArray* out, quint32 value) {
    put16(out, quint16(value));
    put16(out, quint16(value >> 16));
}

void put64(QByteArray* out, quint64 value) {
    put32(out, quint32(value));
    put32(out, quint32(value >> 32));
}

void writeImageTime(QXmlStreamWriter* xml, const QString& element, quint64 value) {
    xml->writeStartElement(element);
    xml->writeTextElement(QStringLiteral("HIGHPART"),
                          QStringLiteral("0x%1").arg(quint32(value >> 32), 8, 16, QLatin1Char('0')));
    xml->writeTextElement(QStringLiteral("LOWPART"),
                          QStringLiteral("0x%1").arg(quint32(value), 8, 16, QLatin1Char('0')));
    xml->writeEndElement();
}

quint16 get16(const QByteArray& in, qsizetype pos) {
    return quint16(quint8(in.at(pos))) |
           (quint16(quint8(in.at(pos + 1))) << 8);
}

quint32 get32(const QByteArray& in, qsizetype pos) {
    return quint32(quint8(in.at(pos))) |
           (quint32(quint8(in.at(pos + 1))) << 8) |
           (quint32(quint8(in.at(pos + 2))) << 16) |
           (quint32(quint8(in.at(pos + 3))) << 24);
}

quint64 get64(const QByteArray& in, qsizetype pos) {
    return quint64(get32(in, pos)) | (quint64(get32(in, pos + 4)) << 32);
}

QByteArray utf16le(const QString& value) {
    QByteArray result;
    result.reserve(value.size() * 2);
    for (QChar unit : value) put16(&result, unit.unicode());
    return result;
}

QString fromUtf16le(const QByteArray& value) {
    QString result;
    result.reserve(value.size() / 2);
    for (qsizetype i = 0; i + 1 < value.size(); i += 2)
        result.append(QChar(get16(value, i)));
    return result;
}

QByteArray resourceHeader(quint64 size, quint64 offset, quint64 originalSize,
                          quint8 flags = 0) {
    QByteArray result;
    put64(&result, (quint64(flags) << 56) | (size & 0x00ffffffffffffffull));
    put64(&result, offset);
    put64(&result, originalSize);
    return result;
}

QByteArray defaultSecurityDescriptor() {
    QByteArray descriptor;
    descriptor.append(char(1));
    descriptor.append(char(0));
    put16(&descriptor, 0x8004);
    put32(&descriptor, 20);
    put32(&descriptor, 32);
    put32(&descriptor, 0);
    put32(&descriptor, 44);
    const QByteArray systemSid = QByteArray::fromHex("010100000000000512000000");
    descriptor.append(systemSid);
    descriptor.append(systemSid);
    descriptor.append(char(2));
    descriptor.append(char(0));
    put16(&descriptor, 28);
    put16(&descriptor, 1);
    put16(&descriptor, 0);
    descriptor.append(char(0));
    descriptor.append(char(0));
    put16(&descriptor, 20);
    put32(&descriptor, 0x001f01ffu);
    descriptor.append(char(1));
    descriptor.append(char(1));
    descriptor.append(QByteArray::fromHex("000000000001"));
    put32(&descriptor, 0);
    return descriptor;
}

bool checkedWrite(QIODevice* out, const QByteArray& data) {
    qint64 offset = 0;
    while (offset < data.size()) {
        const qint64 count = out->write(data.constData() + offset, data.size() - offset);
        if (count <= 0) return false;
        offset += count;
    }
    return true;
}

class WimIntegrityBuilder {
public:
    explicit WimIntegrityBuilder(quint32 chunkSize)
        : chunkSize_(chunkSize), hash_(QCryptographicHash::Sha1) {}

    void add(const char* data, qint64 size) {
        while (size > 0) {
            const qint64 remaining = qint64(chunkSize_) - chunkBytes_;
            const int count = int(qMin<qint64>(size, remaining));
            hash_.addData(data, count);
            data += count;
            size -= count;
            chunkBytes_ += quint32(count);
            if (chunkBytes_ == chunkSize_) {
                hashes_.append(hash_.result());
                hash_.reset();
                chunkBytes_ = 0;
            }
        }
    }

    QByteArray finish() {
        if (chunkBytes_) hashes_.append(hash_.result());
        if (!chunkSize_ || hashes_.size() > int((0xffffffffu - 12u) / 20u))
            return QByteArray();
        QByteArray table;
        put32(&table, quint32(12 + hashes_.size() * 20));
        put32(&table, quint32(hashes_.size()));
        put32(&table, chunkSize_);
        for (const QByteArray& digest : hashes_) table.append(digest);
        return table;
    }

private:
    quint32 chunkSize_;
    quint32 chunkBytes_ = 0;
    QCryptographicHash hash_;
    QList<QByteArray> hashes_;
};

bool writeTracked(QIODevice* output, const char* data, qint64 size,
                  WimIntegrityBuilder* integrity) {
    qint64 offset = 0;
    while (offset < size) {
        const qint64 count = output->write(data + offset, size - offset);
        if (count <= 0) return false;
        if (integrity) integrity->add(data + offset, count);
        offset += count;
    }
    return true;
}

bool writeTracked(QIODevice* output, const QByteArray& data,
                  WimIntegrityBuilder* integrity) {
    return writeTracked(output, data.constData(), data.size(), integrity);
}

bool validateWimIntegrity(const QString& path, const QByteArray& header,
                          quint32* chunkSize, QString* error) {
    const quint64 storedSize = get64(header, 124) & 0x00ffffffffffffffull;
    const quint8 flags = quint8(header.at(131));
    const quint64 offset = get64(header, 132);
    const quint64 originalSize = get64(header, 140);
    *chunkSize = 0;
    if (!storedSize && !offset && !originalSize && !flags) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || flags || storedSize < 12 ||
        storedSize != originalSize || storedSize > quint64(std::numeric_limits<int>::max()) ||
        offset > quint64(file.size()) || storedSize > quint64(file.size()) - offset) {
        if (error) *error = QStringLiteral("The WIM integrity resource is invalid.");
        return false;
    }
    const quint64 tableSize = get64(header, 48) & 0x00ffffffffffffffull;
    const quint64 tableOffset = get64(header, 56);
    if (tableOffset > quint64(file.size()) ||
        tableSize > quint64(file.size()) - tableOffset ||
        tableOffset + tableSize < quint64(kHeaderSize)) {
        if (error) *error = QStringLiteral("The WIM lookup table bounds are invalid.");
        return false;
    }
    if (!file.seek(qint64(offset))) return false;
    const QByteArray table = file.read(qint64(storedSize));
    if (table.size() != qint64(storedSize)) return false;
    const quint32 declaredSize = get32(table, 0);
    const quint32 count = get32(table, 4);
    const quint32 declaredChunkSize = get32(table, 8);
    const quint64 checkedBytes = tableOffset + tableSize - quint64(kHeaderSize);
    if (declaredSize != storedSize || declaredChunkSize == 0 ||
        quint64(declaredSize) != 12ull + quint64(count) * 20ull ||
        (checkedBytes + declaredChunkSize - 1) / declaredChunkSize != count ||
        !file.seek(kHeaderSize)) {
        if (error) *error = QStringLiteral("The WIM integrity table structure is invalid.");
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    quint64 remaining = checkedBytes;
    for (quint32 index = 0; index < count; ++index) {
        const quint64 chunkLength = qMin<quint64>(remaining, declaredChunkSize);
        QCryptographicHash hash(QCryptographicHash::Sha1);
        quint64 chunkRemaining = chunkLength;
        while (chunkRemaining) {
            const qint64 request = qint64(qMin<quint64>(chunkRemaining,
                quint64(buffer.size())));
            const qint64 read = file.read(buffer.data(), request);
            if (read != request) {
                if (error) *error = QStringLiteral("The WIM integrity data is truncated.");
                return false;
            }
            hash.addData(buffer.constData(), int(read));
            chunkRemaining -= quint64(read);
        }
        if (hash.result() != table.mid(12 + int(index) * 20, 20)) {
            if (error) *error = QStringLiteral("The WIM integrity check failed.");
            return false;
        }
        remaining -= chunkLength;
    }
    *chunkSize = declaredChunkSize;
    return true;
}

QByteArray hashFile(const QString& path, bool* ok) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return QByteArray();
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!file.atEnd()) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0) {
            *ok = false;
            return QByteArray();
        }
        if (count == 0) break;
        hash.addData(buffer.constData(), int(count));
    }
    *ok = true;
    return hash.result();
}

bool extractWimResource(QFile* source, quint64 offset, quint64 storedSize,
                        quint64 originalSize, quint8 flags, quint32 compressionFlag,
                        quint32 chunkSize, QIODevice* destination) {
    if (!source || !destination || offset > quint64(source->size()) ||
        storedSize > quint64(source->size()) - offset ||
        originalSize > quint64(std::numeric_limits<qint64>::max())) return false;
    if (!(flags & kResourceCompressed)) {
        if (storedSize != originalSize || !source->seek(qint64(offset))) return false;
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        quint64 remaining = storedSize;
        while (remaining) {
            const qint64 count = source->read(buffer.data(), qint64(qMin<quint64>(
                quint64(buffer.size()), remaining)));
            if (count <= 0 || destination->write(buffer.constData(), count) != count)
                return false;
            remaining -= quint64(count);
        }
        return true;
    }
    if ((compressionFlag != kWimXpress && compressionFlag != kWimLzx) ||
        chunkSize < 32768 || chunkSize > 65536 ||
        (chunkSize & (chunkSize - 1)) != 0 || originalSize == 0) return false;
    const quint64 chunkCount = (originalSize + chunkSize - 1) / chunkSize;
    const quint64 tableSize = (chunkCount - 1) * 4;
    if (tableSize > storedSize || tableSize > quint64(std::numeric_limits<int>::max()) ||
        !source->seek(qint64(offset))) return false;
    const QByteArray offsets = source->read(qint64(tableSize));
    if (offsets.size() != qint64(tableSize)) return false;
    peare::fs::XpressHuffmanDecompressor xpress;
    peare_lzx_wim_decoder* lzx = nullptr;
    if (compressionFlag == kWimLzx &&
        peare_lzx_wim_create(chunkSize, &lzx) != PEARE_LZX_OK) return false;
    bool ok = true;
    quint64 previousOffset = 0;
    for (quint64 chunk = 0; chunk < chunkCount && ok; ++chunk) {
        const quint64 start = chunk == 0 ? 0 : get32(offsets, qsizetype((chunk - 1) * 4));
        const quint64 end = chunk + 1 == chunkCount
            ? storedSize - tableSize
            : get32(offsets, qsizetype(chunk * 4));
        if (start < previousOffset || end <= start || end > storedSize - tableSize) {
            ok = false;
            break;
        }
        const quint64 compressedLength = end - start;
        const quint64 targetLength = qMin<quint64>(chunkSize,
            originalSize - chunk * chunkSize);
        if (compressedLength > quint64(std::numeric_limits<int>::max()) ||
            targetLength > quint64(std::numeric_limits<int>::max()) ||
            !source->seek(qint64(offset + tableSize + start))) {
            ok = false;
            break;
        }
        const QByteArray compressed = source->read(qint64(compressedLength));
        if (compressed.size() != qint64(compressedLength)) {
            ok = false;
            break;
        }
        QByteArray decoded(int(targetLength), '\0');
        if (compressedLength == targetLength) {
            decoded = compressed;
        } else {
            std::size_t written = 0;
            const bool decodedOk = compressionFlag == kWimXpress
                ? xpress.tryDecompress(
                    reinterpret_cast<const std::uint8_t*>(compressed.constData()),
                    std::size_t(compressed.size()),
                    reinterpret_cast<std::uint8_t*>(decoded.data()),
                    std::size_t(decoded.size()), written)
                : peare_lzx_wim_decompress(
                    lzx, compressed.constData(), std::size_t(compressed.size()),
                    decoded.data(), std::size_t(decoded.size())) == PEARE_LZX_OK;
            if (!decodedOk || (compressionFlag == kWimXpress && written != targetLength)) {
                ok = false;
                break;
            }
        }
        if (destination->write(decoded) != decoded.size()) ok = false;
        previousOffset = end;
    }
    if (lzx) peare_lzx_wim_destroy(lzx);
    return ok;
}

QString normalizedPath(const QByteArray& name) {
    QString path = QString::fromUtf8(name);
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    return path;
}

QString keyFor(const QByteArray& name) {
    return normalizedPath(name).toCaseFolded();
}

bool validEntryName(const QByteArray& name) {
    const QString path = normalizedPath(name);
    if (path.isEmpty() || path.contains(QChar(0)) || path.startsWith(QLatin1Char('/')))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& part : parts)
        if (part.isEmpty() || part == QStringLiteral(".") ||
            part == QStringLiteral("..") || part.contains(QLatin1Char(':')))
            return false;
    return true;
}

struct Node {
    QString name;
    int entry = -1;
    bool directory = false;
    std::map<QString, Node> children;
};

void appendDirectoryEntries(const Node& node, QByteArray* metadata,
                            const std::vector<QByteArray>& hashes,
                            const std::vector<quint64>& sizes,
                            const std::vector<QByteArray>& shortNames,
                            const std::vector<QByteArray>& streamData,
                            const std::vector<quint16>& streamCounts,
                            const std::vector<quint32>& securityIds,
                            quint32 defaultSecurityId,
                            quint64 baseOffset) {
    Q_UNUSED(baseOffset);
    std::vector<std::pair<const Node*, int>> subdirectories;
    for (const auto& childPair : node.children) {
        const Node& child = childPair.second;
        const bool directory = child.directory || !child.children.empty() || child.entry < 0;
        const QByteArray name = utf16le(child.name);
        const std::size_t index = child.entry >= 0 ? std::size_t(child.entry) : 0;
        const QByteArray shortName = child.entry >= 0 ? shortNames[index] : QByteArray();
        const QByteArray streams = child.entry >= 0 ? streamData[index] : QByteArray();
        const quint16 streamCount = child.entry >= 0 ? streamCounts[index] : 0;
        const quint64 recordSize = quint64((102 + shortName.size() + name.size() + 7) & ~7) + 8;
        const int recordStart = metadata->size();
        put64(metadata, recordSize);
        put32(metadata, directory ? kDirectoryAttribute : 0x20);
        put32(metadata, child.entry >= 0
            ? securityIds[std::size_t(child.entry)] : defaultSecurityId);
        put64(metadata, 0);
        put64(metadata, 0);
        put64(metadata, 0);
        const quint64 fileTime = 0;
        put64(metadata, fileTime);
        put64(metadata, fileTime);
        put64(metadata, fileTime);
        if (child.entry >= 0 && !directory) metadata->append(hashes[std::size_t(child.entry)]);
        else metadata->append(QByteArray(20, '\0'));
        put32(metadata, 0);
        put64(metadata, 0);
        put16(metadata, streamCount);
        put16(metadata, quint16(shortName.size()));
        put16(metadata, quint16(name.size()));
        metadata->append(shortName);
        metadata->append(name);
        while (metadata->size() < recordStart + qint64(recordSize))
            metadata->append('\0');
        metadata->append(streams);
        if (directory) subdirectories.push_back(std::make_pair(&child, recordStart + 16));
        Q_UNUSED(sizes);
    }
    put64(metadata, 0);
    for (const auto& subdirectory : subdirectories) {
        const quint64 childOffset = quint64(metadata->size());
        for (int i = 0; i < 8; ++i)
            (*metadata)[subdirectory.second + i] = char((childOffset >> (8 * i)) & 0xff);
            appendDirectoryEntries(*subdirectory.first, metadata, hashes, sizes,
                                   shortNames, streamData, streamCounts,
                                   securityIds, defaultSecurityId, baseOffset);
    }
}

void countNodes(const Node& node, quint64* directories, quint64* files) {
    for (const auto& pair : node.children) {
        const Node& child = pair.second;
        if (child.directory || child.entry < 0 || !child.children.empty()) ++*directories;
        else ++*files;
        countNodes(child, directories, files);
    }
}

QByteArray updateWimXml(const QByteArray& source, quint64 directoryCount,
                        quint64 fileCount, quint64 imageBytes,
                        quint64 containerBytes) {
    QXmlStreamReader reader(source);
    QByteArray utf8;
    QXmlStreamWriter writer(&utf8);
    bool inFirstImage = false;
    bool foundImage = false;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = reader.name().toString();
            const auto attributes = reader.attributes();
            if (name == QStringLiteral("IMAGE") &&
                attributes.value(QStringLiteral("INDEX")) == QStringLiteral("1")) {
                inFirstImage = true;
                foundImage = true;
            }
            writer.writeStartElement(name);
            for (const QXmlStreamAttribute& attribute : attributes)
                writer.writeAttribute(attribute.name().toString(), attribute.value().toString());
            if (name == QStringLiteral("DIRCOUNT") && inFirstImage) {
                writer.writeCharacters(QString::number(directoryCount));
                writer.writeEndElement();
                reader.skipCurrentElement();
            } else if (name == QStringLiteral("FILECOUNT") && inFirstImage) {
                writer.writeCharacters(QString::number(fileCount));
                writer.writeEndElement();
                reader.skipCurrentElement();
            } else if (name == QStringLiteral("TOTALBYTES")) {
                writer.writeCharacters(QString::number(inFirstImage ? imageBytes : containerBytes));
                writer.writeEndElement();
                reader.skipCurrentElement();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader.name() == QStringLiteral("IMAGE") && inFirstImage)
                inFirstImage = false;
            writer.writeEndElement();
        } else if (token == QXmlStreamReader::Characters) {
            if (!reader.isWhitespace()) writer.writeCharacters(reader.text().toString());
        } else if (token == QXmlStreamReader::Comment) {
            writer.writeComment(reader.text().toString());
        }
    }
    if (reader.hasError() || !foundImage) return QByteArray();
    QByteArray result;
    put16(&result, 0xfeff);
    result.append(utf16le(QString::fromUtf8(utf8)));
    return result;
}

}  // namespace

WimImageWriter::WimImageWriter() = default;
WimImageWriter::~WimImageWriter() { abort(); }

bool WimImageWriter::beginCreate(const QString& outputPath, const QByteArray& variant,
                                 QString* error) {
    if (active_ || outputPath.isEmpty()) {
        if (error) *error = QStringLiteral("A WIM output path is required.");
        return false;
    }
    if (variant == "xpress") {
        compressionFlag_ = kWimXpress;
        chunkSize_ = kDefaultChunkSize;
    } else if (variant == "lzx") {
        compressionFlag_ = kWimLzx;
        chunkSize_ = kDefaultChunkSize;
    } else if (variant.isEmpty() || variant == "store") {
        compressionFlag_ = 0;
        chunkSize_ = 0;
    } else {
        if (error) *error = QStringLiteral("The requested WIM compression variant is not supported.");
        return false;
    }
    staging_.reset(new QTemporaryDir);
    if (!staging_->isValid()) {
        staging_.reset();
        if (error) *error = QStringLiteral("Could not create WIM staging storage.");
        return false;
    }
    outputPath_ = outputPath;
    entries_.clear();
    sourceHeader_.clear();
    sourceSecurityTable_.clear();
    sourceXml_.clear();
    sourceResources_.clear();
    selectedMetadataIndex_ = 0;
    imageCount_ = 1;
    nextStagedFile_ = 0;
    integrityChunkSize_ = 0;
    active_ = true;
    return true;
}

bool WimImageWriter::canUpdate(const QString& imagePath) {
    QFile file(imagePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray header = file.read(kHeaderSize);
    if (header.size() != kHeaderSize || header.left(8) != QByteArray("MSWIM\0\0\0", 8))
        return false;
    const quint32 flags = get32(header, 16);
    const quint32 compression = flags & (kWimXpress | kWimLzx);
    const quint32 chunkSize = get32(header, 20);
    return (flags & ~(0x00000080u | kWimCompression | kWimXpress | kWimLzx)) == 0 &&
           (compression == 0 || compression == kWimXpress || compression == kWimLzx) &&
           ((compression == 0 && !(flags & kWimCompression) && chunkSize == 0) ||
            (compression != 0 && (flags & kWimCompression) &&
             chunkSize == kDefaultChunkSize)) &&
           get16(header, 40) == 1 && get16(header, 42) == 1 && get32(header, 44) > 0;
}

bool WimImageWriter::beginUpdate(const QString& imagePath, QString* error) {
    if (active_ || !canUpdate(imagePath)) {
        if (error) *error = QStringLiteral("This WIM is not a supported single-image store WIM.");
        return false;
    }
    staging_.reset(new QTemporaryDir);
    if (!staging_->isValid()) {
        staging_.reset();
        if (error) *error = QStringLiteral("Could not create WIM update staging.");
        return false;
    }
    QFile sourceHeader(imagePath);
    if (!sourceHeader.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = sourceHeader.read(kHeaderSize);
    if (header.size() != kHeaderSize ||
        !validateWimIntegrity(imagePath, header, &integrityChunkSize_, error)) {
        staging_.reset();
        return false;
    }
    sourceHeader_ = header;
    imageCount_ = get32(header, 44);
    compressionFlag_ = get32(header, 16) & (kWimXpress | kWimLzx);
    chunkSize_ = get32(header, 20);
    if (!loadExisting(imagePath, error)) {
        staging_.reset();
        return false;
    }
    outputPath_ = imagePath;
    sourcePath_ = imagePath;
    active_ = true;
    return true;
}

bool WimImageWriter::loadExisting(const QString& imagePath, QString* error) {
    QFile input(imagePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = input.errorString();
        return false;
    }
    const QByteArray header = input.read(kHeaderSize);
    if (header.size() != kHeaderSize) return false;
    const quint64 xmlSize = get64(header, 72) & 0x00ffffffffffffffull;
    const quint64 xmlOffset = get64(header, 80);
    const quint64 xmlOriginalSize = get64(header, 88) & 0x00ffffffffffffffull;
    const quint8 xmlFlags = quint8(header.at(79));
    if (xmlOriginalSize > quint64(std::numeric_limits<int>::max()) ||
        xmlOffset > quint64(input.size()) || xmlSize > quint64(input.size()) - xmlOffset)
        return false;
    QBuffer xmlBuffer;
    if (!xmlBuffer.open(QIODevice::WriteOnly) ||
        !extractWimResource(&input, xmlOffset, xmlSize, xmlOriginalSize,
                            xmlFlags, compressionFlag_, chunkSize_, &xmlBuffer))
        return false;
    sourceXml_ = xmlBuffer.data();
    const quint64 tableSize = get64(header, 48) & 0x00ffffffffffffffull;
    const quint64 tableOffset = get64(header, 56);
    if (tableSize > quint64(std::numeric_limits<int>::max()) ||
        tableOffset > quint64(input.size()) || tableSize > quint64(input.size()) - tableOffset) {
        if (error) *error = QStringLiteral("The WIM lookup table is out of bounds.");
        return false;
    }
    if (!input.seek(qint64(tableOffset))) return false;
    const QByteArray table = input.read(qint64(tableSize));
    if (table.size() != qint64(tableSize) || table.size() % 50 != 0) return false;
    std::map<QByteArray, ExistingResource> resources;
    ExistingResource metadataResource;
    bool haveMetadata = false;
    sourceResources_.clear();
    for (qsizetype pos = 0; pos + 50 <= table.size(); pos += 50) {
        const quint64 size = get64(table, pos) & 0x00ffffffffffffffull;
        const quint64 offset = get64(table, pos + 8);
        const quint64 originalSize = get64(table, pos + 16);
        const quint8 flags = quint8(table.at(pos + 7));
        ExistingResource resource;
        resource.hash = table.mid(pos + 30, 20);
        resource.tableRecord = table.mid(pos, 50);
        resource.size = size;
        resource.offset = offset;
        resource.originalSize = originalSize;
        resource.flags = flags;
        resource.refs = get32(table, pos + 26);
        if (flags & kResourceMetadata) {
            if (!haveMetadata) {
                metadataResource = resource;
                selectedMetadataIndex_ = sourceResources_.size();
                haveMetadata = true;
            }
        } else {
            resources.emplace(resource.hash, resource);
        }
        sourceResources_.push_back(resource);
    }
    const auto inBounds = [&](const ExistingResource& resource) {
        return resource.offset <= quint64(input.size()) &&
               resource.size <= quint64(input.size()) - resource.offset;
    };
    if (!haveMetadata || !inBounds(metadataResource) ||
        metadataResource.originalSize > quint64(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("The WIM metadata resource is compressed or invalid.");
        return false;
    }
    QByteArray metadata;
    QBuffer metadataBuffer(&metadata);
    if (!metadataBuffer.open(QIODevice::WriteOnly) ||
        !extractWimResource(&input, metadataResource.offset, metadataResource.size,
                            metadataResource.originalSize, metadataResource.flags,
                            compressionFlag_, chunkSize_, &metadataBuffer) ||
        metadata.size() != qint64(metadataResource.originalSize) || metadata.size() < 16)
        return false;
    const quint32 securityLength = get32(metadata, 0);
    if (securityLength > quint32(metadata.size())) return false;
    sourceSecurityTable_ = metadata.left(int(securityLength));
    if (securityLength == 8 && get32(metadata, 4) == 0)
        sourceSecurityTable_.clear();
    const quint64 rootOffset = (quint64(securityLength) + 7) & ~quint64(7);
    if (securityLength < 8 || rootOffset + 102 > quint64(metadata.size())) {
        if (error) *error = QStringLiteral("The WIM metadata root is out of bounds.");
        return false;
    }
    const quint64 firstDirectory = get64(metadata, qsizetype(rootOffset + 16));
    if (firstDirectory >= quint64(metadata.size())) return false;
    std::vector<quint64> activeDirectories;
    std::function<bool(quint64, const QString&)> readDirectory;
    readDirectory = [&](quint64 directoryOffset, const QString& parent) -> bool {
        if (directoryOffset >= quint64(metadata.size()) ||
            std::find(activeDirectories.begin(), activeDirectories.end(), directoryOffset) !=
                activeDirectories.end()) return false;
        activeDirectories.push_back(directoryOffset);
        quint64 pos = directoryOffset;
        while (pos + 8 <= quint64(metadata.size())) {
            const quint64 length = get64(metadata, qsizetype(pos));
            if (length == 0) break;
            if (length < 102 || pos + length > quint64(metadata.size())) return false;
            const quint32 attributes = get32(metadata, qsizetype(pos + 8));
            const quint64 childOffset = get64(metadata, qsizetype(pos + 16));
            const quint64 creationTime = get64(metadata, qsizetype(pos + 32));
            const quint64 accessTime = get64(metadata, qsizetype(pos + 40));
            const quint64 writeTime = get64(metadata, qsizetype(pos + 48));
            QByteArray hash = metadata.mid(qsizetype(pos + 64), 20);
            const quint16 streamCount = get16(metadata, qsizetype(pos + 96));
            const quint16 nameLength = get16(metadata, qsizetype(pos + 100));
            const quint16 shortNameLength = get16(metadata, qsizetype(pos + 98));
            const quint64 nameOffset = pos + 102 + shortNameLength;
            if ((nameLength & 1) || (shortNameLength & 1) ||
                nameOffset + nameLength > pos + length) return false;
            Entry entry;
            entry.shortName = metadata.mid(qsizetype(pos + 102), shortNameLength);
            entry.streamCount = streamCount;
            quint64 streamOffset = pos + length;
            const quint64 streamDataOffset = streamOffset;
            for (quint16 streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
                if (streamOffset + 38 > quint64(metadata.size())) return false;
                const quint64 streamLength = get64(metadata, qsizetype(streamOffset));
                const quint16 streamNameLength = get16(metadata, qsizetype(streamOffset + 36));
                if (streamLength < 38 || streamLength >
                    quint64(metadata.size()) - streamOffset ||
                    (streamNameLength & 1) || quint64(38) + streamNameLength > streamLength)
                    return false;
                const QString streamName = fromUtf16le(metadata.mid(
                    qsizetype(streamOffset + 38), streamNameLength));
                if (streamIndex == 0 && streamName.isEmpty() &&
                    !(attributes & kDirectoryAttribute)) {
                    entry.primaryStreamHashOffset = int(streamOffset - streamDataOffset + 16);
                    hash = metadata.mid(qsizetype(streamOffset + 16), 20);
                } else {
                    entry.alternateStreamHashes.push_back(
                        metadata.mid(qsizetype(streamOffset + 16), 20));
                }
                streamOffset += streamLength;
            }
            entry.streamData = metadata.mid(qsizetype(streamDataOffset),
                                             qsizetype(streamOffset - streamDataOffset));
            const QString component = fromUtf16le(metadata.mid(qsizetype(nameOffset), nameLength));
            const QString path = parent.isEmpty() ? component : parent + QLatin1Char('/') + component;
            if (component.isEmpty() || !validEntryName(path.toUtf8())) return false;
            entry.name = path.toUtf8();
            entry.directory = (attributes & kDirectoryAttribute) != 0;
            entry.attributes = attributes;
            entry.securityId = sourceSecurityTable_.isEmpty()
                ? 0 : get32(metadata, qsizetype(pos + 12));
            entry.fromExisting = true;
            entry.creationTime = creationTime;
            entry.accessTime = accessTime;
            entry.writeTime = writeTime;
            if (!entry.directory) {
                entry.originalHash = hash;
                auto resource = resources.find(hash);
                if (resource != resources.end()) {
                    if (!inBounds(resource->second)) return false;
                    const QString stagedPath = staging_->filePath(
                        QStringLiteral("entry-%1.bin").arg(nextStagedFile_++));
                    QFile staged(stagedPath);
                    if (!staged.open(QIODevice::WriteOnly) ||
                        !extractWimResource(&input, resource->second.offset,
                            resource->second.size, resource->second.originalSize,
                            resource->second.flags, compressionFlag_, chunkSize_, &staged))
                        return false;
                    entry.stagedPath = stagedPath;
                } else if (hash != QByteArray(20, '\0')) {
                    return false;
                }
            }
            entries_.push_back(std::move(entry));
            if ((attributes & kDirectoryAttribute) && childOffset &&
                !readDirectory(childOffset, path)) return false;
            pos = streamOffset;
        }
        activeDirectories.pop_back();
        return true;
    };
    if (!readDirectory(firstDirectory, QString())) {
        entries_.clear();
        if (error) *error = QStringLiteral("The WIM metadata contains unsupported or invalid entries.");
        return false;
    }
    return true;
}

bool WimImageWriter::stageFile(const QString& sourcePath, QString* stagedPath,
                               QString* error) {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    const QString path = staging_->filePath(QStringLiteral("entry-%1.bin").arg(nextStagedFile_++));
    QFile target(path);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error) *error = target.errorString();
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0 || (count > 0 && target.write(buffer.constData(), count) != count)) {
            if (error) *error = source.errorString().isEmpty() ? target.errorString() : source.errorString();
            return false;
        }
    }
    if (stagedPath) *stagedPath = path;
    return true;
}

bool WimImageWriter::addFile(const QString& sourcePath, const QByteArray& entryName,
                             QString* error) {
    if (!active_ || !validEntryName(entryName)) {
        if (error) *error = QStringLiteral("The WIM entry name is invalid.");
        return false;
    }
    const QString key = keyFor(entryName);
    for (const Entry& entry : entries_)
        if (keyFor(entry.name) == key) {
            if (error) *error = QStringLiteral("The WIM entry already exists.");
            return false;
        }
    QString staged;
    if (!stageFile(sourcePath, &staged, error)) return false;
    Entry entry;
    entry.name = normalizedPath(entryName).toUtf8();
    entry.stagedPath = staged;
    entries_.push_back(std::move(entry));
    return true;
}

bool WimImageWriter::addEmptyFile(const QByteArray& entryName, QString* error) {
    if (!active_ || !validEntryName(entryName)) return false;
    const QString key = keyFor(entryName);
    for (const Entry& entry : entries_)
        if (keyFor(entry.name) == key) {
            if (error) *error = QStringLiteral("The WIM entry already exists.");
            return false;
        }
    const QString staged = staging_->filePath(QStringLiteral("entry-%1.bin").arg(nextStagedFile_++));
    QFile file(staged);
    if (!file.open(QIODevice::WriteOnly)) return false;
    Entry entry;
    entry.name = normalizedPath(entryName).toUtf8();
    entry.stagedPath = staged;
    entries_.push_back(std::move(entry));
    return true;
}

bool WimImageWriter::addDirectory(const QByteArray& entryName, QString* error) {
    if (!active_ || !validEntryName(entryName)) return false;
    const QString key = keyFor(entryName);
    for (const Entry& entry : entries_)
        if (keyFor(entry.name) == key) {
            if (entry.directory) return true;
            if (error) *error = QStringLiteral("The WIM entry already exists as a file.");
            return false;
        }
    Entry entry;
    entry.name = normalizedPath(entryName).toUtf8();
    entry.directory = true;
    entries_.push_back(std::move(entry));
    return true;
}

bool WimImageWriter::replaceFile(const QString& sourcePath, const QByteArray& entryName,
                                QString* error) {
    if (!active_) return false;
    const QString key = keyFor(entryName);
    for (Entry& entry : entries_)
        if (!entry.directory && keyFor(entry.name) == key)
            return stageFile(sourcePath, &entry.stagedPath, error);
    if (error) *error = QStringLiteral("The WIM file to replace was not found.");
    return false;
}

bool WimImageWriter::renameEntry(const QByteArray& oldName, const QByteArray& newName,
                                QString* error) {
    if (!active_ || !validEntryName(newName)) return false;
    const QString oldKey = keyFor(oldName);
    const QString newKey = keyFor(newName);
    bool found = false;
    for (const Entry& entry : entries_) {
        const QString key = keyFor(entry.name);
        if (key == newKey && key != oldKey) {
            if (error) *error = QStringLiteral("The WIM destination already exists.");
            return false;
        }
        if (key == oldKey) found = true;
    }
    if (!found) {
        if (error) *error = QStringLiteral("The WIM entry to rename was not found.");
        return false;
    }
    const QString oldPath = normalizedPath(oldName);
    const QString newPath = normalizedPath(newName);
    const QString oldPrefix = oldPath + QLatin1Char('/');
    for (Entry& entry : entries_) {
        const QString path = normalizedPath(entry.name);
        if (path.compare(oldPath, Qt::CaseInsensitive) == 0)
            entry.name = newPath.toUtf8();
        else if (path.startsWith(oldPrefix, Qt::CaseInsensitive))
            entry.name = (newPath + path.mid(oldPath.size())).toUtf8();
    }
    return true;
}

bool WimImageWriter::deleteEntry(const QByteArray& entryName, bool recursive,
                                QString* error) {
    if (!active_) return false;
    const QString path = normalizedPath(entryName);
    const QString prefix = path + QLatin1Char('/');
    bool found = false;
    for (const Entry& entry : entries_) {
        const QString current = normalizedPath(entry.name);
        if (current.compare(path, Qt::CaseInsensitive) == 0) found = true;
        else if (current.startsWith(prefix, Qt::CaseInsensitive)) {
            if (!recursive) {
                if (error) *error = QStringLiteral("The WIM directory is not empty.");
                return false;
            }
            found = true;
        }
    }
    if (!found) {
        if (error) *error = QStringLiteral("The WIM entry was not found.");
        return false;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        const QString current = normalizedPath(entry.name);
        return current.compare(path, Qt::CaseInsensitive) == 0 ||
               (recursive && current.startsWith(prefix, Qt::CaseInsensitive));
    }), entries_.end());
    return true;
}

bool WimImageWriter::writeImage(QString* error) {
    QSaveFile output(outputPath_);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = output.errorString();
        return false;
    }
    QByteArray placeholder(kHeaderSize, '\0');
    if (!checkedWrite(&output, placeholder)) return false;
    WimIntegrityBuilder integrity(integrityChunkSize_);
    WimIntegrityBuilder* integrityOutput = integrityChunkSize_ ? &integrity : nullptr;

    struct Resource {
        QByteArray hash;
        QByteArray tableRecord;
        quint64 size = 0;
        quint64 originalSize = 0;
        quint64 offset = 0;
        quint32 refs = 1;
        quint8 flags = 0;
        int entry = -1;
    };
    std::vector<Resource> resources;
    std::vector<QByteArray> hashes(entries_.size());
    std::vector<quint64> sizes(entries_.size());
    std::vector<QByteArray> shortNames(entries_.size());
    std::vector<QByteArray> streamData(entries_.size());
    std::vector<quint16> streamCounts(entries_.size());
    std::vector<quint32> securityIds(entries_.size(), 0);
    std::map<QByteArray, int> resourceByHash;
    std::map<quint64, quint64> offsetMap;
    QFile oldImage(sourcePath_);
    if (!sourceResources_.empty() && !oldImage.open(QIODevice::ReadOnly)) {
        if (error) *error = oldImage.errorString();
        return false;
    }
    for (std::size_t i = 0; i < sourceResources_.size(); ++i) {
        if (i == selectedMetadataIndex_) continue;
        const ExistingResource& old = sourceResources_[i];
        if (old.offset > quint64(oldImage.size()) ||
            old.size > quint64(oldImage.size()) - old.offset ||
            !oldImage.seek(qint64(old.offset))) return false;
        Resource resource;
        resource.hash = old.hash;
        resource.tableRecord = old.tableRecord;
        resource.size = old.size;
        resource.originalSize = old.originalSize;
        resource.offset = quint64(output.pos());
        resource.refs = old.refs;
        resource.flags = old.flags;
        QByteArray encodedOffset;
        put64(&encodedOffset, resource.offset);
        resource.tableRecord.replace(8, 8, encodedOffset);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        quint64 remaining = old.size;
        while (remaining) {
            const qint64 count = qint64(qMin<quint64>(remaining,
                quint64(buffer.size())));
            const QByteArray bytes = oldImage.read(count);
            if (bytes.size() != count || !writeTracked(&output, bytes, integrityOutput))
                return false;
            remaining -= quint64(count);
        }
        offsetMap[old.offset] = resource.offset;
        if (!(old.flags & kResourceMetadata))
            resourceByHash.emplace(old.hash, int(resources.size()));
        resources.push_back(std::move(resource));
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        securityIds[i] = entries_[i].securityId;
        shortNames[i] = entries_[i].shortName;
        streamData[i] = entries_[i].streamData;
        streamCounts[i] = entries_[i].streamCount;
        if (entries_[i].directory) continue;
        if (entries_[i].stagedPath.isEmpty()) {
            hashes[i] = QByteArray(20, '\0');
            sizes[i] = 0;
            continue;
        }
        bool ok = false;
        const QByteArray hash = hashFile(entries_[i].stagedPath, &ok);
        QFile input(entries_[i].stagedPath);
        if (!ok || !input.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Could not read a staged WIM file.");
            return false;
        }
        hashes[i] = hash;
        sizes[i] = quint64(input.size());
        if (sizes[i] == 0) {
            hashes[i] = QByteArray(20, '\0');
            continue;
        }
        auto existing = resourceByHash.find(hash);
        if (existing != resourceByHash.end()) continue;
        Resource resource;
        resource.hash = hash;
        resource.originalSize = sizes[i];
        resource.offset = quint64(output.pos());
        resource.entry = int(i);
        resourceByHash[hash] = int(resources.size());
        if (compressionFlag_ == 0) {
            QByteArray buffer(1024 * 1024, Qt::Uninitialized);
            while (!input.atEnd()) {
                const qint64 count = input.read(buffer.data(), buffer.size());
                if (count < 0 || (count > 0 && !writeTracked(
                        &output, buffer.constData(), count, integrityOutput)))
                    return false;
            }
        } else {
            const quint64 chunkCount = (sizes[i] + chunkSize_ - 1) / chunkSize_;
            const quint64 tableSize = (chunkCount - 1) * 4;
            if (tableSize > quint64(std::numeric_limits<int>::max())) {
                if (error) *error = QStringLiteral("The WIM chunk table is too large.");
                return false;
            }
            QByteArray chunkTable(int(tableSize), '\0');
            quint64 compressedOffset = 0;
            bool compressResource = input.seek(0);
            for (quint64 chunkIndex = 0; compressResource && chunkIndex < chunkCount;
                 ++chunkIndex) {
                const qint64 count = qint64(qMin<quint64>(chunkSize_,
                    sizes[i] - chunkIndex * chunkSize_));
                const QByteArray raw = input.read(count);
                if (raw.size() != count) return false;
                const QByteArray encoded = encodeWimChunk(compressionFlag_, raw);
                if (encoded.isEmpty() || encoded.size() >= raw.size())
                    compressResource = false;
                else {
                    if (chunkIndex != 0) {
                        if (compressedOffset > 0xffffffffull) {
                            if (error) *error = QStringLiteral(
                                "The WIM resource exceeds the chunk-offset range.");
                            return false;
                        }
                        const quint32 value = quint32(compressedOffset);
                        for (unsigned byte = 0; byte < 4; ++byte)
                            chunkTable[int((chunkIndex - 1) * 4 + byte)] =
                                char((value >> (byte * 8)) & 0xff);
                    }
                    compressedOffset += quint64(encoded.size());
                }
            }
            if (!input.seek(0)) return false;
            if (!compressResource) {
                QByteArray buffer(1024 * 1024, Qt::Uninitialized);
                while (!input.atEnd()) {
                    const qint64 count = input.read(buffer.data(), buffer.size());
                    if (count < 0 || (count > 0 && !writeTracked(
                            &output, buffer.constData(), count, integrityOutput)))
                        return false;
                }
            } else {
            if (!writeTracked(&output, chunkTable, integrityOutput)) return false;
            quint64 chunkIndex = 0;
            quint64 encodedOffset = 0;
            while (!input.atEnd()) {
                const qint64 count = qint64(qMin<quint64>(chunkSize_,
                    sizes[i] - chunkIndex * chunkSize_));
                if (count <= 0) break;
                const QByteArray raw = input.read(count);
                if (raw.size() != count) return false;
                QByteArray encoded = encodeWimChunk(compressionFlag_, raw);
                if (encoded.isEmpty() || encoded.size() >= raw.size()) return false;
                if (chunkIndex != 0) {
                    const quint32 expectedOffset = get32(chunkTable,
                        qsizetype((chunkIndex - 1) * 4));
                    if (encodedOffset != expectedOffset) return false;
                }
                if (!writeTracked(&output, encoded, integrityOutput)) return false;
                encodedOffset += quint64(encoded.size());
                ++chunkIndex;
            }
            if (chunkIndex != chunkCount) return false;
            resource.flags = kResourceCompressed;
            }
        }
        resource.size = quint64(output.pos()) - resource.offset;
        resources.push_back(resource);
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].primaryStreamHashOffset >= 0) {
            const int hashOffset = entries_[i].primaryStreamHashOffset;
            if (hashOffset > streamData[i].size() - 20) return false;
            streamData[i].replace(hashOffset, 20, hashes[i]);
        }
    }

    std::map<QByteArray, quint64> oldUsage;
    std::map<QByteArray, quint64> newUsage;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].originalHash.size() == 20 &&
            entries_[i].originalHash != QByteArray(20, '\0'))
            ++oldUsage[entries_[i].originalHash];
        if (hashes[i].size() == 20 && hashes[i] != QByteArray(20, '\0'))
            ++newUsage[hashes[i]];
        for (const QByteArray& hash : entries_[i].alternateStreamHashes) {
            if (hash.size() == 20 && hash != QByteArray(20, '\0')) {
                ++oldUsage[hash];
                ++newUsage[hash];
            }
        }
    }
    for (Resource& resource : resources) {
        if (resource.tableRecord.isEmpty()) {
            resource.refs = quint32(newUsage[resource.hash]);
            continue;
        }
        if (resource.flags & kResourceMetadata) continue;
        const quint64 removed = oldUsage[resource.hash];
        const quint64 added = newUsage[resource.hash];
        if (removed > resource.refs) return false;
        resource.refs = quint32(resource.refs - removed + added);
        QByteArray encodedRefs;
        put32(&encodedRefs, resource.refs);
        resource.tableRecord.replace(26, 4, encodedRefs);
    }
    resources.erase(std::remove_if(resources.begin(), resources.end(),
        [](const Resource& resource) {
            return !(resource.flags & kResourceMetadata) && resource.refs == 0;
        }), resources.end());

    Node root;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const QString path = normalizedPath(entries_[i].name);
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        Node* current = &root;
        for (int p = 0; p < parts.size(); ++p) {
            Node& child = current->children[parts[p].toCaseFolded()];
            if (child.name.isEmpty()) child.name = parts[p];
            current = &child;
            if (p == parts.size() - 1) {
                current->entry = int(i);
                current->directory = entries_[i].directory;
            }
        }
    }

    QByteArray metadata;
    if (!sourceSecurityTable_.isEmpty()) {
        metadata.append(sourceSecurityTable_);
    } else {
        const QByteArray securityDescriptor = defaultSecurityDescriptor();
        const quint32 securityLength = quint32(16 + securityDescriptor.size());
        put32(&metadata, securityLength);
        put32(&metadata, 1);
        put64(&metadata, quint64(securityDescriptor.size()));
        metadata.append(securityDescriptor);
    }
    while (metadata.size() % 8) metadata.append('\0');
    const quint64 rootOffset = quint64(metadata.size());
    put64(&metadata, 104);
    put32(&metadata, kDirectoryAttribute);
    put32(&metadata, 0);
    put64(&metadata, rootOffset + 112);
    put64(&metadata, 0);
    put64(&metadata, 0);
    put64(&metadata, 0);
    put64(&metadata, 0);
    put64(&metadata, 0);
    metadata.append(QByteArray(20, '\0'));
    put64(&metadata, 0);
    put16(&metadata, 0);
    put16(&metadata, 0);
    put16(&metadata, 0);
    put16(&metadata, 0);
    put16(&metadata, 0);
    metadata.append(QByteArray(2, '\0'));
    metadata.append(QByteArray(8, '\0'));
    const quint32 defaultSecurityId = 0;
    appendDirectoryEntries(root, &metadata, hashes, sizes, shortNames,
                           streamData, streamCounts, securityIds,
                           defaultSecurityId, 0);

    Resource metadataResource;
    metadataResource.hash = QCryptographicHash::hash(metadata, QCryptographicHash::Sha1);
    metadataResource.size = quint64(metadata.size());
    metadataResource.offset = quint64(output.pos());
    if (!writeTracked(&output, metadata, integrityOutput)) return false;

    QByteArray table;
    QByteArray metadataRecord;
    if (!sourceResources_.empty()) {
        metadataRecord = sourceResources_[selectedMetadataIndex_].tableRecord;
        offsetMap[sourceResources_[selectedMetadataIndex_].offset] = metadataResource.offset;
        metadataRecord.replace(0, 24, resourceHeader(metadataResource.size,
            metadataResource.offset, metadataResource.size, kResourceMetadata));
        QByteArray referenceCount;
        put32(&referenceCount, 1);
        metadataRecord.replace(26, 4, referenceCount);
        metadataRecord.replace(30, 20, metadataResource.hash);
    } else {
        metadataRecord = resourceHeader(metadataResource.size, metadataResource.offset,
                                        metadataResource.size, kResourceMetadata);
        put16(&metadataRecord, 1);
        put32(&metadataRecord, 1);
        metadataRecord.append(metadataResource.hash);
    }
    table.append(metadataRecord);
    for (const Resource& resource : resources) {
        if (!resource.tableRecord.isEmpty()) {
            table.append(resource.tableRecord);
        } else {
            table.append(resourceHeader(resource.size, resource.offset,
                                        resource.originalSize, resource.flags));
            put16(&table, 1);
            put32(&table, resource.refs);
            table.append(resource.hash);
        }
    }
    const quint64 tableOffset = quint64(output.pos());
    if (!writeTracked(&output, table, integrityOutput)) return false;

    QByteArray integrityTable;
    if (integrityOutput) {
        integrityTable = integrity.finish();
        if (integrityTable.isEmpty()) {
            if (error) *error = QStringLiteral("The WIM integrity table is too large.");
            return false;
        }
    }

    QByteArray xml;
    quint64 directoryCount = 0;
    quint64 fileCount = 0;
    countNodes(root, &directoryCount, &fileCount);
    ++directoryCount;
    QByteArray xmlUtf16;
    quint64 imageBytes = 0;
    for (quint64 size : sizes) imageBytes += size;
    if (!sourceXml_.isEmpty()) {
        xmlUtf16 = updateWimXml(sourceXml_, directoryCount, fileCount,
                                imageBytes, quint64(output.pos()));
        if (xmlUtf16.isEmpty()) {
            if (error) *error = QStringLiteral("The WIM image XML could not be updated.");
            return false;
        }
    } else {
        QXmlStreamWriter xmlWriter(&xml);
        xmlWriter.writeStartElement(QStringLiteral("WIM"));
        xmlWriter.writeStartElement(QStringLiteral("IMAGE"));
        xmlWriter.writeAttribute(QStringLiteral("INDEX"), QStringLiteral("1"));
        xmlWriter.writeTextElement(QStringLiteral("NAME"), QStringLiteral("Peare Image"));
        xmlWriter.writeTextElement(QStringLiteral("DESCRIPTION"), QString());
        xmlWriter.writeTextElement(QStringLiteral("DIRCOUNT"), QString::number(directoryCount));
        xmlWriter.writeTextElement(QStringLiteral("FILECOUNT"), QString::number(fileCount));
        xmlWriter.writeTextElement(QStringLiteral("TOTALBYTES"), QString::number(imageBytes));
        xmlWriter.writeTextElement(QStringLiteral("HARDLINKBYTES"), QStringLiteral("0"));
        const quint64 creationTime = quint64(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) *
                                     10000ull + 116444736000000000ull;
        writeImageTime(&xmlWriter, QStringLiteral("CREATIONTIME"), creationTime);
        writeImageTime(&xmlWriter, QStringLiteral("LASTMODIFICATIONTIME"), creationTime);
        xmlWriter.writeEndElement();
        xmlWriter.writeTextElement(QStringLiteral("TOTALBYTES"), QString::number(output.pos()));
        xmlWriter.writeEndElement();
        put16(&xmlUtf16, 0xfeff);
        xmlUtf16.append(utf16le(QString::fromUtf8(xml)));
    }
    for (unsigned pass = 0; pass < 4; ++pass) {
        const quint64 finalSize = quint64(output.pos()) + quint64(xmlUtf16.size()) +
                                  quint64(integrityTable.size());
        QByteArray updatedXml = updateWimXml(xmlUtf16, directoryCount, fileCount,
                                             imageBytes, finalSize);
        if (updatedXml.isEmpty()) {
            if (error) *error = QStringLiteral("The WIM container size could not be updated.");
            return false;
        }
        if (updatedXml == xmlUtf16) break;
        xmlUtf16 = updatedXml;
    }
    const quint64 xmlOffset = quint64(output.pos());
    if (!checkedWrite(&output, xmlUtf16)) return false;
    const quint64 integrityOffset = quint64(output.pos());
    if (!integrityTable.isEmpty() && !checkedWrite(&output, integrityTable)) return false;

    QByteArray header;
    if (sourceHeader_.size() == kHeaderSize) {
        header = sourceHeader_;
        header.replace(48, 24, resourceHeader(table.size(), tableOffset,
                                              table.size(), kResourceMetadata));
        header.replace(72, 24, resourceHeader(xmlUtf16.size(), xmlOffset,
                                              xmlUtf16.size(), kResourceMetadata));
        header.replace(124, 24, integrityTable.isEmpty() ? resourceHeader(0, 0, 0) :
            resourceHeader(integrityTable.size(), integrityOffset,
                           integrityTable.size()));
        const quint64 bootSize = get64(sourceHeader_, 96) & 0x00ffffffffffffffull;
        if (bootSize) {
            const quint64 oldBootOffset = get64(sourceHeader_, 104);
            auto mapped = offsetMap.find(oldBootOffset);
            if (mapped == offsetMap.end()) return false;
            auto oldBoot = std::find_if(sourceResources_.begin(), sourceResources_.end(),
                [&](const ExistingResource& resource) {
                    return resource.offset == oldBootOffset;
                });
            if (oldBoot == sourceResources_.end()) return false;
            quint64 newSize = oldBoot->size;
            quint64 newOriginalSize = oldBoot->originalSize;
            quint8 newFlags = oldBoot->flags;
            if (std::size_t(oldBoot - sourceResources_.begin()) == selectedMetadataIndex_) {
                newSize = metadataResource.size;
                newOriginalSize = metadataResource.size;
                newFlags = kResourceMetadata;
            }
            header.replace(96, 24, resourceHeader(newSize, mapped->second,
                newOriginalSize, newFlags));
        }
    } else {
        header.append(QByteArray("MSWIM\0\0\0", 8));
        put32(&header, quint32(kHeaderSize));
        put32(&header, 0x00010d00u);
        put32(&header, 0x00000080u | (compressionFlag_ ? kWimCompression : 0) |
                       compressionFlag_);
        put32(&header, chunkSize_);
        header.append(QByteArray::fromHex("00112233445566778899aabbccddeeff"));
        put16(&header, 1);
        put16(&header, 1);
        put32(&header, 1);
        header.append(resourceHeader(table.size(), tableOffset, table.size(), kResourceMetadata));
        header.append(resourceHeader(xmlUtf16.size(), xmlOffset, xmlUtf16.size(), kResourceMetadata));
        header.append(resourceHeader(0, 0, 0));
        header.append(resourceHeader(0, 0, 0));
        header.append(resourceHeader(0, 0, 0));
        header.append(resourceHeader(0, 0, 0));
        header.append(QByteArray(kHeaderSize - header.size(), '\0'));
    }
    oldImage.close();
    if (!output.seek(0) || !checkedWrite(&output, header) || !output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

bool WimImageWriter::finish(QString* error) {
    if (!active_) {
        if (error) *error = QStringLiteral("No WIM write is active.");
        return false;
    }
    if (!writeImage(error)) return false;
    active_ = false;
    outputPath_.clear();
    sourcePath_.clear();
    sourceHeader_.clear();
    sourceSecurityTable_.clear();
    sourceXml_.clear();
    sourceResources_.clear();
    integrityChunkSize_ = 0;
    entries_.clear();
    staging_.reset();
    return true;
}

void WimImageWriter::abort() {
    active_ = false;
    outputPath_.clear();
    sourcePath_.clear();
    sourceHeader_.clear();
    sourceSecurityTable_.clear();
    sourceXml_.clear();
    sourceResources_.clear();
    integrityChunkSize_ = 0;
    entries_.clear();
    staging_.reset();
}

}  // namespace writer
}  // namespace peare
