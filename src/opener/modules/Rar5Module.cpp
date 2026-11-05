#include "Rar5Module.h"
#include "Compat.h"

#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace peare {
namespace {

const std::uint8_t kMarker[] = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00};
const std::uint64_t kMaximumHeaderSize = 2 * 1024 * 1024;
const std::uint64_t kMaximumEntries = 1000000;

bool readExact(const fs::IByteStore& source, std::int64_t offset,
               std::uint8_t* output, int size)
{
    const std::int64_t capacity = source.capacity();
    if (!output || offset < 0 || size < 0 || capacity < 0 || size > capacity ||
        offset > capacity - size) return false;
    int done = 0;
    while (done < size) {
        const int got = source.read(offset + done, output + done, size - done);
        if (got <= 0 || got > size - done) return false;
        done += got;
    }
    return true;
}

std::uint32_t readLe32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}

class Cursor final {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool readVint(std::uint64_t* value)
    {
        if (!value) return false;
        std::uint64_t result = 0;
        for (unsigned i = 0; i < 10; ++i) {
            std::uint8_t byte = 0;
            if (!readByte(&byte) || (i == 9 && (byte & 0xfe))) return false;
            result |= std::uint64_t(byte & 0x7f) << (i * 7);
            if (!(byte & 0x80)) {
                *value = result;
                return true;
            }
        }
        return false;
    }

    bool readByte(std::uint8_t* value)
    {
        if (!value || position_ >= size_) return false;
        *value = data_[position_++];
        return true;
    }

    bool readBytes(std::size_t count, const std::uint8_t** value)
    {
        if (!value || count > size_ - position_) return false;
        *value = data_ + position_;
        position_ += count;
        return true;
    }

