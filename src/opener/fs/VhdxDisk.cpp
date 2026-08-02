#include "VhdxDisk.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::int64_t kKiB = 1024;
const std::int64_t kMiB = 1024 * 1024;
const std::int64_t kFileHeaderSize = 64 * kKiB;
const std::int64_t kHeader1Offset = 64 * kKiB;
const std::int64_t kHeader2Offset = 128 * kKiB;
const std::int64_t kRegion1Offset = 192 * kKiB;
const std::int64_t kRegion2Offset = 256 * kKiB;
const std::uint64_t kFileSignature = 0x656c696678646876ULL;  // "vhdxfile"
const std::uint32_t kHeaderSignature = 0x64616568U;          // "head"
const std::uint32_t kRegionSignature = 0x69676572U;          // "regi"
const std::uint64_t kMetadataSignature = 0x617461646174656dULL;  // "metadata"
const std::uint64_t kStatusMask = 0x7ULL;
const std::uint64_t kPayloadFullyPresent = 6;

const std::uint8_t kBatGuid[16] =
    {0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42, 0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
const std::uint8_t kMetadataRegionGuid[16] =
    {0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b, 0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
const std::uint8_t kFileParametersGuid[16] =
    {0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d, 0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
const std::uint8_t kVirtualDiskSizeGuid[16] =
    {0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48, 0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
const std::uint8_t kLogicalSectorSizeGuid[16] =
    {0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47, 0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
const std::uint8_t kPhysicalSectorSizeGuid[16] =
    {0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44, 0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};

std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64u(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) |
           (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::int64_t le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(le64u(p));
}

bool sameGuid(const std::uint8_t* p, const std::uint8_t guid[16]) {
    return std::memcmp(p, guid, 16) == 0;
}

std::uint32_t crc32c(const std::uint8_t* data, std::size_t len) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0x82f63b78U ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xffffffffU;
    for (std::size_t i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xffU] ^ (c >> 8);
    return c ^ 0xffffffffU;
}

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
    return (a + b - 1) / b;
}

struct VhdxHeader {
    bool valid = false;
    std::uint64_t sequence = 0;
    bool hasLog = false;
};

VhdxHeader parseHeader(std::vector<std::uint8_t> h) {
    VhdxHeader out;
    if (h.size() < 4096 || le32(h.data()) != kHeaderSignature)
        return out;
    const std::uint32_t expected = le32(h.data() + 4);
    std::fill(h.begin() + 4, h.begin() + 8, std::uint8_t(0));
    if (crc32c(h.data(), 4096) != expected)
        return out;
    const std::uint16_t version = le16(h.data() + 66);
    out.sequence = le64u(h.data() + 8);
    out.hasLog = le64u(h.data() + 48) != 0 || le64u(h.data() + 56) != 0;
    out.valid = version == 1;
    return out;
}

struct Region {
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

struct VhdxLayout {
    Region bat;
    Region metadata;
    std::int64_t diskSize = 0;
    std::uint32_t blockSize = 0;
    std::uint32_t flags = 0;
    std::uint32_t logicalSectorSize = 512;
};

bool readRegionTableAt(const ByteStorePtr& file, std::int64_t offset, Region* bat,
                       Region* metadata, std::string* error) {
    std::vector<std::uint8_t> table = file->readRange(offset, kFileHeaderSize);
    if (table.size() < static_cast<std::size_t>(kFileHeaderSize) ||
        le32(table.data()) != kRegionSignature)
        return false;
    const std::uint32_t expected = le32(table.data() + 4);
    std::fill(table.begin() + 4, table.begin() + 8, std::uint8_t(0));
    if (crc32c(table.data(), table.size()) != expected)
        return false;
    const std::uint32_t entries = le32(table.data() + 8);
    if (entries > 2047) {
        if (error) *error = "Invalid VHDX region table";
        return false;
    }
    bool haveBat = false;
    bool haveMetadata = false;
    for (std::uint32_t i = 0; i < entries; ++i) {
        const std::size_t e = 16 + std::size_t(i) * 32;
        const std::int64_t fileOffset = le64(table.data() + e + 16);
        const std::int64_t length = le32(table.data() + e + 24);
        if (fileOffset < 0 || length <= 0 || fileOffset + length > file->capacity()) {
            if (error) *error = "Invalid VHDX region extent";
            return false;
        }
        if (sameGuid(table.data() + e, kBatGuid)) {
            bat->offset = fileOffset;
            bat->length = length;
            haveBat = true;
        } else if (sameGuid(table.data() + e, kMetadataRegionGuid)) {
            metadata->offset = fileOffset;
            metadata->length = length;
            haveMetadata = true;
        }
    }
    return haveBat && haveMetadata;
}

bool readMetadata(const ByteStorePtr& file, const Region& r, VhdxLayout* layout,
                  std::string* error) {
    std::vector<std::uint8_t> table = file->readRange(r.offset, 64 * kKiB);
    if (table.size() < 64 * kKiB || le64u(table.data()) != kMetadataSignature) {
        if (error) *error = "Missing VHDX metadata table";
        return false;
    }
    const std::uint16_t entries = le16(table.data() + 10);
    if (entries > 2047) {
        if (error) *error = "Invalid VHDX metadata entry count";
        return false;
    }
    bool haveFileParams = false;
    bool haveDiskSize = false;
    bool haveLogicalSector = false;
    for (std::uint16_t i = 0; i < entries; ++i) {
        const std::size_t e = 32 + std::size_t(i) * 32;
        if (e + 32 > table.size())
            break;
        const std::uint32_t itemOffset = le32(table.data() + e + 16);
        const std::uint32_t itemLength = le32(table.data() + e + 20);
        if (itemOffset + itemLength > r.length || r.offset + itemOffset + itemLength > file->capacity()) {
            if (error) *error = "Invalid VHDX metadata item extent";
            return false;
        }
        std::vector<std::uint8_t> item = file->readRange(r.offset + itemOffset, itemLength);
        if (item.size() < itemLength)
            return false;
        if (sameGuid(table.data() + e, kFileParametersGuid) && itemLength >= 8) {
            layout->blockSize = le32(item.data());
            layout->flags = le32(item.data() + 4);
            haveFileParams = true;
        } else if (sameGuid(table.data() + e, kVirtualDiskSizeGuid) && itemLength >= 8) {
            layout->diskSize = static_cast<std::int64_t>(le64u(item.data()));
            haveDiskSize = true;
        } else if (sameGuid(table.data() + e, kLogicalSectorSizeGuid) && itemLength >= 4) {
            layout->logicalSectorSize = le32(item.data());
            haveLogicalSector = true;
        }
    }
    if (!haveFileParams || !haveDiskSize || !haveLogicalSector) {
        if (error) *error = "Missing required VHDX metadata";
        return false;
    }
    if (layout->flags & 2U) {
        if (error) *error = "VHDX differencing images require a parent image";
        return false;
    }
    if (layout->diskSize <= 0 || layout->blockSize == 0 || layout->logicalSectorSize == 0) {
        if (error) *error = "Invalid VHDX geometry";
        return false;
    }
    return true;
}

bool readLayout(const ByteStorePtr& file, VhdxLayout* layout, std::string* error) {
    if (!file || file->capacity() < kRegion2Offset + kFileHeaderSize) {
        if (error) *error = "Truncated VHDX image";
        return false;
    }
    std::vector<std::uint8_t> head = file->readRange(0, 8);
    if (head.size() < 8 || le64u(head.data()) != kFileSignature) {
        if (error) *error = "Missing VHDX file signature";
        return false;
    }
    const VhdxHeader h1 = parseHeader(file->readRange(kHeader1Offset, 4096));
    const VhdxHeader h2 = parseHeader(file->readRange(kHeader2Offset, 4096));
    if (!h1.valid && !h2.valid) {
        if (error) *error = "No valid VHDX headers";
        return false;
    }
    const VhdxHeader active = h2.valid && (!h1.valid || h2.sequence > h1.sequence) ? h2 : h1;
    if (active.hasLog) {
        if (error) *error = "VHDX log replay is not supported";
        return false;
    }
    if (!readRegionTableAt(file, kRegion1Offset, &layout->bat, &layout->metadata, error) &&
        !readRegionTableAt(file, kRegion2Offset, &layout->bat, &layout->metadata, error)) {
        if (error && error->empty()) *error = "No valid VHDX region table";
        return false;
    }
    return readMetadata(file, layout->metadata, layout, error);
}

class VhdxStore final : public IByteStore {
public:
    VhdxStore(ByteStorePtr file, VhdxLayout layout, std::string* error)
        : file_(std::move(file)), layout_(layout) {
        valid_ = readBat(error);
    }

    bool valid() const { return valid_; }
    std::int64_t capacity() const override { return layout_.diskSize; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= layout_.diskSize) return 0;
        const int want = static_cast<int>(std::min<std::int64_t>(count, layout_.diskSize - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const std::int64_t block = p / layout_.blockSize;
            const std::int64_t offsetInBlock = p % layout_.blockSize;
            const int chunk = static_cast<int>(std::min<std::int64_t>(
                want - produced, static_cast<std::int64_t>(layout_.blockSize) - offsetInBlock));
            if (block < 0 || block >= static_cast<std::int64_t>(bat_.size())) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const std::uint64_t entry = bat_[static_cast<std::size_t>(block)];
                const std::uint64_t status = entry & kStatusMask;
                if (status == kPayloadFullyPresent) {
                    const std::int64_t src =
                        static_cast<std::int64_t>((entry >> 20) & 0xFFFFFFFFFFFULL) * kMiB +
                        offsetInBlock;
                    int got = file_->read(src, dst + produced, chunk);
                    if (got < chunk)
                        std::fill(dst + produced + got, dst + produced + chunk, std::uint8_t(0));
                } else {
                    std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                }
            }
            produced += chunk;
        }
        return produced;
    }

private:
    bool readBat(std::string* error) {
        const std::int64_t dataBlocks = ceilDiv(layout_.diskSize, layout_.blockSize);
        const std::int64_t chunkSize = (1LL << 23) * layout_.logicalSectorSize;
        const std::int64_t chunkRatio = chunkSize / layout_.blockSize;
        if (dataBlocks <= 0 || chunkRatio <= 0) {
            if (error) *error = "Invalid VHDX BAT geometry";
            return false;
        }
        const std::int64_t totalEntries = dataBlocks + (dataBlocks - 1) / chunkRatio;
        if (layout_.bat.length < totalEntries * 8) {
            if (error) *error = "Truncated VHDX BAT";
            return false;
        }
        std::vector<std::uint8_t> raw = file_->readRange(layout_.bat.offset, totalEntries * 8);
        if (raw.size() < static_cast<std::size_t>(totalEntries * 8)) {
            if (error) *error = "Truncated VHDX BAT";
            return false;
        }
        bat_.resize(static_cast<std::size_t>(dataBlocks));
        for (std::int64_t dataBlock = 0; dataBlock < dataBlocks; ++dataBlock) {
            const std::int64_t chunk = dataBlock / chunkRatio;
            const std::int64_t blockInChunk = dataBlock % chunkRatio;
            const std::int64_t batIndex = chunk * (chunkRatio + 1) + blockInChunk;
            bat_[static_cast<std::size_t>(dataBlock)] = le64u(raw.data() + batIndex * 8);
        }
        return true;
    }

    ByteStorePtr file_;
    VhdxLayout layout_;
    bool valid_ = false;
    std::vector<std::uint64_t> bat_;
};

}  // namespace

ByteStorePtr openVhdxDisk(const ByteStorePtr& file, std::string* error) {
    VhdxLayout layout;
    if (!readLayout(file, &layout, error))
        return nullptr;
    ByteStorePtr disk = std::make_shared<VhdxStore>(file, layout, error);
    if (!static_cast<VhdxStore*>(disk.get())->valid())
        return nullptr;
    return disk;
}

}  // namespace fs
}  // namespace peare
