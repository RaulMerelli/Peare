#include "FfuModule.h"
#include "Compat.h"

#include <QFile>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kSectorSize = 512;
const std::uint32_t kStoreHeaderV1Size = 248;
const std::uint32_t kStoreHeaderV2FixedSize = 262;
const std::int64_t kMissingBlock = -1;

std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool checkedAdd(std::int64_t a, std::int64_t b, std::int64_t* result) {
    if (a < 0 || b < 0 || a > std::numeric_limits<std::int64_t>::max() - b)
        return false;
    *result = a + b;
    return true;
}

bool checkedMultiply(std::int64_t a, std::int64_t b, std::int64_t* result) {
    if (a < 0 || b < 0 || (a != 0 && b > std::numeric_limits<std::int64_t>::max() / a))
        return false;
    *result = a * b;
    return true;
}

bool roundUp(std::int64_t value, std::int64_t alignment, std::int64_t* result) {
    if (value < 0 || alignment <= 0) return false;
    const std::int64_t rem = value % alignment;
    if (rem == 0) {
        *result = value;
        return true;
    }
    return checkedAdd(value, alignment - rem, result);
}

bool readExact(const fs::ByteStorePtr& file, std::int64_t offset, int count,
               std::vector<std::uint8_t>* out) {
    if (!file || offset < 0 || count < 0 || offset > file->capacity() - count)
        return false;
    out->resize(static_cast<std::size_t>(count));
    return count == 0 || file->read(offset, out->data(), count) == count;
}

QString fixedAscii(const std::uint8_t* p, int length) {
    int end = 0;
    while (end < length && p[end] != 0) ++end;
    while (end > 0 && p[end - 1] == ' ') --end;
    return QString::fromLatin1(reinterpret_cast<const char*>(p), end);
}

QString utf16Name(const std::uint8_t* p, int byteLength) {
    QString result;
    result.reserve(byteLength / 2);
    for (int i = 0; i + 1 < byteLength; i += 2) {
        const ushort ch = le16(p + i);
        if (ch == 0) break;
        result.append(QChar(ch));
    }
    return result.trimmed();
}

QString guidTypeName(const std::uint8_t* guid) {
    // On disk, the first three GUID fields are little-endian.
    const std::uint32_t d1 = le32(guid);
    if (d1 == 0xC12A7328u) return QStringLiteral("EFI System");
    if (d1 == 0xEBD0A0A2u) return QStringLiteral("Microsoft Basic Data");
    if (d1 == 0xE3C9E316u) return QStringLiteral("Microsoft Reserved");
    if (d1 == 0xDE94BBA4u) return QStringLiteral("Windows Recovery");
    return QStringLiteral("GPT partition");
}

bool nonZeroGuid(const std::uint8_t* guid) {
    for (int i = 0; i < 16; ++i)
        if (guid[i] != 0) return true;
    return false;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

class FfuDiskStore final : public fs::IByteStore {
public:
    FfuDiskStore(fs::ByteStorePtr file, std::int64_t blockSize,
                 std::int64_t diskSize, std::vector<std::int64_t> blockOffsets)
        : file_(std::move(file)), blockSize_(blockSize), diskSize_(diskSize),
          blockOffsets_(std::move(blockOffsets)) {}

    std::int64_t capacity() const override { return diskSize_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!file_ || blockSize_ <= 0 || pos < 0 || count <= 0 || pos >= diskSize_)
            return 0;
        const std::int64_t available = diskSize_ - pos;
        int wanted = count < available ? count : static_cast<int>(available);
        int produced = 0;
        while (wanted > 0) {
            const std::int64_t logical = pos + produced;
            const std::int64_t blockIndex = logical / blockSize_;
            const std::int64_t within = logical % blockSize_;
            const int amount = static_cast<int>(std::min<std::int64_t>(blockSize_ - within, wanted));
            if (blockIndex < 0 || blockIndex >= static_cast<std::int64_t>(blockOffsets_.size()) ||
                blockOffsets_[static_cast<std::size_t>(blockIndex)] == kMissingBlock) {
                std::fill(dst + produced, dst + produced + amount, std::uint8_t(0));
            } else {
                const std::int64_t source = blockOffsets_[static_cast<std::size_t>(blockIndex)] + within;
                const int got = file_->read(source, dst + produced, amount);
                if (got < amount) {
                    if (got > 0) produced += got;
                    std::fill(dst + produced, dst + produced + (amount - std::max(got, 0)),
                              std::uint8_t(0));
                    produced += amount - std::max(got, 0);
                    wanted -= amount;
                    continue;
                }
            }
            produced += amount;
            wanted -= amount;
        }
        return produced;
    }

private:
    fs::ByteStorePtr file_;
    std::int64_t blockSize_;
    std::int64_t diskSize_;
    std::vector<std::int64_t> blockOffsets_;
};

