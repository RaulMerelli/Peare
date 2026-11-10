#include "Rar5Module.h"
#include "Compat.h"

#include "../fs/QFileByteStore.h"
#include "../../common/rar/Rar5Crypto.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <array>
#include <cstring>
#include <vector>

using peare::compression::decryptRar5AesCbc;
using peare::compression::decryptRar5Data;
using peare::compression::deriveRar5Keys;
using peare::compression::rar5Checksum;

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
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values = {};
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = std::uint32_t(i);
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1U) ? 0xedb88320U : 0U);
            values[i] = value;
        }
        return values;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xffU];
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

#include "../../common/rar/Rar5Decoder.inl"

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

QString nextPartVolumePath(const QString& path)
{
    const int suffix = path.lastIndexOf(QLatin1String(".part"), -1,
                                        Qt::CaseInsensitive);
    const int extension = path.lastIndexOf(QLatin1String(".rar"), -1,
                                           Qt::CaseInsensitive);
    if (suffix < 0 || extension <= suffix + 5) return QString();
    const QString number = path.mid(suffix + 5, extension - suffix - 5);
    bool ok = false;
    const int current = number.toInt(&ok);
    if (!ok || current < 1) return QString();
    return path.left(suffix + 5) + QString::number(current + 1).rightJustified(
        number.size(), QLatin1Char('0')) + path.mid(extension);
}

struct Rar5DataChunk {
    fs::ByteStorePtr source;
    std::int64_t offset;
    std::uint64_t size;
};

struct Rar5MemberData {
    std::vector<Rar5DataChunk> chunks;
    QString path;
    std::uint64_t packedSize = 0;
    std::uint64_t unpackedSize = 0;
    std::uint64_t dictionarySize = 128 * 1024;
    std::uint32_t crc = 0;
    unsigned algorithmVersion = 0;
    unsigned method = 0;
    bool extendedDistances = false;
    bool solid = false;
    bool encrypted = false;
    std::uint8_t encryptionFlags = 0;
    std::uint8_t kdfCount = 0;
    std::array<std::uint8_t, 16> salt{};
    std::array<std::uint8_t, 16> iv{};
};

class Rar5SolidSequence final {
public:
    Rar5SolidSequence(std::vector<Rar5MemberData> members, QByteArray password)
        : members_(std::move(members)),
          password_(std::move(password)),
          decoded_(members_.size()), checked_(members_.size(), 0) {}

    std::uint64_t memberSize(std::size_t index) const
    {
        return index < members_.size() ? members_[index].unpackedSize : 0;
    }

    bool readMember(std::size_t index, std::int64_t position,
                    std::uint8_t* output, int count) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!decodeThrough(index) || position < 0 || count <= 0 || !output ||
            std::uint64_t(position) >= decoded_[index].size()) return false;
        const std::size_t amount = std::min<std::size_t>(
            std::size_t(count), decoded_[index].size() - std::size_t(position));
        std::memcpy(output, decoded_[index].data() + std::size_t(position), amount);
        return amount != 0;
    }

    bool validateMember(std::size_t index) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return decodeThrough(index);
    }

