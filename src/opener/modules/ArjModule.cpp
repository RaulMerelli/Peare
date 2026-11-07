#include "ArjModule.h"
#include "Compat.h"
#include "../compression/ArjGost40.h"
#include "../compression/Crc32.h"
#include "../compression/peare/LhaDecoder.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <cstring>
#include <limits>
#include <vector>

namespace peare {
namespace {

constexpr std::size_t kMinHeaderSize = 30;
constexpr std::size_t kMaxHeaderSize = 2600;
constexpr std::size_t kMaxEntries = 1000000;

struct HeaderBlock {
    std::vector<std::uint8_t> bytes;
    std::uint64_t next = 0;
};

struct ArjEntry {
    struct Segment {
        fs::ByteStorePtr file;
        std::uint64_t packedSize = 0;
        std::uint64_t unpackedSize = 0;
        std::uint64_t dataOffset = 0;
        std::uint64_t fileOffset = 0;
        std::uint32_t crc = 0;
        std::uint8_t method = 0;
        std::uint8_t flags = 0;
        std::uint8_t passwordModifier = 0;
        std::uint8_t encryptionVersion = 0;
        std::uint32_t fileTime = 0;
    };

    QStringList path;
    std::uint64_t packedSize = 0;
    std::uint64_t unpackedSize = 0;
    std::uint64_t dataOffset = 0;
    std::uint32_t crc = 0;
    std::uint8_t method = 0;
    std::uint8_t flags = 0;
    std::uint8_t fileType = 0;
    std::vector<Segment> segments;
};

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
}

std::uint16_t read16(const std::uint8_t* p)
{
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

bool readBlock(const fs::ByteStorePtr& file, std::uint64_t offset,
               bool hasSignature, HeaderBlock* block, bool* end)
{
    if (!file || !block || !end) return false;
    const std::uint64_t capacity = static_cast<std::uint64_t>(file->capacity());
    const std::uint64_t prefix = hasSignature ? 4 : 2;
    if (offset > capacity || capacity - offset < prefix) return false;
    const std::vector<std::uint8_t> head = file->readRange(
        static_cast<std::int64_t>(offset), static_cast<std::size_t>(prefix));
    if (head.size() != prefix) return false;
    if (hasSignature && (head[0] != 0x60 || head[1] != 0xEA)) return false;
    const std::uint16_t size = read16(head.data() + (hasSignature ? 2 : 0));
    if (size == 0) {
        *end = true;
        block->next = offset + prefix;
        block->bytes.clear();
        return true;
    }
    if (hasSignature && (size < kMinHeaderSize || size > kMaxHeaderSize)) return false;
    if (capacity - offset - prefix < std::uint64_t(size) + 4) return false;
    block->bytes = file->readRange(static_cast<std::int64_t>(offset + prefix), size);
    const std::vector<std::uint8_t> crcBytes = file->readRange(
        static_cast<std::int64_t>(offset + prefix + size), 4);
    if (block->bytes.size() != size || crcBytes.size() != 4 ||
        read32(crcBytes.data()) != compression::crc32(block->bytes.data(), block->bytes.size()))
        return false;
    block->next = offset + prefix + size + 4;
    *end = false;
    return true;
}

bool skipExtended(const fs::ByteStorePtr& file, std::uint64_t* offset)
{
    for (std::size_t count = 0; count < kMaxEntries; ++count) {
        HeaderBlock block;
        bool end = false;
        if (!readBlock(file, *offset, false, &block, &end)) return false;
        *offset = block.next;
        if (end) return true;
    }
    return false;
}

QStringList safePath(const std::uint8_t* bytes, std::size_t size)
{
    const std::uint8_t* nul = std::find(bytes, bytes + size, std::uint8_t(0));
    QString path = QString::fromLocal8Bit(reinterpret_cast<const char*>(bytes),
                                          int(nul - bytes));
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QStringList parts;
    const QString forbidden = QStringLiteral("<>:\"|?*");
    for (QString part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) part = QStringLiteral("_");
        for (QChar& c : part)
            if (c.unicode() < 0x20 || forbidden.contains(c)) c = QLatin1Char('_');
        if (!part.isEmpty()) parts.push_back(part);
    }
    return parts;
}

bool readEntries(const fs::ByteStorePtr& file, std::vector<ArjEntry>* entries,
                 bool* hasNextVolume)
{
    if (!file || !entries || !hasNextVolume) return false;
    std::uint64_t offset = 0;
    HeaderBlock mainHeader;
    bool end = false;
    if (!readBlock(file, offset, true, &mainHeader, &end) || end ||
        mainHeader.bytes.size() < kMinHeaderSize || mainHeader.bytes[0] < kMinHeaderSize ||
        mainHeader.bytes[6] != 2)
        return false;
    *hasNextVolume = (mainHeader.bytes[4] & 0x04U) != 0;
    const std::uint8_t encryptionVersion = mainHeader.bytes[28];
    offset = mainHeader.next;
    if (!skipExtended(file, &offset)) return false;

    while (offset < static_cast<std::uint64_t>(file->capacity())) {
        HeaderBlock header;
        if (!readBlock(file, offset, true, &header, &end)) return false;
        offset = header.next;
        if (end) return true;
        if (header.bytes.size() < kMinHeaderSize || header.bytes[0] < kMinHeaderSize ||
            header.bytes[0] > header.bytes.size()) return false;
        const std::uint8_t* fixed = header.bytes.data();
        ArjEntry entry;
        entry.flags = fixed[4];
        entry.method = fixed[5];
        entry.fileType = fixed[6];
        entry.packedSize = read32(fixed + 12);
        entry.unpackedSize = read32(fixed + 16);
        entry.crc = read32(fixed + 20);
        std::size_t nameOffset = fixed[0];
        if (nameOffset >= header.bytes.size()) return false;
        const std::uint8_t* nameEnd = std::find(header.bytes.data() + nameOffset,
            header.bytes.data() + header.bytes.size(), std::uint8_t(0));
        if (nameEnd == header.bytes.data() + header.bytes.size()) return false;
        entry.path = safePath(header.bytes.data() + nameOffset,
                              std::size_t(nameEnd - (header.bytes.data() + nameOffset)) + 1);
        if (entry.path.isEmpty()) return false;
        ArjEntry::Segment segment;
        segment.file = file;
        segment.packedSize = entry.packedSize;
        segment.unpackedSize = entry.unpackedSize;
        segment.crc = entry.crc;
        segment.method = entry.method;
        segment.flags = entry.flags;
        segment.passwordModifier = fixed[7];
        segment.encryptionVersion = encryptionVersion;
        segment.fileTime = read32(fixed + 8);
        if (entry.flags & 0x08U) {
            if (nameOffset < 34) return false;
            segment.fileOffset = read32(fixed + 30);
        }
        if (!skipExtended(file, &offset)) return false;
        entry.dataOffset = offset;
        if (entry.packedSize > static_cast<std::uint64_t>(file->capacity()) - offset)
            return false;
        offset += entry.packedSize;
        segment.dataOffset = entry.dataOffset;
        entry.segments.push_back(std::move(segment));
        if (entries->size() >= kMaxEntries) return false;
        entries->push_back(entry);
    }
    return false;
}

bool decodeSegment(const ArjEntry::Segment& segment, const QString& password,
                   std::vector<std::uint8_t>* output)
{
    if (!segment.file || !output || segment.unpackedSize > output->max_size() ||
        segment.packedSize > output->max_size()) return false;
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(segment.packedSize));
    std::uint64_t packedOffset = 0;
    while (packedOffset < segment.packedSize) {
        const int request = static_cast<int>(std::min<std::uint64_t>(
            segment.packedSize - packedOffset, 4U * 1024U * 1024U));
        const int count = segment.file->read(static_cast<std::int64_t>(segment.dataOffset + packedOffset),
                                     packed.data() + packedOffset, request);
        if (count != request) return false;
        packedOffset += static_cast<std::uint64_t>(count);
    }
    if ((segment.flags & 1U) && segment.encryptionVersion <= 1) {
        if (password.isNull() || password.isEmpty()) return false;
        const QByteArray passwordBytes = password.toLocal8Bit();
        if (passwordBytes.isEmpty()) return false;
        for (std::size_t i = 0; i < packed.size(); ++i) {
            const std::uint8_t passwordByte = static_cast<std::uint8_t>(
                passwordBytes.at(static_cast<int>(i % std::size_t(passwordBytes.size()))));
            packed[i] ^= static_cast<std::uint8_t>(passwordByte + segment.passwordModifier);
        }
    } else if ((segment.flags & 1U) && segment.encryptionVersion == 4) {
        if (!compression::decryptArjGost40(&packed, password.toLocal8Bit(),
                                           segment.passwordModifier, segment.fileTime))
            return false;
    } else if ((segment.flags & 1U) &&
               (segment.encryptionVersion == 2 || segment.encryptionVersion == 3)) {
        if (!compression::decryptArjGost256(&packed, password.toLocal8Bit(),
                                            segment.passwordModifier, segment.fileTime,
                                            segment.encryptionVersion == 3))
            return false;
    } else if (segment.flags & 1U) {
        return false;
    }
    if (segment.method == 0) {
        if (segment.packedSize != segment.unpackedSize) return false;
        *output = packed;
    } else if (segment.method >= 1 && segment.method <= 3) {
        if (!compression::decompressArjLzh(packed.data(), packed.size(),
                                           static_cast<std::size_t>(segment.unpackedSize), output))
            return false;
    } else if (segment.method == 4) {
        if (!compression::decompressArjMethod4(packed.data(), packed.size(),
                                               static_cast<std::size_t>(segment.unpackedSize), output))
            return false;
    } else {
        return false;
    }
    return output->size() == segment.unpackedSize &&
           compression::crc32(output->data(), output->size()) == segment.crc;
}