struct Location {
    std::uint32_t method = 0;
    std::uint32_t blockIndex = 0;
};

struct Descriptor {
    std::uint32_t blockCount = 0;
    std::vector<Location> locations;
    std::uint64_t payloadBlockIndex = 0;
};

struct Store {
    QString platformId;
    QString devicePath;
    std::uint16_t index = 1;
    std::uint16_t count = 1;
    std::int64_t blockSize = 0;
    std::int64_t payloadOffset = 0;
    std::int64_t payloadBytes = 0;
    std::vector<Descriptor> descriptors;
};

struct ParsedFfu {
    std::int64_t chunkSize = 0;
    std::int64_t headersEnd = 0;
    std::vector<Store> stores;
};

bool parseStore(const fs::ByteStorePtr& file, std::int64_t offset,
                std::int64_t chunkSize, Store* store, std::int64_t* nextOffset,
                QString* error) {
    std::vector<std::uint8_t> first;
    if (!readExact(file, offset, int(kStoreHeaderV1Size), &first)) {
        *error = QStringLiteral("Truncated FFU store header");
        return false;
    }

    const std::uint16_t major = le16(first.data() + 4);
    const std::uint16_t minor = le16(first.data() + 6);
    if (major == 0 || major > 16) {
        *error = QStringLiteral("Unsupported FFU store header version %1.%2").arg(major).arg(minor);
        return false;
    }

    store->platformId = fixedAscii(first.data() + 12, 192);
    store->blockSize = le32(first.data() + 204);
    const std::uint32_t writeCount = le32(first.data() + 208);
    const std::uint32_t writeLength = le32(first.data() + 212);
    const std::uint32_t validateCount = le32(first.data() + 216);
    const std::uint32_t validateLength = le32(first.data() + 220);
    (void)validateCount;

    if (store->blockSize < 512 || store->blockSize > 64LL * 1024LL * 1024LL ||
        (store->blockSize % 512) != 0 || writeCount > 10000000u) {
        *error = QStringLiteral("Invalid FFU block size or descriptor count");
        return false;
    }

    std::uint32_t fixedSize = kStoreHeaderV1Size;
    if (major > 1) {
        std::vector<std::uint8_t> v2;
        if (!readExact(file, offset, int(kStoreHeaderV2FixedSize), &v2)) {
            *error = QStringLiteral("Truncated FFU v2 store header");
            return false;
        }
        store->count = le16(v2.data() + 248);
        store->index = le16(v2.data() + 250);
        const std::uint64_t declaredPayload = le64(v2.data() + 252);
        const std::uint16_t pathLength = le16(v2.data() + 260);
        if (store->count == 0 || store->count > 256 || store->index == 0 ||
            store->index > store->count || pathLength > 32767) {
            *error = QStringLiteral("Invalid FFU v2 store metadata");
            return false;
        }
        std::int64_t pathBytes = 0;
        if (!checkedMultiply(pathLength, 2, &pathBytes) ||
            pathBytes > std::numeric_limits<std::uint32_t>::max() - kStoreHeaderV2FixedSize) {
            *error = QStringLiteral("Invalid FFU device path length");
            return false;
        }
        fixedSize = kStoreHeaderV2FixedSize + static_cast<std::uint32_t>(pathBytes);
        if (pathBytes > 0) {
            std::vector<std::uint8_t> path;
            if (!readExact(file, offset + kStoreHeaderV2FixedSize, int(pathBytes), &path)) {
                *error = QStringLiteral("Truncated FFU device path");
                return false;
            }
            store->devicePath = utf16Name(path.data(), int(path.size()));
        }
        if (declaredPayload > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
            *error = QStringLiteral("FFU store payload is too large");
            return false;
        }
        store->payloadBytes = static_cast<std::int64_t>(declaredPayload);
    }

    std::int64_t descriptorsBytes = 0;
    if (!checkedAdd(validateLength, writeLength, &descriptorsBytes) ||
        descriptorsBytes > 512LL * 1024LL * 1024LL) {
        *error = QStringLiteral("Invalid FFU descriptor lengths");
        return false;
    }
    std::int64_t totalHeaderBytes = 0;
    if (!checkedAdd(fixedSize, descriptorsBytes, &totalHeaderBytes) ||
        offset > file->capacity() - totalHeaderBytes) {
        *error = QStringLiteral("Truncated FFU store descriptors");
        return false;
    }

    std::vector<std::uint8_t> write;
    if (writeLength > 0 && !readExact(file, offset + fixedSize + validateLength,
                                      int(writeLength), &write)) {
        *error = QStringLiteral("Truncated FFU write descriptors");
        return false;
    }

    std::size_t cursor = 0;
    std::uint64_t payloadBlock = 0;
    store->descriptors.reserve(writeCount);
    for (std::uint32_t i = 0; i < writeCount; ++i) {
        if (cursor + 8 > write.size()) {
            *error = QStringLiteral("FFU write descriptor table ends early");
            return false;
        }
        const std::uint32_t locationCount = le32(write.data() + cursor);
        const std::uint32_t blockCount = le32(write.data() + cursor + 4);
        cursor += 8;
        if (locationCount == 0 || locationCount > 65536 || blockCount == 0 ||
            cursor + std::size_t(locationCount) * 8 > write.size()) {
            *error = QStringLiteral("Invalid FFU write descriptor");
            return false;
        }
        Descriptor descriptor;
        descriptor.blockCount = blockCount;
        descriptor.payloadBlockIndex = payloadBlock;
        descriptor.locations.reserve(locationCount);
        for (std::uint32_t j = 0; j < locationCount; ++j) {
            Location location;
            location.method = le32(write.data() + cursor);
            location.blockIndex = le32(write.data() + cursor + 4);
            cursor += 8;
            if (location.method != 0 && location.method != 2) {
                *error = QStringLiteral("Unsupported FFU disk access method %1")
                             .arg(location.method);
                return false;
            }
            descriptor.locations.push_back(location);
        }
        if (payloadBlock > std::numeric_limits<std::uint64_t>::max() - blockCount) {
            *error = QStringLiteral("FFU payload block count overflow");
            return false;
        }
        payloadBlock += blockCount;
        store->descriptors.push_back(std::move(descriptor));
    }
    if (cursor > write.size()) {
        *error = QStringLiteral("Invalid FFU write descriptor length");
        return false;
    }

    std::int64_t computedPayload = 0;
    if (payloadBlock > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
        !checkedMultiply(static_cast<std::int64_t>(payloadBlock), store->blockSize,
                         &computedPayload)) {
        *error = QStringLiteral("FFU payload size overflow");
        return false;
    }
    if (store->payloadBytes != 0 && store->payloadBytes != computedPayload) {
        // Version 2 explicitly records payload bytes. A mismatch would shift all
        // following stores and make every partition unsafe to expose.
        *error = QStringLiteral("FFU store payload size does not match descriptors");
        return false;
    }
    store->payloadBytes = computedPayload;

    if (!roundUp(totalHeaderBytes, chunkSize, &totalHeaderBytes) ||
        !checkedAdd(offset, totalHeaderBytes, nextOffset) || *nextOffset > file->capacity()) {
        *error = QStringLiteral("Invalid FFU store header alignment");
        return false;
    }
    return true;
}

