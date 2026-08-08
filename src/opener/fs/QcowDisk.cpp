#include "QcowDisk.h"

#include "../modules/DeflateDecoder.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kQcowMagic = 0x514649fbU;
const std::uint64_t kCopiedFlag = std::uint64_t(1) << 63;
const std::uint64_t kCompressedFlagV1 = std::uint64_t(1) << 63;
const std::uint64_t kCompressedFlagV2 = std::uint64_t(1) << 62;
const std::uint64_t kQcow2OffsetMask = 0x00fffffffffffe00ULL;
const std::uint64_t kQcow2IncompatDirty = std::uint64_t(1) << 0;
const std::uint64_t kQcow2IncompatCorrupt = std::uint64_t(1) << 1;
const std::uint64_t kQcow2IncompatExternalData = std::uint64_t(1) << 2;
const std::uint64_t kQcow2IncompatCompression = std::uint64_t(1) << 3;
const std::uint64_t kQcow2IncompatExtendedL2 = std::uint64_t(1) << 4;
const std::uint64_t kSupportedIncompat =
    kQcow2IncompatDirty | kQcow2IncompatExtendedL2;

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t be64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(be32(p)) << 32) |
           static_cast<std::uint64_t>(be32(p + 4));
}

bool rangeInside(std::uint64_t offset, std::uint64_t length, std::int64_t total) {
    if (total < 0 || offset > static_cast<std::uint64_t>(total)) return false;
    return length <= static_cast<std::uint64_t>(total) - offset;
}

bool aligned(std::uint64_t value, std::uint64_t alignment) {
    return alignment != 0 && (value & (alignment - 1)) == 0;
}

std::uint64_t lowMask(unsigned bits) {
    if (bits >= 64) return ~std::uint64_t(0);
    return bits == 0 ? 0 : (std::uint64_t(1) << bits) - 1;
}

struct QcowHeader {
    std::uint32_t version = 0;
    std::uint64_t backingFileOffset = 0;
    std::uint32_t backingFileSize = 0;
    std::uint64_t diskSize = 0;
    unsigned clusterBits = 0;
    unsigned l2Bits = 0;
    std::uint32_t cryptMethod = 0;
    std::uint32_t l1Size = 0;
    std::uint64_t l1TableOffset = 0;
    std::uint64_t incompatibleFeatures = 0;
    bool extendedL2 = false;

    std::uint64_t clusterSize() const { return std::uint64_t(1) << clusterBits; }
    std::uint64_t l2EntrySize() const { return extendedL2 ? 16 : 8; }
    std::uint64_t l2Entries() const {
        return version == 1 ? (std::uint64_t(1) << l2Bits)
                            : clusterSize() / l2EntrySize();
    }
    std::uint64_t l2TableBytes() const {
        return version == 1 ? l2Entries() * 8 : clusterSize();
    }
};

bool validateCommonHeader(const ByteStorePtr& file, QcowHeader* h,
                          std::string* error) {
    if (h->diskSize == 0 || h->diskSize >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        if (error) *error = "Invalid QCOW virtual disk size";
        return false;
    }
    if (h->clusterBits < 9 || h->clusterBits > 21) {
        if (error) *error = "Unsupported QCOW cluster size";
        return false;
    }
    if (h->cryptMethod != 0) {
        if (error) *error = "Encrypted QCOW images are unsupported";
        return false;
    }
    if (h->backingFileOffset != 0 || h->backingFileSize != 0) {
        if (error) *error = "QCOW backing images are not resolved";
        return false;
    }

    const std::uint64_t clusterSize = h->clusterSize();
    if (h->version == 1) {
        if (h->l2Bits == 0 || h->l2Bits > 21 ||
            h->clusterBits + h->l2Bits >= 63) {
            if (error) *error = "Invalid QCOW v1 L2 geometry";
            return false;
        }
        const std::uint64_t coverage = std::uint64_t(1) <<
            (h->clusterBits + h->l2Bits);
        const std::uint64_t l1Size = h->diskSize / coverage +
            (h->diskSize % coverage != 0 ? 1 : 0);
        if (l1Size > std::numeric_limits<std::uint32_t>::max()) {
            if (error) *error = "QCOW v1 L1 table is too large";
            return false;
        }
        h->l1Size = static_cast<std::uint32_t>(l1Size);
    } else {
        if (h->extendedL2 && h->clusterBits < 14) {
            if (error) *error = "QCOW2 extended L2 requires 16 KiB clusters";
            return false;
        }
        if (h->l1Size == 0) {
            if (error) *error = "Invalid QCOW2 L1 table size";
            return false;
        }
    }

    if (h->l1Size == 0 || h->l1Size > 4U * 1024U * 1024U) {
        if (error) *error = "QCOW L1 table is too large";
        return false;
    }
    const std::uint64_t l1Bytes = static_cast<std::uint64_t>(h->l1Size) * 8;
    if (h->l1TableOffset == 0 ||
        (h->version != 1 && !aligned(h->l1TableOffset, clusterSize)) ||
        !rangeInside(h->l1TableOffset, l1Bytes, file->capacity())) {
        if (error) *error = "Invalid QCOW L1 table location";
        return false;
    }

    const std::uint64_t entries = h->l2Entries();
    if (entries == 0 || h->l2TableBytes() == 0 ||
        h->l2TableBytes() > 32ULL * 1024ULL * 1024ULL) {
        if (error) *error = "Invalid QCOW L2 table geometry";
        return false;
    }
    const std::uint64_t guestClusters = h->diskSize / clusterSize +
        (h->diskSize % clusterSize != 0 ? 1 : 0);
    const std::uint64_t neededL1 = guestClusters / entries +
        (guestClusters % entries != 0 ? 1 : 0);
    if (neededL1 > h->l1Size) {
        if (error) *error = "QCOW L1 table does not cover the virtual disk";
        return false;
    }
    return true;
}