bool decodeEntry(const ArjEntry& entry, const QString& password,
                 std::vector<std::uint8_t>* output)
{
    if (!output || entry.unpackedSize > output->max_size()) return false;
    output->clear();
    for (const ArjEntry::Segment& segment : entry.segments) {
        std::vector<std::uint8_t> decoded;
        if (!decodeSegment(segment, password, &decoded) ||
            segment.fileOffset > output->max_size() ||
            segment.fileOffset > output->size()) return false;
        if (segment.fileOffset < output->size()) return false;
        output->insert(output->end(), decoded.begin(), decoded.end());
    }
    return output->size() == entry.unpackedSize;
}

class ArjMemberStore final : public fs::IByteStore {
public:
    ArjMemberStore(ArjEntry entry, QString password)
        : entry_(std::move(entry)), password_(std::move(password)) {}

    std::int64_t capacity() const override {
        return static_cast<std::int64_t>(entry_.unpackedSize);
    }

    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t offset, std::uint8_t* destination, int count) const override {
        if (offset < 0 || count <= 0 || !destination || offset >= capacity() || !validate())
            return 0;
        const std::size_t available = data_.size() - static_cast<std::size_t>(offset);
        const std::size_t amount = std::min<std::size_t>(available, static_cast<std::size_t>(count));
        std::memcpy(destination, data_.data() + offset, amount);
        return static_cast<int>(amount);
    }

    bool validate() const override {
        std::call_once(loadOnce_, [this] {
            try {
                valid_ = decodeEntry(entry_, password_, &data_);
            } catch (...) {
                data_.clear();
                valid_ = false;
            }
        });
        return valid_;
    }