private:
    bool decodeThrough(std::size_t index) const
    {
        if (index >= members_.size() || checked_[index] == 2) return false;
        if (checked_[index] == 1) return true;
        std::size_t first = index;
        while (first > 0 && members_[first].solid) --first;
        Rar5UnpackState state;
        for (std::size_t current = first; current <= index; ++current) {
            const Rar5MemberData& member = members_[current];
            if (!member.solid) state = Rar5UnpackState();
            if (member.unpackedSize > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
                member.packedSize > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
                checked_[current] = 2;
                return false;
            }
            try {
                std::vector<std::uint8_t> packed(
                    static_cast<std::size_t>(member.packedSize));
                std::uint64_t outputOffset = 0;
                for (const Rar5DataChunk& chunk : member.chunks) {
                    if (!chunk.source || chunk.offset < 0 ||
                        chunk.size > std::uint64_t(chunk.source->capacity()) ||
                        std::uint64_t(chunk.offset) >
                            std::uint64_t(chunk.source->capacity()) - chunk.size ||
                        chunk.size > member.packedSize - outputOffset) {
                        checked_[current] = 2;
                        return false;
                    }
                    std::uint64_t done = 0;
                    while (done < chunk.size) {
                    const int amount = int(std::min<std::uint64_t>(
                            64 * 1024, chunk.size - done));
                        if (!readExact(*chunk.source, chunk.offset + std::int64_t(done),
                                       packed.data() + std::size_t(outputOffset + done),
                                       amount)) {
                        checked_[current] = 2;
                        return false;
                    }
                    done += std::uint64_t(amount);
                    }
                    outputOffset += chunk.size;
                }
                if (outputOffset != member.packedSize) {
                    checked_[current] = 2;
                    return false;
                }
                QByteArray checksumKey;
                if (member.encrypted) {
                    if (password_.isNull()) {
                        checked_[current] = 2;
                        return false;
                    }
                    std::vector<std::uint8_t> decrypted;
                    if (!decryptRar5Data(password_, member.salt.data(), member.iv.data(),
                                         member.kdfCount, packed, &decrypted, &checksumKey)) {
                        checked_[current] = 2;
                        return false;
                    }
                    packed.swap(decrypted);
                }
                std::vector<std::uint8_t> unpacked;
                if (member.method == 0) {
                    unpacked.swap(packed);
                    if (unpacked.size() < member.unpackedSize ||
                        unpacked.size() - std::size_t(member.unpackedSize) > 15 ||
                        std::any_of(unpacked.begin() + std::size_t(member.unpackedSize),
                                    unpacked.end(),
                                    [](std::uint8_t value) { return value != 0; })) {
                        checked_[current] = 2;
                        return false;
                    }
                    unpacked.resize(std::size_t(member.unpackedSize));
                    const bool preserveHistory = current + 1 < members_.size() &&
                        members_[current + 1].solid;
                    if (preserveHistory) {
                        state.history.insert(state.history.end(), unpacked.begin(), unpacked.end());
                        if (state.history.size() > member.dictionarySize) {
                            const std::size_t keep = std::size_t(member.dictionarySize);
                            state.history.erase(state.history.begin(), state.history.end() - keep);
                        }
                    }
                } else if (!decodeRar5Member(packed.data(), packed.size(), member.unpackedSize,
                                              member.dictionarySize, member.extendedDistances,
                                              member.encrypted,
                                              current + 1 < members_.size() &&
                                                  members_[current + 1].solid,
                                              &state, &unpacked)) {
                    checked_[current] = 2;
                    return false;
                }
                std::uint32_t actualCrc = crc32(unpacked.data(), unpacked.size());
                if (member.encrypted && (member.encryptionFlags & 2U))
                    actualCrc = rar5Checksum(
                        reinterpret_cast<const std::uint8_t*>(checksumKey.constData()),
                        actualCrc);
                if (unpacked.size() != member.unpackedSize || actualCrc != member.crc) {
                    checked_[current] = 2;
                    return false;
                }
                if (current == index) {
                    decoded_[current] = std::move(unpacked);
                    checked_[current] = 1;
                }
            } catch (...) {
                checked_[current] = 2;
                return false;
            }
        }
        return true;
    }

    std::vector<Rar5MemberData> members_;
    QByteArray password_;
    mutable std::vector<std::vector<std::uint8_t>> decoded_;
    mutable std::vector<std::uint8_t> checked_;
    mutable std::mutex mutex_;
};

class Rar5SequenceMemberStore final : public fs::IByteStore {
public:
    Rar5SequenceMemberStore(std::shared_ptr<Rar5SolidSequence> sequence,
                            std::size_t index)
        : sequence_(std::move(sequence)), index_(index) {}