bool parseFfu(const fs::ByteStorePtr& file, ParsedFfu* result, QString* error) {
    std::vector<std::uint8_t> security;
    if (!readExact(file, 0, 32, &security) ||
        std::memcmp(security.data() + 4, "SignedImage ", 12) != 0) {
        *error = QStringLiteral("Invalid FFU security signature");
        return false;
    }

    const std::uint32_t securitySize = le32(security.data());
    const std::uint32_t chunkSizeKb = le32(security.data() + 16);
    const std::uint32_t catalogSize = le32(security.data() + 24);
    const std::uint32_t hashTableSize = le32(security.data() + 28);
    if (securitySize < 32 || chunkSizeKb == 0 || chunkSizeKb > 65536) {
        *error = QStringLiteral("Invalid FFU security header");
        return false;
    }
    result->chunkSize = std::int64_t(chunkSizeKb) * 1024;
    if ((result->chunkSize % 512) != 0) {
        *error = QStringLiteral("FFU chunk size is not sector-aligned");
        return false;
    }

    std::int64_t securityContent = 0;
    if (!checkedAdd(securitySize, catalogSize, &securityContent) ||
        !checkedAdd(securityContent, hashTableSize, &securityContent) ||
        !roundUp(securityContent, result->chunkSize, &securityContent) ||
        securityContent > file->capacity()) {
        *error = QStringLiteral("Invalid FFU security region");
        return false;
    }

    std::vector<std::uint8_t> image;
    if (!readExact(file, securityContent, 24, &image) ||
        std::memcmp(image.data() + 4, "ImageFlash  ", 12) != 0) {
        *error = QStringLiteral("Invalid FFU image signature");
        return false;
    }
    const std::uint32_t imageSize = le32(image.data());
    const std::uint32_t manifestSize = le32(image.data() + 16);
    if (imageSize < 24) {
        *error = QStringLiteral("Invalid FFU image header");
        return false;
    }
    std::int64_t imageContent = 0;
    if (!checkedAdd(imageSize, manifestSize, &imageContent) ||
        !roundUp(imageContent, result->chunkSize, &imageContent)) {
        *error = QStringLiteral("Invalid FFU image region");
        return false;
    }

    std::int64_t storeOffset = 0;
    if (!checkedAdd(securityContent, imageContent, &storeOffset) ||
        storeOffset >= file->capacity()) {
        *error = QStringLiteral("Missing FFU store header");
        return false;
    }

    std::uint16_t expectedStores = 1;
    do {
        Store store;
        std::int64_t next = 0;
        if (!parseStore(file, storeOffset, result->chunkSize, &store, &next, error))
            return false;
        if (result->stores.empty()) expectedStores = store.count;
        if (store.count != expectedStores || store.index != result->stores.size() + 1) {
            *error = QStringLiteral("Inconsistent FFU store sequence");
            return false;
        }
        result->stores.push_back(std::move(store));
        storeOffset = next;
    } while (result->stores.size() < expectedStores);

    result->headersEnd = storeOffset;
    std::int64_t payloadOffset = storeOffset;
    for (Store& store : result->stores) {
        store.payloadOffset = payloadOffset;
        if (!checkedAdd(payloadOffset, store.payloadBytes, &payloadOffset) ||
            payloadOffset > file->capacity()) {
            *error = QStringLiteral("Truncated FFU payload");
            return false;
        }
    }
    // WPinternals requires exact size. Permit only chunk-alignment padding after
    // the payload, because some generators pad the final store to a hash chunk.
    if (payloadOffset != file->capacity()) {
        std::int64_t padded = 0;
        if (!roundUp(payloadOffset, result->chunkSize, &padded) || padded != file->capacity()) {
            *error = QStringLiteral("Unexpected data after FFU payload");
            return false;
        }
    }
    return true;
}