private:
    ArjEntry entry_;
    QString password_;
    mutable std::once_flag loadOnce_;
    mutable std::vector<std::uint8_t> data_;
    mutable bool valid_ = false;
};

QString volumePath(const QString& currentPath, int nextVolume)
{
    const QFileInfo info(currentPath);
    const QString suffix = info.suffix();
    QString nextSuffix;
    if (suffix.compare(QStringLiteral("arj"), Qt::CaseInsensitive) == 0) {
        nextSuffix = QStringLiteral("a%1").arg(nextVolume, 2, 10, QLatin1Char('0'));
    } else if (suffix.size() == 3 && suffix[0].toLower() == QLatin1Char('a') &&
               suffix.mid(1).toInt() > 0) {
        const int current = suffix.mid(1).toInt();
        nextSuffix = current < 99
            ? QStringLiteral("a%1").arg(current + 1, 2, 10, QLatin1Char('0'))
            : QString::number(current + 1);
    } else if (suffix.size() == 3 && suffix.toInt() > 0) {
        nextSuffix = QStringLiteral("%1").arg(suffix.toInt() + 1, 3, 10, QLatin1Char('0'));
    } else {
        return QString();
    }
    return info.dir().filePath(info.completeBaseName() + QLatin1Char('.') + nextSuffix);
}