bool readHeader(const ByteStorePtr& file, QcowHeader* h, std::string* error) {
    if (!file || file->capacity() < 8) {
        if (error) *error = "Truncated QCOW image";
        return false;
    }
    const std::vector<std::uint8_t> first = file->readRange(0, 104);
    if (first.size() < 8 || be32(first.data()) != kQcowMagic) {
        if (error) *error = "Missing QCOW signature";
        return false;
    }

    h->version = be32(first.data() + 4);
    if (h->version == 1) {
        if (first.size() < 48) {
            if (error) *error = "Truncated QCOW v1 header";
            return false;
        }
        h->backingFileOffset = be64(first.data() + 8);
        h->backingFileSize = be32(first.data() + 16);
        h->diskSize = be64(first.data() + 24);
        h->clusterBits = first[32];
        h->l2Bits = first[33];
        h->cryptMethod = be32(first.data() + 36);
        h->l1TableOffset = be64(first.data() + 40);
        return validateCommonHeader(file, h, error);
    }

    if (h->version != 2 && h->version != 3) {
        if (error) *error = "Unsupported QCOW version";
        return false;
    }
    if (first.size() < 72) {
        if (error) *error = "Truncated QCOW2 header";
        return false;
    }
    h->backingFileOffset = be64(first.data() + 8);
    h->backingFileSize = be32(first.data() + 16);
    h->clusterBits = be32(first.data() + 20);
    h->diskSize = be64(first.data() + 24);
    h->cryptMethod = be32(first.data() + 32);
    h->l1Size = be32(first.data() + 36);
    h->l1TableOffset = be64(first.data() + 40);

    if (h->version == 3) {
        if (first.size() < 104) {
            if (error) *error = "Truncated QCOW2 v3 header";
            return false;
        }
        h->incompatibleFeatures = be64(first.data() + 72);
        const std::uint32_t headerLength = be32(first.data() + 100);
        if (h->clusterBits < 9 || h->clusterBits > 21 ||
            headerLength < 104 || (headerLength & 7U) != 0 ||
            headerLength > (std::uint64_t(1) << h->clusterBits)) {
            if (error) *error = "Invalid QCOW2 header length";
            return false;
        }
        if ((h->incompatibleFeatures & kQcow2IncompatCorrupt) != 0) {
            if (error) *error = "QCOW2 image is marked corrupt";
            return false;
        }
        if ((h->incompatibleFeatures & kQcow2IncompatExternalData) != 0) {
            if (error) *error = "QCOW2 external data files are unsupported";
            return false;
        }
        if ((h->incompatibleFeatures & kQcow2IncompatCompression) != 0) {
            if (error) *error = "Non-zlib QCOW2 compression is unsupported";
            return false;
        }
        const std::uint64_t unknown = h->incompatibleFeatures & ~kSupportedIncompat;
        if (unknown != 0) {
            if (error) *error = "Unsupported QCOW2 incompatible feature";
            return false;
        }
        h->extendedL2 =
            (h->incompatibleFeatures & kQcow2IncompatExtendedL2) != 0;
    }
    return validateCommonHeader(file, h, error);
}

class QcowStore final : public IByteStore {
public:
    QcowStore(ByteStorePtr file, QcowHeader header, std::string* error)
        : file_(std::move(file)), header_(header) {
        valid_ = loadL1(error);
    }