std::int64_t diskSizeFromGpt(const fs::ByteStorePtr& provisional) {
    if (!provisional || provisional->capacity() < 1024) return 0;
    std::vector<std::uint8_t> header = provisional->readRange(512, 512);
    if (header.size() < 92 || std::memcmp(header.data(), "EFI PART", 8) != 0)
        return 0;
    const std::uint64_t alternateLba = le64(header.data() + 32);
    if (alternateLba == 0 || alternateLba > std::uint64_t(std::numeric_limits<std::int64_t>::max() / 512) - 1)
        return 0;
    return static_cast<std::int64_t>(alternateLba + 1) * 512;
}

fs::ByteStorePtr buildDisk(const fs::ByteStorePtr& file, const Store& store,
                           QString* error) {
    std::uint64_t highestBegin = 0;
    bool haveBegin = false;
    for (const Descriptor& descriptor : store.descriptors) {
        for (const Location& location : descriptor.locations) {
            if (location.method != 0) continue;
            const std::uint64_t end = std::uint64_t(location.blockIndex) + descriptor.blockCount;
            if (end > highestBegin) highestBegin = end;
            haveBegin = true;
        }
    }
    if (!haveBegin || highestBegin == 0 ||
        highestBegin > std::uint64_t(std::numeric_limits<std::int64_t>::max() / store.blockSize)) {
        *error = QStringLiteral("FFU store has no beginning-relative blocks");
        return nullptr;
    }

    const std::int64_t provisionalSize = static_cast<std::int64_t>(highestBegin) * store.blockSize;
    std::vector<std::int64_t> provisionalMap(static_cast<std::size_t>(highestBegin), kMissingBlock);
    for (const Descriptor& descriptor : store.descriptors) {
        for (const Location& location : descriptor.locations) {
            if (location.method != 0) continue;
            for (std::uint32_t k = 0; k < descriptor.blockCount; ++k) {
                const std::uint64_t logical = std::uint64_t(location.blockIndex) + k;
                if (logical >= provisionalMap.size()) continue;
                provisionalMap[static_cast<std::size_t>(logical)] =
                    store.payloadOffset +
                    static_cast<std::int64_t>(descriptor.payloadBlockIndex + k) * store.blockSize;
            }
        }
    }
    fs::ByteStorePtr provisional = std::make_shared<FfuDiskStore>(
        file, store.blockSize, provisionalSize, provisionalMap);
    std::int64_t diskSize = diskSizeFromGpt(provisional);
    if (diskSize <= 0) diskSize = provisionalSize;
    const std::int64_t logicalBlocks = (diskSize + store.blockSize - 1) / store.blockSize;
    if (logicalBlocks <= 0 || logicalBlocks > 100000000) {
        *error = QStringLiteral("FFU target disk size is invalid");
        return nullptr;
    }

    std::vector<std::int64_t> map(static_cast<std::size_t>(logicalBlocks), kMissingBlock);
    for (const Descriptor& descriptor : store.descriptors) {
        for (const Location& location : descriptor.locations) {
            std::int64_t target = 0;
            if (location.method == 0) {
                target = location.blockIndex;
            } else {
                // DISK_END block zero means the last block. For a multi-block
                // descriptor, preserve payload order in the contiguous run.
                target = logicalBlocks - std::int64_t(location.blockIndex) - descriptor.blockCount;
            }
            if (target < 0 || target > logicalBlocks - descriptor.blockCount) {
                *error = QStringLiteral("FFU write descriptor targets outside the disk");
                return nullptr;
            }
            for (std::uint32_t k = 0; k < descriptor.blockCount; ++k) {
                map[static_cast<std::size_t>(target + k)] =
                    store.payloadOffset +
                    static_cast<std::int64_t>(descriptor.payloadBlockIndex + k) * store.blockSize;
            }
        }
    }
    return std::make_shared<FfuDiskStore>(file, store.blockSize, diskSize, std::move(map));
}