    std::size_t position() const { return position_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

class Rar5BitReader final {
public:
    Rar5BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool read(unsigned count, std::uint32_t* value)
    {
        if (!value || count > 32 || size_ > std::numeric_limits<std::size_t>::max() / 8 ||
            bit_ > size_ * 8 || count > size_ * 8 - bit_) return false;
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            result = (result << 1) |
                ((data_[bit_ / 8] >> (7U - unsigned(bit_ & 7U))) & 1U);
            ++bit_;
        }
        *value = result;
        return true;
    }

    std::size_t position() const { return bit_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bit_ = 0;
};

class Rar5Huffman final {
public:
    bool build(const std::vector<std::uint8_t>& lengths)
    {
        std::array<std::uint32_t, 16> counts{};
        for (std::uint8_t length : lengths) {
            if (length > 15) return false;
            if (length) ++counts[length];
        }
        std::uint32_t code = 0;
        std::uint32_t offset = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            code = (code + counts[length - 1]) << 1;
            const std::uint32_t limit = 1U << length;
            if (code > limit || counts[length] > limit - code) return false;
            firstCode_[length] = code;
            firstSymbol_[length] = offset;
            count_[length] = counts[length];
            offset += counts[length];
        }
        if (!offset) return false;
        symbols_.clear();
        symbols_.reserve(offset);
        for (unsigned length = 1; length <= 15; ++length)
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
                if (lengths[symbol] == length) symbols_.push_back(std::uint32_t(symbol));
        return symbols_.size() == offset;
    }

    bool decode(Rar5BitReader* bits, std::uint32_t* symbol) const
    {
        if (!bits || !symbol) return false;
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            std::uint32_t bit = 0;
            if (!bits->read(1, &bit)) return false;
            code = (code << 1) | bit;
            if (code >= firstCode_[length] &&
                code - firstCode_[length] < count_[length]) {
                const std::uint32_t index = firstSymbol_[length] + code - firstCode_[length];
                if (index >= symbols_.size()) return false;
                *symbol = symbols_[index];
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::uint32_t, 16> firstCode_{};
    std::array<std::uint32_t, 16> firstSymbol_{};
    std::array<std::uint32_t, 16> count_{};
    std::vector<std::uint32_t> symbols_;
};

struct Rar5Tables {
    Rar5Huffman literalLength;
    Rar5Huffman distance;
    Rar5Huffman lowDistance;
    Rar5Huffman repeatedLength;
};

bool readRar5Tables(Rar5BitReader* bits, Rar5Tables* tables)
{
    std::vector<std::uint8_t> bitLengths(20);
    for (std::size_t i = 0; i < bitLengths.size(); ++i) {
        std::uint32_t length = 0;
        if (!bits->read(4, &length)) return false;
        if (length == 15) {
            std::uint32_t zeros = 0;
            if (!bits->read(4, &zeros)) return false;
            if (!zeros) bitLengths[i] = 15;
            else {
                const std::size_t run = zeros + 2;
                if (run > bitLengths.size() - i) return false;
                i += run - 1;
            }
        } else bitLengths[i] = std::uint8_t(length);
    }
    Rar5Huffman bitLengthTree;
    if (!bitLengthTree.build(bitLengths)) return false;
    const std::size_t tableSize = 306 + 64 + 16 + 44;
    std::vector<std::uint8_t> lengths;
    lengths.reserve(tableSize);
    while (lengths.size() < tableSize) {
        std::uint32_t symbol = 0;
        if (!bitLengthTree.decode(bits, &symbol)) return false;
        if (symbol < 16) lengths.push_back(std::uint8_t(symbol));
        else {
            std::uint32_t extra = 0;
            std::size_t count = 0;
            if (symbol < 18) {
                if (lengths.empty()) return false;
                if (!bits->read(symbol == 16 ? 3 : 7, &extra)) return false;
                count = extra + (symbol == 16 ? 3 : 11);
                if (count > tableSize - lengths.size()) return false;
                lengths.insert(lengths.end(), count, lengths.back());
            } else if (symbol < 20) {
                if (!bits->read(symbol == 18 ? 3 : 7, &extra)) return false;
                count = extra + (symbol == 18 ? 3 : 11);
                if (count > tableSize - lengths.size()) return false;
                lengths.insert(lengths.end(), count, 0);
            } else return false;
        }
    }
    std::vector<std::uint8_t> part(lengths.begin(), lengths.begin() + 306);
    if (!tables->literalLength.build(part)) return false;
    part.assign(lengths.begin() + 306, lengths.begin() + 370);
    if (!tables->distance.build(part)) return false;
    part.assign(lengths.begin() + 370, lengths.begin() + 386);
    if (!tables->lowDistance.build(part)) return false;
    part.assign(lengths.begin() + 386, lengths.end());
    return tables->repeatedLength.build(part);
}

bool decodeRar5Member(const std::uint8_t* packed, std::size_t packedSize,
                      std::uint64_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!packed || !output || unpackedSize > std::uint64_t(output->max_size())) return false;
    output->clear();
    output->reserve(std::size_t(unpackedSize));
    std::size_t position = 0;
    Rar5Tables tables;
    bool haveTables = false;
    std::array<std::uint64_t, 4> oldDistances{{0, 0, 0, 0}};
    std::uint64_t lastLength = 0;
    while (position < packedSize) {
        if (packedSize - position < 3) return false;
        const std::uint8_t flags = packed[position++];
        const std::uint8_t savedChecksum = packed[position++];
        const unsigned byteCount = ((flags >> 3) & 3U) + 1;
        if (byteCount == 4 || byteCount > packedSize - position) return false;
        std::uint32_t blockSize = 0;
        for (unsigned i = 0; i < byteCount; ++i)
            blockSize |= std::uint32_t(packed[position++]) << (8U * i);
        const std::uint8_t checksum = std::uint8_t(0x5aU ^ flags ^ blockSize ^
            (blockSize >> 8) ^ (blockSize >> 16));
        if (!blockSize || checksum != savedChecksum || blockSize > packedSize - position)
            return false;
        Rar5BitReader bits(packed + position, blockSize);
        const std::size_t blockEnd = (std::size_t(blockSize) - 1) * 8 + (flags & 7U) + 1;
        if (flags & 0x80U) {
            if (!readRar5Tables(&bits, &tables)) return false;
            haveTables = true;
        }
        if (!haveTables) return false;
        while (bits.position() < blockEnd && output->size() < unpackedSize) {
            std::uint32_t symbol = 0;
            if (!tables.literalLength.decode(&bits, &symbol)) return false;
            if (symbol < 256) output->push_back(std::uint8_t(symbol));
            else {
                std::uint64_t length = 0;
                std::uint64_t distance = 0;
                if (symbol == 256) return false;
                if (symbol == 257) {
                    if (!lastLength) return false;
                    length = lastLength;
                    distance = oldDistances[0];
                } else if (symbol < 262) {
                    const unsigned index = symbol - 258;
                    distance = oldDistances[index];
                    for (unsigned i = index; i > 0; --i) oldDistances[i] = oldDistances[i - 1];
                    oldDistances[0] = distance;
                    std::uint32_t slot = 0;
                    if (!tables.repeatedLength.decode(&bits, &slot)) return false;
                    std::uint32_t extra = 0;
                    const unsigned extraBits = slot < 8 ? 0 : slot / 4 - 1;
                    length = slot < 8 ? 2 + slot : 2 + ((4U | (slot & 3U)) << extraBits);
                    if (extraBits && (!bits.read(extraBits, &extra))) return false;
                    length += extra;
                } else {
                    const std::uint32_t slot = symbol - 262;
                    const unsigned lengthBits = slot < 8 ? 0 : slot / 4 - 1;
                    std::uint32_t extraLength = 0;
                    length = slot < 8 ? 2 + slot : 2 + ((4U | (slot & 3U)) << lengthBits);
                    if (lengthBits && !bits.read(lengthBits, &extraLength)) return false;
                    length += extraLength;
                    std::uint32_t distanceSlot = 0;
                    if (!tables.distance.decode(&bits, &distanceSlot)) return false;
                    unsigned distanceBits = 0;
                    distance = 1;
                    if (distanceSlot < 4) distance += distanceSlot;
                    else {
                        distanceBits = distanceSlot / 2 - 1;
                        distance += std::uint64_t(2U | (distanceSlot & 1U)) << distanceBits;
                        if (distanceBits >= 4) {
                            std::uint32_t high = 0, low = 0;
                            if (distanceBits > 4 && !bits.read(distanceBits - 4, &high)) return false;
                            if (!tables.lowDistance.decode(&bits, &low)) return false;
                            distance += (std::uint64_t(high) << 4) + low;
                        } else if (distanceBits) {
                            std::uint32_t low = 0;
                            if (!bits.read(distanceBits, &low)) return false;
                            distance += low;
                        }
                    }
                    if (distance > 0x100) ++length;
                    if (distance > 0x2000) ++length;
                    if (distance > 0x40000) ++length;
                    for (unsigned i = 3; i > 0; --i) oldDistances[i] = oldDistances[i - 1];
                    oldDistances[0] = distance;
                }
                if (!distance || distance > output->size() || length > unpackedSize - output->size())
                    return false;
                lastLength = length;
                for (std::uint64_t i = 0; i < length; ++i)
                    output->push_back((*output)[output->size() - std::size_t(distance)]);
            }
        }
        if (bits.position() > blockEnd) return false;
        position += blockSize;
        if (flags & 0x40U) return output->size() == unpackedSize && position == packedSize;
    }
    return false;
}

QString safePath(const QByteArray& bytes)
{
    QString path = QString::fromUtf8(bytes.constData(), bytes.size());
    if (path.toUtf8() != bytes) return QString();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1Char('/')) || path.contains(QChar(0))) return QString();
    QStringList parts;
    const QStringList rawParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : rawParts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) return QString();
        parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