    std::int64_t capacity() const override
    {
        const std::uint64_t size = sequence_ ? sequence_->memberSize(index_) : 0;
        return size <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(size) : 0;
    }
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override { return sequence_ && sequence_->validateMember(index_); }
    int read(std::int64_t position, std::uint8_t* output, int count) const override
    {
        return sequence_ && sequence_->readMember(index_, position, output, count) ?
            int(std::min<std::uint64_t>(count, sequence_->memberSize(index_) -
                std::uint64_t(position))) : 0;
    }

private:
    std::shared_ptr<Rar5SolidSequence> sequence_;
    std::size_t index_;
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

ModulePtr Rar5Module::open(const fs::ByteStorePtr& initialSource, const QString& sourceName,
                           const QString& password)
{
    fs::ByteStorePtr source = initialSource;
    QString currentSourcePath = sourceName;
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
    bool encryptedHeaders = false;
    QByteArray headerKey;
    std::array<std::uint8_t, 16> headerSalt = {};
    std::uint8_t headerKdfCount = 0;
    std::vector<Rar5MemberData> members;
    std::vector<std::size_t> memberResourceIndices;
    std::size_t pendingMember = std::numeric_limits<std::size_t>::max();
    while (position >= 0 && position < source->capacity()) {
        std::uint8_t crcBytes[4];
        std::vector<std::uint8_t> crcData;
        std::uint64_t headerSize = 0;
        int sizeLength = 0;
        std::uint64_t physicalHeaderSize = 0;
        if (encryptedHeaders) {
            std::uint8_t iv[16];
            std::vector<std::uint8_t> firstBlock(16);
            if (!readExact(*source, position, iv, sizeof(iv)) ||
                !readExact(*source, position + 16, firstBlock.data(), 16) ||
                !decryptRar5AesCbc(headerKey, iv, &firstBlock)) {
                module->info_.error = QStringLiteral("Invalid encrypted RAR5 header block");
                return module;
            }
            Cursor firstSize(firstBlock.data() + 4, firstBlock.size() - 4);
            if (!firstSize.readVint(&headerSize) || headerSize == 0 ||
                headerSize > kMaximumHeaderSize) {
                module->info_.error = QStringLiteral("Invalid encrypted RAR5 header size");
                return module;
            }
            sizeLength = int(firstSize.position());
            const std::uint64_t plainHeaderSize =
                4 + std::uint64_t(sizeLength) + headerSize;
            const std::uint64_t encryptedSize = (plainHeaderSize + 15U) & ~15ULL;
            if (encryptedSize < firstBlock.size() ||
                encryptedSize > kMaximumHeaderSize + 32ULL ||
                std::uint64_t(position) + 16 + encryptedSize >
                    std::uint64_t(source->capacity())) {
                module->info_.error = QStringLiteral("Truncated encrypted RAR5 header");
                return module;
            }
            std::vector<std::uint8_t> encrypted(
                static_cast<std::size_t>(encryptedSize), 0);
            if (!readExact(*source, position + 16, encrypted.data(),
                           int(encrypted.size())) ||
                !decryptRar5AesCbc(headerKey, iv, &encrypted)) {
                module->info_.error = QStringLiteral("Unable to decrypt RAR5 header");
                return module;
            }
            std::copy(encrypted.begin(), encrypted.begin() + 4, crcBytes);
            crcData.assign(encrypted.begin() + 4,
                           encrypted.begin() + std::size_t(plainHeaderSize));
            physicalHeaderSize = 16 + encryptedSize;
        } else {
            if (!readExact(*source, position, crcBytes, sizeof(crcBytes))) break;
            std::uint8_t sizeBytes[10];
            for (; sizeLength < int(sizeof(sizeBytes)); ++sizeLength) {
                if (!readExact(*source, position + 4 + sizeLength,
                               sizeBytes + sizeLength, 1)) {
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
            crcData.resize(std::size_t(sizeLength) + std::size_t(headerSize));
            if (!readExact(*source, position + 4, crcData.data(),
                           int(crcData.size()))) {
                module->info_.error = QStringLiteral("Truncated RAR5 header");
                return module;
            }
            physicalHeaderSize = 4 + std::uint64_t(sizeLength) + headerSize;
        }
        if (crc32(crcData.data(), crcData.size()) != readLe32(crcBytes)) {
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
        const std::uint64_t dataOffset = std::uint64_t(position) + physicalHeaderSize;
        if (dataSize > std::uint64_t(source->capacity()) ||
            dataOffset > std::uint64_t(source->capacity()) - dataSize) {
            module->info_.error = QStringLiteral("RAR5 block data exceeds source bounds");
            return module;
        }
        if (type == 1) {
            sawMain = true;
        } else if (type == 4) {
            std::uint64_t version = 0;
            std::uint64_t encryptionFlags = 0;
            const std::uint8_t* salt = nullptr;
            QByteArray ignoredHashKey;
            if (encryptedHeaders || !cursor.readVint(&version) || version != 0 ||
                !cursor.readVint(&encryptionFlags) || (encryptionFlags & ~1ULL) ||
                !cursor.readByte(&headerKdfCount) ||
                !cursor.readBytes(headerSalt.size(), &salt)) {
                module->info_.error = QStringLiteral("Unsupported RAR5 header encryption");
                return module;
            }
            if (encryptionFlags & 1U) {
                const std::uint8_t* passwordCheck = nullptr;
                if (!cursor.readBytes(12, &passwordCheck)) {
                    module->info_.error = QStringLiteral("Truncated RAR5 header password check");
                    return module;
                }
            }
            if (password.isNull()) {
                module->info_.error = QStringLiteral("RAR5 encrypted headers require a password");
                return module;
            }
            std::copy(salt, salt + headerSalt.size(), headerSalt.begin());
            if (!deriveRar5Keys(password.toUtf8(), headerSalt.data(), headerKdfCount,
                                &headerKey, &ignoredHashKey)) {
                module->info_.error = QStringLiteral("Invalid RAR5 header encryption parameters");
                return module;
            }
            encryptedHeaders = true;
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
            Rar5MemberData member;
            member.encrypted = false;
            if (extraSize) {
                if (extraSize > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
                    module->info_.error = QStringLiteral("RAR5 extra area is too large");
                    return module;
                }
                const std::uint8_t* extraBytes = nullptr;
                if (!cursor.readBytes(std::size_t(extraSize), &extraBytes)) {
                    module->info_.error = QStringLiteral("Truncated RAR5 extra area");
                    return module;
                }
                Cursor extra(extraBytes, std::size_t(extraSize));
                while (extra.position() < std::size_t(extraSize)) {
                    std::uint64_t recordSize = 0;
                    if (!extra.readVint(&recordSize) || !recordSize ||
                        recordSize > std::size_t(extraSize) - extra.position()) {
                        module->info_.error = QStringLiteral("Malformed RAR5 extra record");
                        return module;
                    }
                    const std::size_t recordEnd = extra.position() + std::size_t(recordSize);
                    std::uint64_t recordType = 0;
                    if (!extra.readVint(&recordType)) {
                        module->info_.error = QStringLiteral("Malformed RAR5 extra record type");
                        return module;
                    }
                    if (recordType == 1) {
                        std::uint64_t version = 0;
                        std::uint64_t encryptionFlags = 0;
                        const std::uint8_t* salt = nullptr;
                        const std::uint8_t* iv = nullptr;
                        if (member.encrypted || !extra.readVint(&version) || version != 0 ||
                            !extra.readVint(&encryptionFlags) || (encryptionFlags & ~3ULL) ||
                            !extra.readByte(&member.kdfCount) ||
                            !extra.readBytes(member.salt.size(), &salt) ||
                            !extra.readBytes(member.iv.size(), &iv)) {
                            module->info_.error = QStringLiteral("Unsupported RAR5 encryption record");
                            return module;
                        }
                        if (encryptionFlags & 1U) {
                            const std::uint8_t* passwordCheck = nullptr;
                            if (!extra.readBytes(12, &passwordCheck)) {
                                module->info_.error = QStringLiteral("Truncated RAR5 password check");
                                return module;
                            }
                        }
                        member.encryptionFlags = std::uint8_t(encryptionFlags);
                        std::copy(salt, salt + member.salt.size(), member.salt.begin());
                        std::copy(iv, iv + member.iv.size(), member.iv.begin());
                        member.encrypted = true;
                    }
                    if (extra.position() > recordEnd) {
                        module->info_.error = QStringLiteral("Malformed RAR5 extra record size");
                        return module;
                    }
                    const std::size_t remaining = recordEnd - extra.position();
                    const std::uint8_t* ignored = nullptr;
                    if (remaining && !extra.readBytes(remaining, &ignored)) {
                        module->info_.error = QStringLiteral("Truncated RAR5 extra record data");
                        return module;
                    }
                }
            }
            const unsigned method = unsigned((compressionInfo >> 7) & 7U);
            const unsigned algorithmVersion = unsigned(compressionInfo & 0x3fU);
            const bool solid = (compressionInfo & 0x40U) != 0;
            const bool directory = (fileFlags & 1U) != 0;
            const bool splitBefore = (flags & 0x08U) != 0;
            const bool splitAfter = (flags & 0x10U) != 0;
            const bool split = splitBefore || splitAfter;
            const bool rar5Compatible = (compressionInfo & 0x100000U) != 0;
            if (!directory) {
                const unsigned dictionaryBits = unsigned((compressionInfo >> 10) & 0x1fU);
                const unsigned dictionaryFraction = unsigned((compressionInfo >> 15) & 0x1fU);
                if (algorithmVersion > 1 || method > 5 ||
                    (rar5Compatible && algorithmVersion != 1) ||
                    (algorithmVersion == 0 &&
                     (dictionaryBits > 15 || (compressionInfo & 0x1f8000U))) ||
                    (algorithmVersion == 1 && dictionaryBits > 23)) {
                    module->info_.error = QStringLiteral("Unsupported RAR5 compression parameters");
                    return module;
                }
                const std::uint64_t baseDictionary = (128ULL * 1024ULL) << dictionaryBits;
                const std::uint64_t extraDictionary = algorithmVersion == 1
                    ? (baseDictionary / 32) * dictionaryFraction : 0;
                const std::uint64_t maximumDictionary = 1ULL << 40;
                if (baseDictionary > maximumDictionary ||
                    extraDictionary > maximumDictionary - baseDictionary) {
                    module->info_.error = QStringLiteral("RAR7 dictionary exceeds 1 TiB");
                    return module;
                }
                member.algorithmVersion = algorithmVersion;
                member.extendedDistances = algorithmVersion == 1 && !rar5Compatible;
                member.dictionarySize = baseDictionary + extraDictionary;
                member.method = method;
                member.solid = solid;
            }
            if (directory && split) {
                module->info_.error = QStringLiteral("Split RAR5 directory entry");
                return module;
            }
            if (!directory && (fileFlags & 4U) == 0) {
                module->info_.error = QStringLiteral("RAR5 file entry has no CRC32");
                return module;
            }
            if (!directory && method == 0 && !split &&
                ((!member.encrypted && dataSize != unpacked) ||
                 (member.encrypted && (dataSize < unpacked || dataSize - unpacked > 15)))) {
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
            if (splitBefore) {
                if (pendingMember == std::numeric_limits<std::size_t>::max() ||
                    pendingMember >= members.size() ||
                    members[pendingMember].path != normalized ||
                    members[pendingMember].method != method ||
                    members[pendingMember].unpackedSize != unpacked ||
                    members[pendingMember].algorithmVersion != member.algorithmVersion ||
                    members[pendingMember].extendedDistances != member.extendedDistances ||
                    members[pendingMember].dictionarySize != member.dictionarySize ||
                    dataSize > std::numeric_limits<std::uint64_t>::max() -
                        members[pendingMember].packedSize) {
                    module->info_.error = QStringLiteral("Invalid RAR5 split member sequence");
                    return module;
                }
                Rar5MemberData& pending = members[pendingMember];
                if (member.encrypted && (!pending.encrypted ||
                    member.kdfCount != pending.kdfCount ||
                    member.salt != pending.salt || member.iv != pending.iv)) {
                    module->info_.error = QStringLiteral("Inconsistent RAR5 split encryption parameters");
                    return module;
                }
                if (member.encrypted)
                    pending.encryptionFlags = member.encryptionFlags;
                pending.chunks.push_back({source, std::int64_t(dataOffset), dataSize});
                pending.packedSize += dataSize;
                pending.crc = expectedCrc;
                if (!splitAfter) pendingMember = std::numeric_limits<std::size_t>::max();
                Q_UNUSED(attributes);
                Q_UNUSED(hostOs);
                Q_UNUSED(extraSize);
                position += std::int64_t(physicalHeaderSize + dataSize);
                continue;
            }
            if (pendingMember != std::numeric_limits<std::size_t>::max()) {
                module->info_.error = QStringLiteral("RAR5 split member has no continuation");
                return module;
            }
            ResourceEntry entry;
            entry.type = directory ? QStringLiteral("RAR5_DIR") : QStringLiteral("RAR5_FILE");
            entry.name = parts.last();
            entry.hierarchyPath = parts;
            entry.hierarchyPath.removeLast();
            entry.dataOffset = dataOffset;
            entry.dataSize = directory ? 0 : unpacked;
            entry.isDirectory = directory;
            entry.isEmbeddedFile = !directory;
            if (!directory) {
                member.path = normalized;
                member.chunks.push_back({source, std::int64_t(dataOffset), dataSize});
                member.packedSize = dataSize;
                member.unpackedSize = unpacked;
                member.crc = expectedCrc;
                memberResourceIndices.push_back(module->resources_.size());
                members.push_back(member);
                if (splitAfter) pendingMember = members.size() - 1;
            }
            module->resources_.push_back(std::move(entry));
            Q_UNUSED(attributes);
            Q_UNUSED(hostOs);
            Q_UNUSED(extraSize);
        } else if (type == 5) {
            std::uint64_t endFlags = 0;
            if (!cursor.readVint(&endFlags)) {
                module->info_.error = QStringLiteral("Malformed RAR5 end marker");
                return module;
            }
            if (endFlags & 1U) {
                const QString nextPath = nextPartVolumePath(currentSourcePath);
                if (nextPath.isEmpty()) {
                    module->info_.error = QStringLiteral("RAR5 volume set has no next part name");
                    return module;
                }
                auto nextSource = std::make_shared<fs::QFileByteStore>(nextPath);
                if (!nextSource->validate()) {
                    module->info_.error = QStringLiteral("RAR5 next volume is unavailable");
                    return module;
                }
                std::int64_t nextMarker = -1;
                const std::int64_t nextScanEnd = std::min<std::int64_t>(
                    nextSource->capacity() - std::int64_t(sizeof(kMarker)), 1024 * 1024);
                for (std::int64_t i = 0; i <= nextScanEnd; ++i) {
                    if (readExact(*nextSource, i, probe, sizeof(probe)) &&
                        std::equal(probe, probe + sizeof(probe), kMarker)) {
                        nextMarker = i;
                        break;
                    }
                }
                if (nextMarker < 0) {
                    module->info_.error = QStringLiteral("RAR5 next volume marker not found");
                    return module;
                }
                source = nextSource;
                currentSourcePath = nextPath;
                position = nextMarker + std::int64_t(sizeof(kMarker));
                encryptedHeaders = false;
                headerKey.fill(char(0));
                sawEnd = false;
                continue;
            }
            sawEnd = true;
            break;
        }
        position += std::int64_t(physicalHeaderSize + dataSize);
    }
    if (!members.empty()) {
        if (pendingMember != std::numeric_limits<std::size_t>::max()) {
            module->info_.error = QStringLiteral("RAR5 split member is incomplete");
            return module;
        }
        const auto sequence = std::make_shared<Rar5SolidSequence>(
            std::move(members), password.toUtf8());
        for (std::size_t index = 0; index < memberResourceIndices.size(); ++index)
            module->resources_[memberResourceIndices[index]].content =
                std::make_shared<Rar5SequenceMemberStore>(sequence, index);
    }
    if (!sawMain) module->info_.error = QStringLiteral("RAR5 main header not found");
    else if (!sawEnd) module->info_.error = QStringLiteral("RAR5 end marker not found");
    return module;
}

} // namespace peare