bool appendGptPartitions(const fs::ByteStorePtr& disk, const Store& store,
                         bool multipleStores, QVector<ResourceEntry>* resources,
                         QString* error) {
    std::vector<std::uint8_t> header = disk->readRange(512, 512);
    if (header.size() < 92 || std::memcmp(header.data(), "EFI PART", 8) != 0) {
        *error = QStringLiteral("FFU virtual disk has no GPT header");
        return false;
    }
    const std::uint64_t entryLba = le64(header.data() + 72);
    const std::uint32_t count = le32(header.data() + 80);
    const std::uint32_t entrySize = le32(header.data() + 84);
    if (count == 0 || count > 4096 || entrySize < 128 || entrySize > 4096) {
        *error = QStringLiteral("Invalid GPT partition table in FFU");
        return false;
    }
    std::int64_t tableBytes = 0;
    if (!checkedMultiply(count, entrySize, &tableBytes) ||
        entryLba > std::uint64_t(std::numeric_limits<std::int64_t>::max() / 512)) {
        *error = QStringLiteral("GPT partition table is too large");
        return false;
    }
    std::vector<std::uint8_t> table = disk->readRange(
        static_cast<std::int64_t>(entryLba) * 512, tableBytes);
    if (table.size() != static_cast<std::size_t>(tableBytes)) {
        *error = QStringLiteral("Truncated GPT partition table in FFU");
        return false;
    }

    int visibleIndex = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* entry = table.data() + std::size_t(i) * entrySize;
        if (!nonZeroGuid(entry)) continue;
        const std::uint64_t first = le64(entry + 32);
        const std::uint64_t last = le64(entry + 40);
        if (last < first || first > std::uint64_t(std::numeric_limits<std::int64_t>::max() / 512))
            continue;
        const std::uint64_t sectors = last - first + 1;
        if (sectors > std::uint64_t(std::numeric_limits<std::int64_t>::max() / 512))
            continue;
        const std::int64_t offset = static_cast<std::int64_t>(first) * 512;
        const std::int64_t length = static_cast<std::int64_t>(sectors) * 512;
        if (offset < 0 || length <= 0 || offset > disk->capacity() - length)
            continue;

        QString name = utf16Name(entry + 56, std::min<std::uint32_t>(72, entrySize - 56));
        const QString type = guidTypeName(entry);
        ++visibleIndex;
        if (name.isEmpty()) name = QStringLiteral("Partition %1").arg(visibleIndex);

        ResourceEntry resource;
        resource.type = QStringLiteral("DISK_PARTITION");
        resource.name = name;
        resource.language = QStringLiteral("neutral");
        resource.dataOffset = quint64(offset);
        resource.dataSize = quint64(length);
        resource.format = ModuleFormat::FFU;
        resource.isEmbeddedFile = true;
        resource.content = std::make_shared<fs::SubStore>(disk, offset, length);
        if (multipleStores) {
            QString storeName = QStringLiteral("Store %1").arg(store.index);
            if (!store.devicePath.isEmpty()) storeName += QStringLiteral(" (%1)").arg(store.devicePath);
            resource.hierarchyPath << storeName;
        }
        resources->push_back(std::move(resource));
    }
    if (visibleIndex == 0) {
        *error = QStringLiteral("FFU GPT contains no partitions");
        return false;
    }
    return true;
}

} // namespace