class Rar5MemberStore final : public fs::IByteStore {
public:
    Rar5MemberStore(fs::ByteStorePtr source, std::int64_t offset,
                    std::uint64_t size, std::uint32_t expectedCrc)
        : source_(std::move(source)), offset_(offset), size_(size), crc_(expectedCrc) {}

    std::int64_t capacity() const override
    {
        return size_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(size_) : 0;
    }
    bool cheapRandomAccess() const override { return source_ && source_->cheapRandomAccess(); }
    bool validate() const override
    {
        if (!source_ || offset_ < 0 ||
            size_ > std::uint64_t(source_->capacity()) ||
            std::uint64_t(offset_) > std::uint64_t(source_->capacity()) - size_ ||
            size_ > std::uint64_t(std::numeric_limits<std::size_t>::max())) return false;
        std::uint8_t buffer[64 * 1024];
        std::uint32_t value = 0xffffffffU;
        std::uint64_t done = 0;
        while (done < size_) {
            const int amount = int(std::min<std::uint64_t>(sizeof(buffer), size_ - done));
            if (!readExact(*source_, offset_ + std::int64_t(done), buffer, amount)) return false;
            for (int i = 0; i < amount; ++i) {
                value ^= buffer[i];
                for (int bit = 0; bit < 8; ++bit)
                    value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            }
            done += std::uint64_t(amount);
        }
        return (value ^ 0xffffffffU) == crc_;
    }
    int read(std::int64_t position, std::uint8_t* output, int count) const override
    {
        if (!source_ || !output || position < 0 || count <= 0 || !validate() ||
            std::uint64_t(position) >= size_) return 0;
        const int amount = int(std::min<std::uint64_t>(count, size_ - std::uint64_t(position)));
        return source_->read(offset_ + position, output, amount);
    }

private:
    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::uint64_t size_;
    std::uint32_t crc_;
};