    bool valid() const { return valid_; }
    std::int64_t capacity() const override {
        return static_cast<std::int64_t>(header_.diskSize);
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || !dst || count <= 0 ||
            static_cast<std::uint64_t>(pos) >= header_.diskSize)
            return 0;
        const int want = static_cast<int>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(count), header_.diskSize - pos));
        int produced = 0;
        const std::uint64_t clusterSize = header_.clusterSize();

        while (produced < want) {
            const std::uint64_t guest = static_cast<std::uint64_t>(pos + produced);
            const std::uint64_t offsetInCluster = guest & (clusterSize - 1);
            int chunk = static_cast<int>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(want - produced),
                clusterSize - offsetInCluster));

            std::uint64_t descriptor = 0;
            std::uint64_t allocationBitmap = 0;
            if (!lookup(guest, &descriptor, &allocationBitmap) || descriptor == 0) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                produced += chunk;
                continue;
            }

            if (isCompressed(descriptor)) {
                if (!readCompressed(descriptor, offsetInCluster,
                                    dst + produced, chunk))
                    std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                produced += chunk;
                continue;
            }

            if (header_.extendedL2) {
                const std::uint64_t subclusterSize = clusterSize / 32;
                const unsigned subcluster = static_cast<unsigned>(
                    offsetInCluster / subclusterSize);
                const std::uint64_t subOffset = offsetInCluster % subclusterSize;
                chunk = static_cast<int>(std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(chunk), subclusterSize - subOffset));
                const bool allocated =
                    (allocationBitmap & (std::uint64_t(1) << subcluster)) != 0;
                const bool zero =
                    (allocationBitmap & (std::uint64_t(1) << (32 + subcluster))) != 0;
                if (zero || !allocated) {
                    std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                    produced += chunk;
                    continue;
                }
            } else if (header_.version >= 3 && (descriptor & 1U) != 0) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                produced += chunk;
                continue;
            }

            const std::uint64_t host = normalHostOffset(descriptor);
            if (host == 0 || !rangeInside(host + offsetInCluster,
                                          static_cast<std::uint64_t>(chunk),
                                          file_->capacity())) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const int got = file_->read(
                    static_cast<std::int64_t>(host + offsetInCluster),
                    dst + produced, chunk);
                if (got < chunk)
                    std::fill(dst + produced + std::max(got, 0),
                              dst + produced + chunk, std::uint8_t(0));
            }
            produced += chunk;
        }
        return produced;
    }