ModulePtr FfuModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<FfuModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::FFU;
    module->info_.description = QStringLiteral("Microsoft Full Flash Update (FFU)");
    module->file_ = file;
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open FFU file");
        return ModulePtr(std::move(module));
    }

    ParsedFfu parsed;
    QString error;
    if (!parseFfu(file, &parsed, &error)) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }

    if (!parsed.stores.empty() && !parsed.stores.front().platformId.isEmpty())
        module->info_.description += QStringLiteral(" — %1").arg(parsed.stores.front().platformId);

    const bool multipleStores = parsed.stores.size() > 1;
    for (const Store& store : parsed.stores) {
        fs::ByteStorePtr disk = buildDisk(file, store, &error);
        if (!disk) {
            module->info_.error = error;
            module->resources_.clear();
            module->disks_.clear();
            return ModulePtr(std::move(module));
        }
        module->disks_.push_back(disk);
        if (!appendGptPartitions(disk, store, multipleStores, &module->resources_, &error)) {
            module->info_.error = error;
            module->resources_.clear();
            module->disks_.clear();
            return ModulePtr(std::move(module));
        }
    }
    return ModulePtr(std::move(module));
}

ModulePtr FfuModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<FfuModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::FFU;
        module->info_.description = QStringLiteral("Microsoft Full Flash Update (FFU)");
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

} // namespace peare