class Rar5CompressedStore final : public fs::IByteStore {
public:
    Rar5CompressedStore(fs::ByteStorePtr source, std::int64_t offset,
                        std::uint64_t packedSize, std::uint64_t unpackedSize,
                        std::uint32_t expectedCrc)
        : source_(std::move(source)), offset_(offset), packedSize_(packedSize),
          unpackedSize_(unpackedSize), crc_(expectedCrc) {}

    std::int64_t capacity() const override
    {
        return unpackedSize_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(unpackedSize_) : 0;
    }
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (checked_) return valid_;
        checked_ = true;
        if (!source_ || offset_ < 0 ||
            packedSize_ > std::uint64_t(source_->capacity()) ||
            std::uint64_t(offset_) > std::uint64_t(source_->capacity()) - packedSize_ ||
            packedSize_ > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
            unpackedSize_ > std::uint64_t(std::numeric_limits<std::size_t>::max())) return false;
        try {
            std::vector<std::uint8_t> packed(static_cast<std::size_t>(packedSize_), std::uint8_t(0));
            std::uint64_t done = 0;
            while (done < packedSize_) {
                const int amount = int(std::min<std::uint64_t>(64 * 1024, packedSize_ - done));
                if (!readExact(*source_, offset_ + std::int64_t(done),
                               packed.data() + std::size_t(done), amount)) return false;
                done += std::uint64_t(amount);
            }
            valid_ = decodeRar5Member(packed.data(), packed.size(), unpackedSize_, &decoded_) &&
                crc32(decoded_.data(), decoded_.size()) == crc_;
        } catch (...) {
            decoded_.clear();
            valid_ = false;
        }
        return valid_;
    }
    int read(std::int64_t position, std::uint8_t* output, int count) const override
    {
        if (!output || position < 0 || count <= 0 || !validate() ||
            std::uint64_t(position) >= unpackedSize_) return 0;
        const int amount = int(std::min<std::uint64_t>(
            count, unpackedSize_ - std::uint64_t(position)));
        std::memcpy(output, decoded_.data() + std::size_t(position), std::size_t(amount));
        return amount;
    }

private:
    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::uint64_t packedSize_;
    std::uint64_t unpackedSize_;
    std::uint32_t crc_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable std::mutex mutex_;
    mutable bool checked_ = false;
    mutable bool valid_ = false;
};

} // namespace

ModulePtr Rar5Module::open(const QString& filePath, const QString& password)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<Rar5Module>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::RAR5;
        module->info_.description = QStringLiteral("RAR5 archive");
        module->info_.error = QStringLiteral("RAR5 source is unavailable");
        return module;
    }
    return open(source, filePath, password);
}

