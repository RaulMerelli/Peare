#include "AndroidSuperStore.h"

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <limits>
#include <set>

namespace peare {
namespace fs {
namespace {

constexpr std::uint32_t kGeometryMagic = 0x616c4467U;
constexpr std::uint32_t kHeaderMagic = 0x414c5030U;
constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint64_t kGeometrySize = 4096;
constexpr std::uint32_t kPartitionDisabled = 1U << 3;
constexpr std::uint32_t kTargetLinear = 0;
constexpr std::uint32_t kTargetZero = 1;
constexpr std::size_t kGeometryStructMin = 52;
constexpr std::size_t kHeaderV1Size = 128;
constexpr std::size_t kPartitionEntryMin = 52;
constexpr std::size_t kExtentEntryMin = 24;
constexpr std::size_t kGroupEntryMin = 48;
constexpr std::size_t kBlockDeviceEntryMin = 64;
constexpr std::uint64_t kMaxMetadataSize = 64ULL * 1024 * 1024;
constexpr std::uint32_t kMaxMetadataSlots = 16;

std::uint16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(readLe32(p)) |
        (static_cast<std::uint64_t>(readLe32(p + 4)) << 32);
}

bool readExact(const ByteStorePtr& source, std::int64_t offset,
               std::uint8_t* dst, std::size_t size) {
    if (!source || offset < 0 || size > static_cast<std::size_t>(INT_MAX))
        return false;
    std::size_t done = 0;
    while (done < size) {
        const int request = static_cast<int>(size - done);
        const int count = source->read(offset + static_cast<std::int64_t>(done),
                                       dst + done, request);
        if (count <= 0 || count > request) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool within(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

bool digestMatches(QByteArray bytes, std::size_t digestOffset,
                   const std::uint8_t* expected) {
    if (digestOffset > static_cast<std::size_t>(bytes.size()) ||
        32 > static_cast<std::size_t>(bytes.size()) - digestOffset)
        return false;
    std::memset(bytes.data() + digestOffset, 0, 32);
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    return digest.size() == 32 && std::memcmp(digest.constData(), expected, 32) == 0;
}

struct Geometry {
    std::uint64_t offset = 0;
    std::uint64_t metadataStart = 0;
    std::uint32_t metadataSize = 0;
    std::uint32_t metadataSlots = 0;
};

bool readGeometry(const ByteStorePtr& source, std::uint64_t offset, Geometry* geometry) {
    if (!source || !geometry || !within(offset, kGeometryStructMin,
            static_cast<std::uint64_t>(source->capacity())))
        return false;
    std::uint8_t prefix[kGeometryStructMin]{};
    if (!readExact(source, static_cast<std::int64_t>(offset), prefix, sizeof(prefix)) ||
        readLe32(prefix) != kGeometryMagic)
        return false;
    const std::uint32_t structSize = readLe32(prefix + 4);
    const std::uint32_t metadataSize = readLe32(prefix + 40);
    const std::uint32_t metadataSlots = readLe32(prefix + 44);
    const std::uint32_t blockSize = readLe32(prefix + 48);
    if (structSize < kGeometryStructMin || structSize > kGeometrySize ||
        !within(offset, structSize, static_cast<std::uint64_t>(source->capacity())) ||
        metadataSize < kSectorSize || metadataSize > kMaxMetadataSize ||
        metadataSize % kSectorSize != 0 || metadataSlots == 0 ||
        metadataSlots > kMaxMetadataSlots || blockSize < kSectorSize ||
        blockSize % kSectorSize != 0)
        return false;
    QByteArray bytes(static_cast<int>(structSize), Qt::Uninitialized);
    if (!readExact(source, static_cast<std::int64_t>(offset),
                   reinterpret_cast<std::uint8_t*>(bytes.data()), structSize) ||
        !digestMatches(bytes, 8, prefix + 8))
        return false;
    geometry->offset = offset;
    geometry->metadataStart = offset == 0 ? kGeometrySize : kGeometrySize * 3;
    geometry->metadataSize = metadataSize;
    geometry->metadataSlots = metadataSlots;
    return true;
}

struct Table {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    std::uint32_t entrySize = 0;
};

Table readTable(const std::uint8_t* p) {
    return {readLe32(p), readLe32(p + 4), readLe32(p + 8)};
}

bool validTable(const Table& table, std::uint32_t tablesSize,
                std::size_t minimumEntrySize) {
    if (table.count == 0)
        return table.entrySize >= minimumEntrySize && table.offset <= tablesSize;
    if (table.entrySize < minimumEntrySize) return false;
    const std::uint64_t length = static_cast<std::uint64_t>(table.count) * table.entrySize;
    return within(table.offset, length, tablesSize);
}

std::string readName(const std::uint8_t* bytes, std::size_t size) {
    const void* end = std::memchr(bytes, 0, size);
    const std::size_t length = end
        ? static_cast<const std::uint8_t*>(end) - bytes : size;
    return std::string(reinterpret_cast<const char*>(bytes), length);
}

bool parseMetadata(const ByteStorePtr& source, const Geometry& geometry,
                   std::uint32_t slot, std::vector<AndroidLogicalPartition>* partitions) {
    if (!partitions || slot >= geometry.metadataSlots) return false;
    const std::uint64_t primaryBase = geometry.metadataStart;
    const std::uint64_t slotsBytes = static_cast<std::uint64_t>(geometry.metadataSize) *
        geometry.metadataSlots;
    const std::uint64_t copies[2] = {primaryBase, primaryBase + slotsBytes};
    for (std::uint64_t copyBase : copies) {
        const std::uint64_t metadataOffset = copyBase +
            static_cast<std::uint64_t>(slot) * geometry.metadataSize;
        if (!within(metadataOffset, kHeaderV1Size,
                    static_cast<std::uint64_t>(source->capacity())))
            continue;
        std::uint8_t fixedHeader[kHeaderV1Size]{};
        if (!readExact(source, static_cast<std::int64_t>(metadataOffset),
                       fixedHeader, sizeof(fixedHeader)) ||
            readLe32(fixedHeader) != kHeaderMagic || readLe16(fixedHeader + 4) != 10 ||
            readLe16(fixedHeader + 6) > 2)
            continue;
        const std::uint32_t headerSize = readLe32(fixedHeader + 8);
        const std::uint32_t tablesSize = readLe32(fixedHeader + 44);
        if (headerSize < kHeaderV1Size || headerSize > geometry.metadataSize ||
            tablesSize > geometry.metadataSize - headerSize)
            continue;
        QByteArray header(static_cast<int>(headerSize), Qt::Uninitialized);
        if (!readExact(source, static_cast<std::int64_t>(metadataOffset),
                       reinterpret_cast<std::uint8_t*>(header.data()), headerSize) ||
            !digestMatches(header, 12, fixedHeader + 12))
            continue;
        QByteArray tables(static_cast<int>(tablesSize), Qt::Uninitialized);
        const std::uint64_t tablesOffset = metadataOffset + headerSize;
        if ((tablesSize && !readExact(source, static_cast<std::int64_t>(tablesOffset),
                reinterpret_cast<std::uint8_t*>(tables.data()), tablesSize)) ||
            QCryptographicHash::hash(tables, QCryptographicHash::Sha256) !=
                QByteArray(reinterpret_cast<const char*>(fixedHeader + 48), 32))
            continue;
        const std::uint8_t* headerBytes =
            reinterpret_cast<const std::uint8_t*>(header.constData());
        const Table partitionTable = readTable(headerBytes + 80);
        const Table extentTable = readTable(headerBytes + 92);
        const Table groupTable = readTable(headerBytes + 104);
        const Table deviceTable = readTable(headerBytes + 116);
        if (!validTable(partitionTable, tablesSize, kPartitionEntryMin) ||
            !validTable(extentTable, tablesSize, kExtentEntryMin) ||
            !validTable(groupTable, tablesSize, kGroupEntryMin) ||
            !validTable(deviceTable, tablesSize, kBlockDeviceEntryMin) ||
            deviceTable.count == 0)
            continue;
        std::array<Table, 4> declaredTables = {
            partitionTable, extentTable, groupTable, deviceTable};
        std::sort(declaredTables.begin(), declaredTables.end(),
            [](const Table& left, const Table& right) {
                return left.offset < right.offset;
            });
        std::uint64_t tableCursor = 0;
        bool tablesContiguous = true;
        for (const Table& table : declaredTables) {
            if (table.count == 0) continue;
            const std::uint64_t tableBytes =
                static_cast<std::uint64_t>(table.count) * table.entrySize;
            if (table.offset != tableCursor) {
                tablesContiguous = false;
                break;
            }
            tableCursor += tableBytes;
        }
        if (!tablesContiguous || tableCursor != tablesSize) continue;

        const auto* tableBytes = reinterpret_cast<const std::uint8_t*>(tables.constData());
        const auto* partitionBytes = tableBytes + partitionTable.offset;
        const auto* extentBytes = tableBytes + extentTable.offset;
        const auto* groupBytes = tableBytes + groupTable.offset;
        const auto* deviceBytes = tableBytes + deviceTable.offset;
        std::vector<std::uint64_t> deviceSizes;
        deviceSizes.reserve(deviceTable.count);
        for (std::uint32_t i = 0; i < deviceTable.count; ++i) {
            const std::uint8_t* device = deviceBytes +
                static_cast<std::size_t>(i) * deviceTable.entrySize;
            deviceSizes.push_back(readLe64(device + 16));
        }
        std::vector<AndroidLogicalPartition> parsed;
        std::set<std::string> names;
        bool valid = true;
        for (std::uint32_t i = 0; i < partitionTable.count && valid; ++i) {
            const std::uint8_t* partition = partitionBytes +
                static_cast<std::size_t>(i) * partitionTable.entrySize;
            const std::string name = readName(partition, 36);
            const std::uint32_t attributes = readLe32(partition + 36);
            const std::uint32_t firstExtent = readLe32(partition + 40);
            const std::uint32_t extentCount = readLe32(partition + 44);
            const std::uint32_t groupIndex = readLe32(partition + 48);
            const bool validName = !name.empty() && std::all_of(name.begin(), name.end(),
                [](unsigned char ch) {
                    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '_';
                });
            if (!validName || (attributes & ~0x0fU) != 0 ||
                !names.insert(name).second || groupIndex >= groupTable.count ||
                firstExtent > extentTable.count ||
                extentCount > extentTable.count - firstExtent) {
                valid = false;
                break;
            }
            if (attributes & kPartitionDisabled) continue;
            std::vector<ByteStorePtr> parts;
            std::uint64_t totalSize = 0;
            for (std::uint32_t j = 0; j < extentCount; ++j) {
                const std::uint8_t* extent = extentBytes +
                    static_cast<std::size_t>(firstExtent + j) * extentTable.entrySize;
                const std::uint64_t sectors = readLe64(extent);
                const std::uint32_t targetType = readLe32(extent + 8);
                const std::uint64_t targetData = readLe64(extent + 12);
                const std::uint32_t targetSource = readLe32(extent + 20);
                if (sectors > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) / kSectorSize) {
                    valid = false;
                    break;
                }
                const std::uint64_t extentSize = sectors * kSectorSize;
                if (extentSize > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) - totalSize) {
                    valid = false;
                    break;
                }
                totalSize += extentSize;
                if (targetType == kTargetZero) {
                    if (targetData != 0 || targetSource != 0) {
                        valid = false;
                        break;
                    }
                    parts.push_back(std::make_shared<ZeroStore>(
                        static_cast<std::int64_t>(extentSize)));
                } else if (targetType == kTargetLinear) {
                    if (targetSource != 0 || targetData >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                            kSectorSize) {
                        valid = false;
                        break;
                    }
                    const std::uint64_t sourceOffset = targetData * kSectorSize;
                    if (!within(sourceOffset, extentSize, deviceSizes[0]) ||
                        !within(sourceOffset, extentSize,
                            static_cast<std::uint64_t>(source->capacity()))) {
                        valid = false;
                        break;
                    }
                    parts.push_back(std::make_shared<SubStore>(source,
                        static_cast<std::int64_t>(sourceOffset),
                        static_cast<std::int64_t>(extentSize)));
                } else {
                    valid = false;
                    break;
                }
            }
            if (!valid) break;
            parsed.push_back({name, attributes, std::make_shared<ConcatStore>(std::move(parts))});
        }
        if (valid) {
            *partitions = std::move(parsed);
            return true;
        }
    }
    return false;
}

}  // namespace

bool AndroidSuperStore::detect(const ByteStorePtr& source) {
    if (!source) return false;
    std::uint8_t magic[4]{};
    const std::uint64_t offsets[] = {0, kGeometrySize, kGeometrySize * 2};
    for (std::uint64_t offset : offsets) {
        if (within(offset, sizeof(magic), static_cast<std::uint64_t>(source->capacity())) &&
            readExact(source, static_cast<std::int64_t>(offset), magic, sizeof(magic)) &&
            readLe32(magic) == kGeometryMagic)
            return true;
    }
    return false;
}

bool AndroidSuperStore::detect(const std::uint8_t* data, std::size_t size) {
    if (!data) return false;
    const std::uint64_t offsets[] = {0, kGeometrySize, kGeometrySize * 2};
    for (std::uint64_t offset : offsets) {
        if (within(offset, 4, size) && readLe32(data + offset) == kGeometryMagic)
            return true;
    }
    return false;
}

std::shared_ptr<AndroidSuperStore> AndroidSuperStore::open(
    const ByteStorePtr& source, std::string* error) {
    auto result = std::shared_ptr<AndroidSuperStore>(new AndroidSuperStore());
    result->source_ = source;
    if (!source || !result->parse(error)) return nullptr;
    return result;
}

bool AndroidSuperStore::parse(std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!source_ || source_->capacity() < static_cast<std::int64_t>(kGeometrySize + 128))
        return fail("Truncated Android super image");
    Geometry geometry;
    if (!readGeometry(source_, 0, &geometry) &&
        !readGeometry(source_, kGeometrySize, &geometry) &&
        !readGeometry(source_, kGeometrySize * 2, &geometry))
        return fail("Invalid Android super geometry or checksum");
    for (std::uint32_t slot = 0; slot < geometry.metadataSlots; ++slot) {
        if (parseMetadata(source_, geometry, slot, &partitions_))
            return true;
    }
    return fail("No valid Android super metadata copy was found");
}

}  // namespace fs
}  // namespace peare