bool loadArchiveVolumes(const QString& firstPath, const fs::ByteStorePtr& firstStore,
                        std::vector<ArjEntry>* entries)
{
    std::map<QString, std::size_t> splitEntries;
    fs::ByteStorePtr store = firstStore;
    QString path = firstPath;
    int volume = 0;
    bool hasNext = false;
    do {
        std::vector<ArjEntry> volumeEntries;
        if (!readEntries(store, &volumeEntries, &hasNext)) return false;
        for (ArjEntry& item : volumeEntries) {
            const bool segmented = (item.flags & 0x0cU) != 0;
            const QString key = item.path.join(QLatin1Char('/'));
            const auto found = splitEntries.find(key);
            if (found != splitEntries.end()) {
                ArjEntry& combined = (*entries)[found->second];
                if (!(item.flags & 0x08U) || item.segments.size() != 1) return false;
                const ArjEntry::Segment& previous = combined.segments.back();
                const ArjEntry::Segment& current = item.segments.front();
                if (!(previous.flags & 0x04U) || current.fileOffset != combined.unpackedSize ||
                    combined.unpackedSize > std::numeric_limits<std::uint64_t>::max() -
                                                current.unpackedSize)
                    return false;
                combined.unpackedSize += current.unpackedSize;
                combined.packedSize += current.packedSize;
                combined.flags = current.flags;
                combined.segments.push_back(current);
                continue;
            }
            if (segmented && (item.flags & 0x08U)) return false;
            if (segmented && item.segments.size() != 1) return false;
            if (segmented) splitEntries.emplace(key, entries->size());
            entries->push_back(std::move(item));
        }
        if (!hasNext) break;
        if (++volume > 999) return false;
        const QString nextPath = volumePath(path, volume);
        if (nextPath.isEmpty() || !QFileInfo::exists(nextPath)) return false;
        path = nextPath;
        store = storeForFile(path);
        if (!store) return false;
    } while (hasNext);

    for (const auto& split : splitEntries) {
        const ArjEntry& item = (*entries)[split.second];
        if (item.segments.empty() || (item.segments.back().flags & 0x04U)) return false;
    }
    return true;
}

} // namespace

ModulePtr ArjModule::open(const QString& filePath, const QString& password)
{
    return open(storeForFile(filePath), filePath, password);
}

ModulePtr ArjModule::open(const QByteArray& data, const QString& logicalName,
                          const QString& password)
{
    return open(std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        static_cast<std::size_t>(data.size())), logicalName, password);
}

ModulePtr ArjModule::open(const fs::ByteStorePtr& file, const QString& sourceName,
                          const QString& password)
{
    auto module = peare::makeUnique<ArjModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ARJ;
    module->info_.description = QStringLiteral("ARJ archive");
    std::vector<ArjEntry> entries;
    const QFileInfo sourceInfo(sourceName);
    const bool hasFilesystemSource = sourceInfo.exists() && sourceInfo.isFile();
    bool hasNextVolume = false;
    const bool valid = file && (hasFilesystemSource
        ? loadArchiveVolumes(sourceName, file, &entries)
        : readEntries(file, &entries, &hasNextVolume) && !hasNextVolume);
    if (!valid) {
        module->info_.error = QStringLiteral("Invalid or damaged ARJ archive");
        return ModulePtr(std::move(module));
    }
    if (!hasFilesystemSource && std::any_of(entries.cbegin(), entries.cend(),
        [](const ArjEntry& item) { return (item.flags & 0x0cU) != 0; })) {
        module->info_.error = QStringLiteral("ARJ split archives require file-based volume access");
        return ModulePtr(std::move(module));
    }
    for (const ArjEntry& item : entries) {
        if ((item.flags & 1U) && (password.isNull() || password.isEmpty())) {
            module->info_.openError = ModuleOpenError::PasswordRequired;
            module->info_.error = QStringLiteral("ARJ encrypted members require a password");
            return ModulePtr(std::move(module));
        }
        if (item.fileType == 3) {
            ResourceEntry entry;
            entry.type = QStringLiteral("ARJ_DIR");
            entry.name = item.path.last();
            entry.hierarchyPath = item.path;
            entry.isDirectory = true;
            module->resources_.push_back(std::move(entry));
            continue;
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("ARJ_FILE");
        entry.name = item.path.last();
        entry.hierarchyPath = item.path;
        entry.dataSize = item.unpackedSize;
        entry.isEmbeddedFile = true;
        entry.format = ModuleFormat::ARJ;
        entry.content = std::make_shared<ArjMemberStore>(item, password);
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