ModulePtr Rar5Module::open(const fs::ByteStorePtr& source, const QString& sourceName,
                           const QString& password)
{
    auto module = peare::makeUnique<Rar5Module>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::RAR5;
    module->info_.description = QStringLiteral("RAR5 archive");
    if (!source || source->capacity() < std::int64_t(sizeof(kMarker))) {
        module->info_.error = QStringLiteral("Invalid RAR5 source");
        return module;
    }
    std::int64_t marker = -1;
    const std::int64_t scanEnd = std::min<std::int64_t>(
        source->capacity() - std::int64_t(sizeof(kMarker)), 1024 * 1024);
    std::uint8_t probe[sizeof(kMarker)];
    for (std::int64_t i = 0; i <= scanEnd; ++i) {
        if (readExact(*source, i, probe, sizeof(probe)) &&
            std::equal(probe, probe + sizeof(probe), kMarker)) {
            marker = i;
            break;
        }
    }
    if (marker < 0) {
        module->info_.error = QStringLiteral("RAR5 marker not found");
        return module;
    }
    module->info_.headerOffset = std::uint64_t(marker);
    std::int64_t position = marker + std::int64_t(sizeof(kMarker));
    std::uint64_t entryCount = 0;
    bool sawMain = false;
    bool sawEnd = false;
    while (position >= 0 && position < source->capacity()) {
        std::uint8_t crcBytes[4];
        if (!readExact(*source, position, crcBytes, sizeof(crcBytes))) break;
        std::uint8_t sizeBytes[10];
        std::uint64_t headerSize = 0;
        int sizeLength = 0;
        for (; sizeLength < int(sizeof(sizeBytes)); ++sizeLength) {
            if (!readExact(*source, position + 4 + sizeLength, sizeBytes + sizeLength, 1)) {
                module->info_.error = QStringLiteral("Truncated RAR5 header size");
                return module;
            }
            Cursor sizeCursor(sizeBytes, std::size_t(sizeLength + 1));
            if (sizeCursor.readVint(&headerSize)) {
                ++sizeLength;
                break;
            }
            if (!(sizeBytes[sizeLength] & 0x80)) {
                module->info_.error = QStringLiteral("Invalid RAR5 header size");
                return module;
            }
        }
        if (sizeLength <= 0 || sizeLength > 10 || headerSize == 0 ||
            headerSize > kMaximumHeaderSize ||
            std::uint64_t(position) + 4 + std::uint64_t(sizeLength) + headerSize >
                std::uint64_t(source->capacity())) {
            module->info_.error = QStringLiteral("Invalid or truncated RAR5 header");
            return module;
        }
        const std::size_t crcLength = std::size_t(sizeLength) + std::size_t(headerSize);
        std::vector<std::uint8_t> crcData(crcLength);
        if (!readExact(*source, position + 4, crcData.data(), int(crcData.size())) ||
            crc32(crcData.data(), crcData.size()) != readLe32(crcBytes)) {
            module->info_.error = QStringLiteral("RAR5 header CRC mismatch");
            return module;
        }
        Cursor cursor(crcData.data() + sizeLength, std::size_t(headerSize));
        std::uint64_t type = 0;
        std::uint64_t flags = 0;
        std::uint64_t extraSize = 0;
        std::uint64_t dataSize = 0;
        if (!cursor.readVint(&type) || !cursor.readVint(&flags) ||
            ((flags & 1) && !cursor.readVint(&extraSize)) ||
            ((flags & 2) && !cursor.readVint(&dataSize))) {
            module->info_.error = QStringLiteral("Malformed RAR5 block header");
            return module;
        }
        const std::uint64_t physicalHeaderSize = 4 + std::uint64_t(sizeLength) + headerSize;
        const std::uint64_t dataOffset = std::uint64_t(position) + physicalHeaderSize;
        if (dataSize > std::uint64_t(source->capacity()) ||
            dataOffset > std::uint64_t(source->capacity()) - dataSize) {
            module->info_.error = QStringLiteral("RAR5 block data exceeds source bounds");
            return module;
        }
        if (type == 1) {
            sawMain = true;
        } else if (type == 2) {
            if (!sawMain || ++entryCount > kMaximumEntries) {
                module->info_.error = QStringLiteral("Invalid RAR5 file entry sequence");
                return module;
            }
            std::uint64_t fileFlags = 0;
            std::uint64_t unpacked = 0;
            std::uint64_t attributes = 0;
            if (!cursor.readVint(&fileFlags) || !cursor.readVint(&unpacked) ||
                !cursor.readVint(&attributes)) {
                module->info_.error = QStringLiteral("Malformed RAR5 file header");
                return module;
            }
            if (fileFlags & 2) {
                const std::uint8_t* timeBytes = nullptr;
                if (!cursor.readBytes(4, &timeBytes)) {
                    module->info_.error = QStringLiteral("Truncated RAR5 timestamp");
                    return module;
                }
            }
            std::uint32_t expectedCrc = 0;
            if (fileFlags & 4) {
                const std::uint8_t* crc = nullptr;
                if (!cursor.readBytes(4, &crc)) {
                    module->info_.error = QStringLiteral("Truncated RAR5 file CRC");
                    return module;
                }
                expectedCrc = readLe32(crc);
            }
            std::uint64_t compressionInfo = 0;
            std::uint64_t hostOs = 0;
            std::uint64_t nameSize = 0;
            const std::uint8_t* nameBytes = nullptr;
            if (!cursor.readVint(&compressionInfo) || !cursor.readVint(&hostOs) ||
                !cursor.readVint(&nameSize) || nameSize > 65535 ||
                !cursor.readBytes(std::size_t(nameSize), &nameBytes)) {
                module->info_.error = QStringLiteral("Malformed RAR5 file name or compression fields");
                return module;
            }
            const unsigned method = unsigned((compressionInfo >> 7) & 7U);
            const unsigned algorithmVersion = unsigned(compressionInfo & 0x3fU);
            const bool solid = (compressionInfo & 0x40U) != 0;
            const bool directory = (fileFlags & 1U) != 0;
            if (!directory && (algorithmVersion != 0 || solid || method > 5)) {
                module->info_.error = QStringLiteral("Unsupported RAR5 compression parameters");
                return module;
            }
            if (!directory && (fileFlags & 4U) == 0) {
                module->info_.error = QStringLiteral("RAR5 file entry has no CRC32");
                return module;
            }
            if (!directory && method == 0 && dataSize != unpacked) {
                module->info_.error = QStringLiteral("RAR5 stored member size mismatch");
                return module;
            }
            const QByteArray rawName(reinterpret_cast<const char*>(nameBytes), int(nameSize));
            const QString normalized = safePath(rawName);
            if (normalized.isEmpty()) {
                module->info_.error = QStringLiteral("Invalid or unsafe RAR5 path");
                return module;
            }
            const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            ResourceEntry entry;
            entry.type = directory ? QStringLiteral("RAR5_DIR") : QStringLiteral("RAR5_FILE");
            entry.name = parts.last();
            entry.hierarchyPath = parts;
            entry.hierarchyPath.removeLast();
            entry.dataOffset = dataOffset;
            entry.dataSize = directory ? 0 : unpacked;
            entry.isEmbeddedFile = !directory;
            if (!directory) {
                if (method == 0)
                    entry.content = std::make_shared<Rar5MemberStore>(source,
                        std::int64_t(dataOffset), unpacked, expectedCrc);
                else
                    entry.content = std::make_shared<Rar5CompressedStore>(source,
                        std::int64_t(dataOffset), dataSize, unpacked, expectedCrc);
            }
            module->resources_.push_back(std::move(entry));
            Q_UNUSED(attributes);
            Q_UNUSED(hostOs);
            Q_UNUSED(extraSize);
        } else if (type == 5) {
            sawEnd = true;
            break;
        }
        position += std::int64_t(physicalHeaderSize + dataSize);
    }
    if (!sawMain) module->info_.error = QStringLiteral("RAR5 main header not found");
    else if (!sawEnd) module->info_.error = QStringLiteral("RAR5 end marker not found");
    return module;
}

} // namespace peare