private:
    struct L2CacheEntry {
        std::uint64_t offset = 0;
        std::uint64_t age = 0;
        std::vector<std::uint8_t> bytes;
    };

    bool loadL1(std::string* error) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(header_.l1Size) * 8;
        const std::vector<std::uint8_t> raw =
            file_->readRange(static_cast<std::int64_t>(header_.l1TableOffset),
                             static_cast<std::int64_t>(bytes));
        if (raw.size() != bytes) {
            if (error) *error = "Truncated QCOW L1 table";
            return false;
        }
        l1_.resize(header_.l1Size);
        for (std::size_t i = 0; i < l1_.size(); ++i) {
            const std::uint64_t entry = be64(raw.data() + i * 8);
            const std::uint64_t offset = header_.version == 1
                ? entry : (entry & kQcow2OffsetMask);
            if (offset != 0 &&
                (!aligned(offset, header_.clusterSize()) ||
                 !rangeInside(offset, header_.l2TableBytes(), file_->capacity()))) {
                if (error) *error = "Invalid QCOW L2 table pointer";
                return false;
            }
            l1_[i] = offset;
        }
        l2Cache_.resize(4);
        return true;
    }

    bool lookup(std::uint64_t guest, std::uint64_t* descriptor,
                std::uint64_t* allocationBitmap) const {
        const std::uint64_t cluster = guest >> header_.clusterBits;
        const std::uint64_t l2Entries = header_.l2Entries();
        const std::uint64_t l1Index = cluster / l2Entries;
        const std::uint64_t l2Index = cluster % l2Entries;
        if (l1Index >= l1_.size() || l1_[static_cast<std::size_t>(l1Index)] == 0)
            return false;

        const std::uint64_t tableOffset = l1_[static_cast<std::size_t>(l1Index)];
        std::lock_guard<std::mutex> lock(l2Mutex_);
        const std::vector<std::uint8_t>* table = nullptr;
        for (std::size_t i = 0; i < l2Cache_.size(); ++i) {
            if (l2Cache_[i].offset == tableOffset && !l2Cache_[i].bytes.empty()) {
                l2Cache_[i].age = ++l2Age_;
                table = &l2Cache_[i].bytes;
                break;
            }
        }
        if (!table) {
            std::size_t replace = 0;
            for (std::size_t i = 1; i < l2Cache_.size(); ++i)
                if (l2Cache_[i].age < l2Cache_[replace].age) replace = i;
            L2CacheEntry& slot = l2Cache_[replace];
            slot.bytes = file_->readRange(
                static_cast<std::int64_t>(tableOffset),
                static_cast<std::int64_t>(header_.l2TableBytes()));
            if (slot.bytes.size() != header_.l2TableBytes()) {
                slot.bytes.clear();
                slot.offset = 0;
                return false;
            }
            slot.offset = tableOffset;
            slot.age = ++l2Age_;
            table = &slot.bytes;
        }

        const std::uint64_t entryOffset = l2Index * header_.l2EntrySize();
        if (entryOffset + header_.l2EntrySize() > table->size()) return false;
        *descriptor = be64(table->data() + entryOffset);
        *allocationBitmap = header_.extendedL2
            ? be64(table->data() + entryOffset + 8) : 0;
        return true;
    }

    bool isCompressed(std::uint64_t descriptor) const {
        return (descriptor & (header_.version == 1
            ? kCompressedFlagV1 : kCompressedFlagV2)) != 0;
    }

    std::uint64_t normalHostOffset(std::uint64_t descriptor) const {
        if (header_.version == 1) return descriptor;
        return descriptor & kQcow2OffsetMask;
    }

    bool readCompressed(std::uint64_t descriptor, std::uint64_t clusterOffset,
                        std::uint8_t* dst, int count) const {
        std::lock_guard<std::mutex> lock(compressedMutex_);
        if (compressedDescriptor_ != descriptor ||
            compressedCluster_.size() != header_.clusterSize()) {
            std::uint64_t hostOffset = 0;
            std::uint64_t compressedSize = 0;
            if (header_.version == 1) {
                const unsigned sizeShift = 63 - header_.clusterBits;
                hostOffset = descriptor & lowMask(sizeShift);
                compressedSize = (descriptor >> sizeShift) &
                    (header_.clusterSize() - 1);
            } else {
                const unsigned sizeShift = 62 - (header_.clusterBits - 8);
                const std::uint64_t sectorsMask =
                    lowMask(header_.clusterBits - 8);
                hostOffset = descriptor & lowMask(sizeShift);
                const std::uint64_t sectors =
                    ((descriptor >> sizeShift) & sectorsMask) + 1;
                compressedSize = sectors * 512 - (hostOffset & 511U);
            }
            if (compressedSize == 0 ||
                compressedSize > header_.clusterSize() + 512 ||
                !rangeInside(hostOffset, compressedSize, file_->capacity())) {
                compressedDescriptor_ = 0;
                compressedCluster_.clear();
                return false;
            }

            const std::vector<std::uint8_t> compressed = file_->readRange(
                static_cast<std::int64_t>(hostOffset),
                static_cast<std::int64_t>(compressedSize));
            std::vector<std::uint8_t> decoded;
            std::string ignored;
            if (compressed.size() != compressedSize ||
                !compression::inflateRawExact(
                    compressed.data(), compressed.size(),
                    static_cast<std::size_t>(header_.clusterSize()),
                    nullptr, &decoded, &ignored)) {
                compressedDescriptor_ = 0;
                compressedCluster_.clear();
                return false;
            }
            compressedDescriptor_ = descriptor;
            compressedCluster_.swap(decoded);
        }
        if (clusterOffset > compressedCluster_.size() ||
            static_cast<std::size_t>(count) > compressedCluster_.size() - clusterOffset)
            return false;
        std::copy(compressedCluster_.begin() + static_cast<std::ptrdiff_t>(clusterOffset),
                  compressedCluster_.begin() + static_cast<std::ptrdiff_t>(clusterOffset + count),
                  dst);
        return true;
    }

    ByteStorePtr file_;
    QcowHeader header_;
    bool valid_ = false;
    std::vector<std::uint64_t> l1_;

    mutable std::mutex l2Mutex_;
    mutable std::vector<L2CacheEntry> l2Cache_;
    mutable std::uint64_t l2Age_ = 0;

    mutable std::mutex compressedMutex_;
    mutable std::uint64_t compressedDescriptor_ = 0;
    mutable std::vector<std::uint8_t> compressedCluster_;
};

}  // namespace

ByteStorePtr openQcowDisk(const ByteStorePtr& file, std::string* error) {
    if (error) error->clear();
    QcowHeader header;
    if (!readHeader(file, &header, error)) return nullptr;
    std::shared_ptr<QcowStore> disk =
        std::make_shared<QcowStore>(file, header, error);
    if (!disk->valid()) return nullptr;
    return disk;
}

}  // namespace fs
}  // namespace peare
