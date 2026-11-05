#include "VmdkDisk.h"

#include "../modules/DeflateDecoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSector = 512;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64u(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) |
           (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}
std::int64_t le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(le64u(p));
}
std::int64_t alignSector(std::int64_t value) {
    return ((value + kSector - 1) / kSector) * kSector;
}

// A positioned byte store over a monolithicSparse VMDK. Reads resolve through the
// grain directory + grain tables; unallocated grains read as zero.
class VmdkSparseStore final : public IByteStore {
public:
    VmdkSparseStore(ByteStorePtr file, std::int64_t capacitySectors, std::int64_t grainSectors,
                    std::uint32_t gtesPerGt, bool zeroedGrainEntries,
                    std::vector<std::uint32_t> grainDir)
        : file_(std::move(file)),
          capacityBytes_(capacitySectors * kSector),
          grainSectors_(grainSectors),
          gtesPerGt_(gtesPerGt),
          zeroedGrainEntries_(zeroedGrainEntries),
          grainDir_(std::move(grainDir)) {
        gtCoverageSectors_ = grainSectors_ * static_cast<std::int64_t>(gtesPerGt_);
    }

    std::int64_t capacity() const override { return capacityBytes_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= capacityBytes_) return 0;
        int want = static_cast<int>(std::min<std::int64_t>(count, capacityBytes_ - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const std::int64_t sector = p / kSector;
            const std::int64_t grainBytes = grainSectors_ * kSector;
            const std::int64_t offsetInGrain = p % grainBytes;
            const int chunk = static_cast<int>(
                std::min<std::int64_t>(want - produced, grainBytes - offsetInGrain));

            const std::int64_t gtIndex = sector / gtCoverageSectors_;
            std::int64_t grainStartSector = 0;  // 0 == unallocated -> zeros
            if (gtIndex >= 0 && gtIndex < static_cast<std::int64_t>(grainDir_.size())) {
                const std::uint32_t gtSector = grainDir_[static_cast<std::size_t>(gtIndex)];
                if (gtSector != 0) {
                    const std::int64_t sectorInGt = sector - gtIndex * gtCoverageSectors_;
                    const std::int64_t grainInGt = sectorInGt / grainSectors_;
                    std::uint8_t gte[4];
                    const std::int64_t gtePos = gtSector * kSector + grainInGt * 4;
                    if (file_->read(gtePos, gte, 4) == 4) grainStartSector = le32(gte);
                }
            }

            if (grainStartSector == 0 || (zeroedGrainEntries_ && grainStartSector == 1)) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const std::int64_t src = grainStartSector * kSector + offsetInGrain;
                int got = 0;
                while (got < chunk) {
                    const int n = file_->read(src + got, dst + produced + got, chunk - got);
                    if (n <= 0) { std::fill(dst + produced + got, dst + produced + chunk, 0); break; }
                    got += n;
                }
            }
            produced += chunk;
        }
        return produced;
    }

private:
    ByteStorePtr file_;
    std::int64_t capacityBytes_;
    std::int64_t grainSectors_;
    std::uint32_t gtesPerGt_;
    bool zeroedGrainEntries_;
    std::vector<std::uint32_t> grainDir_;
    std::int64_t gtCoverageSectors_ = 0;
};

class VmdkStreamOptimizedStore final : public IByteStore {
public:
    VmdkStreamOptimizedStore(ByteStorePtr file, std::int64_t capacitySectors,
                             std::int64_t grainSectors, std::int64_t overheadSectors,
                             std::uint32_t gtesPerGt, std::int64_t rgdOffsetSectors,
                             std::int64_t gdOffsetSectors,
                             std::string* error)
        : file_(std::move(file)),
          capacityBytes_(capacitySectors * kSector),
          grainBytes_(grainSectors * kSector),
          capacitySectors_(capacitySectors),
          grainSectors_(grainSectors),
          gtesPerGt_(gtesPerGt),
          rgdOffsetSectors_(rgdOffsetSectors),
          gdOffsetSectors_(gdOffsetSectors) {
        if (!file_ || grainBytes_ <= 0 || capacityBytes_ <= 0) {
            if (error) *error = "Invalid streamOptimized VMDK geometry";
            return;
        }
        valid_ = buildIndex(overheadSectors * kSector, error);
    }

    bool valid() const { return valid_; }
    std::int64_t capacity() const override { return capacityBytes_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= capacityBytes_) return 0;
        const int want = static_cast<int>(
            std::min<std::int64_t>(count, capacityBytes_ - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const std::int64_t grainIndex = p / grainBytes_;
            const std::int64_t offsetInGrain = p % grainBytes_;
            const int chunk = static_cast<int>(
                std::min<std::int64_t>(want - produced, grainBytes_ - offsetInGrain));
            const std::vector<std::uint8_t>* grain = loadGrain(grainIndex);
            if (!grain || grain->empty()) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const std::size_t available =
                    offsetInGrain < static_cast<std::int64_t>(grain->size())
                        ? grain->size() - static_cast<std::size_t>(offsetInGrain)
                        : 0;
                const int n = static_cast<int>(
                    std::min<std::size_t>(static_cast<std::size_t>(chunk), available));
                if (n > 0)
                    std::copy(grain->begin() + static_cast<std::ptrdiff_t>(offsetInGrain),
                              grain->begin() + static_cast<std::ptrdiff_t>(offsetInGrain + n),
                              dst + produced);
                if (n < chunk)
                    std::fill(dst + produced + n, dst + produced + chunk, std::uint8_t(0));
            }
            produced += chunk;
        }
        return produced;
    }

private:
    struct GrainRef {
        GrainRef() : dataOffset(0), compressedSize(0) {}
        GrainRef(std::int64_t valueDataOffset, std::uint32_t valueCompressedSize)
            : dataOffset(valueDataOffset), compressedSize(valueCompressedSize) {}

        std::int64_t dataOffset;
        std::uint32_t compressedSize;
    };

    bool validateEmptyStreamMetadata() const {
        const std::int64_t max = std::numeric_limits<std::int64_t>::max();
        if (rgdOffsetSectors_ <= 0 || gdOffsetSectors_ <= 0 ||
            gtesPerGt_ == 0 || gtesPerGt_ > 512 ||
            grainSectors_ > max / static_cast<std::int64_t>(gtesPerGt_))
            return false;

        const std::int64_t sectorsPerTable =
            grainSectors_ * static_cast<std::int64_t>(gtesPerGt_);
        const std::int64_t tableCount =
            1 + (capacitySectors_ - 1) / sectorsPerTable;
        if (tableCount <= 0 || tableCount > 1000000)
            return false;

        const std::int64_t directoryBytes = tableCount * 4;
        const std::int64_t directorySectors =
            (directoryBytes + kSector - 1) / kSector;
        const std::int64_t tableSectors =
            (static_cast<std::int64_t>(gtesPerGt_) * 4 + kSector - 1) / kSector;
        if (tableCount > (max - directorySectors) / tableSectors)
            return false;
        const std::int64_t tablesSectors = tableCount * tableSectors;
        if (rgdOffsetSectors_ > max - directorySectors - tablesSectors ||
            rgdOffsetSectors_ + directorySectors + tablesSectors != gdOffsetSectors_ ||
            gdOffsetSectors_ > max - directorySectors - tablesSectors)
            return false;

        const std::int64_t dataOffset = file_->capacity();
        if (dataOffset % kSector != 0 ||
            gdOffsetSectors_ + directorySectors + tablesSectors > dataOffset / kSector)
            return false;
        const std::int64_t primaryDirectory = rgdOffsetSectors_ * kSector;
        const std::int64_t backupDirectory = gdOffsetSectors_ * kSector;
        const std::int64_t directoryReadBytes = tableCount * 4;
        const std::int64_t primaryTables =
            (rgdOffsetSectors_ + directorySectors) * kSector;
        const std::int64_t backupTables =
            (gdOffsetSectors_ + directorySectors) * kSector;
        const std::int64_t tableReadBytes = tablesSectors * kSector;
        if (primaryDirectory < 0 || backupDirectory < 0 ||
            primaryDirectory > dataOffset - directoryReadBytes ||
            backupDirectory > dataOffset - directoryReadBytes ||
            primaryTables > dataOffset - tableReadBytes ||
            backupTables > dataOffset - tableReadBytes)
            return false;

        const std::vector<std::uint8_t> primary =
            file_->readRange(primaryDirectory, directoryReadBytes);
        const std::vector<std::uint8_t> backup =
            file_->readRange(backupDirectory, directoryReadBytes);
        if (primary.size() != static_cast<std::size_t>(directoryReadBytes) ||
            backup.size() != static_cast<std::size_t>(directoryReadBytes))
            return false;

        for (std::int64_t i = 0; i < tableCount; ++i) {
            const std::uint32_t primarySector =
                le32(primary.data() + static_cast<std::size_t>(i * 4));
            const std::uint32_t backupSector =
                le32(backup.data() + static_cast<std::size_t>(i * 4));
            if (primarySector != rgdOffsetSectors_ + directorySectors + i * tableSectors ||
                backupSector != gdOffsetSectors_ + directorySectors + i * tableSectors)
                return false;
        }

        const std::int64_t scanChunk = 64 * 1024;
        for (const std::int64_t tableStart : {primaryTables, backupTables}) {
            std::int64_t done = 0;
            while (done < tableReadBytes) {
                const int count = static_cast<int>(
                    std::min(scanChunk, tableReadBytes - done));
                const std::vector<std::uint8_t> tables =
                    file_->readRange(tableStart + done, count);
                if (tables.size() != static_cast<std::size_t>(count) ||
                    std::any_of(tables.begin(), tables.end(),
                                [](std::uint8_t value) { return value != 0; }))
                    return false;
                done += count;
            }
        }
        return true;
    }

    bool buildIndex(std::int64_t pos, std::string* error) {
        // QEMU can omit EOS when an empty streamOptimized image ends at its data offset.
        if (pos == file_->capacity()) {
            if (validateEmptyStreamMetadata())
                return true;
            if (error) *error = "Invalid empty streamOptimized VMDK metadata";
            return false;
        }
        while (pos + 16 <= file_->capacity()) {
            std::uint8_t marker[16];
            file_->readExactly(pos, marker, 16);
            const std::uint64_t val = le64u(marker);
            const std::uint32_t size = le32(marker + 8);
            if (size > 0) {
                const std::int64_t grainIndex =
                    static_cast<std::int64_t>(val) / (grainBytes_ / kSector);
                if (grainIndex >= 0)
                    grains_[grainIndex] = GrainRef{pos + 12, size};
                pos = alignSector(pos + 12 + size);
                continue;
            }
            const std::uint32_t type = le32(marker + 12);
            if (type == 0) return true;  // end-of-stream marker
            if (type == 1 || type == 2 || type == 3) {
                // Metadata markers are sector-aligned and followed by val sectors
                // of grain table, grain directory, or footer data. The stream
                // index already has the compressed grain locations, so skip them.
                pos += kSector + static_cast<std::int64_t>(val) * kSector;
                continue;
            }
            if (error) *error = "Invalid streamOptimized VMDK marker";
            return false;
        }
        if (error) *error = "Truncated streamOptimized VMDK marker stream";
        return false;
    }

    const std::vector<std::uint8_t>* loadGrain(std::int64_t grainIndex) const {
        if (cacheIndex_ == grainIndex) return &cache_;
        cacheIndex_ = grainIndex;
        cache_.clear();
        const auto found = grains_.find(grainIndex);
        if (found == grains_.end()) return &cache_;
        const GrainRef& ref = found->second;
        std::vector<std::uint8_t> compressed =
            file_->readRange(ref.dataOffset, ref.compressedSize);
        if (compressed.size() != ref.compressedSize) return &cache_;
        if (!compression::inflateZlib(
                compressed, static_cast<std::size_t>(grainBytes_), &cache_))
            cache_.clear();
        return &cache_;
    }

    ByteStorePtr file_;
    std::int64_t capacityBytes_;
    std::int64_t grainBytes_;
    std::int64_t capacitySectors_;
    std::int64_t grainSectors_;
    std::uint32_t gtesPerGt_;
    std::int64_t rgdOffsetSectors_;
    std::int64_t gdOffsetSectors_;
    bool valid_ = false;
    std::map<std::int64_t, GrainRef> grains_;
    mutable std::int64_t cacheIndex_ = -1;
    mutable std::vector<std::uint8_t> cache_;
};

// Extracts the quoted file name from an extent descriptor line, or "" if absent.
std::string quotedName(const std::string& line) {
    const std::size_t a = line.find('"');
    if (a == std::string::npos) return std::string();
    const std::size_t b = line.find('"', a + 1);
    if (b == std::string::npos) return std::string();
    return line.substr(a + 1, b - a - 1);
}

}  // namespace

ByteStorePtr openVmdkExtent(const ByteStorePtr& file, std::string* error) {
    auto fail = [&](const std::string& m) -> ByteStorePtr {
        if (error) *error = m;
        return nullptr;
    };
    if (!file) return fail("Null VMDK source");

    std::vector<std::uint8_t> h = file->readRange(0, 512);
    if (h.size() < 512) return fail("Truncated VMDK header");
    if (le32(h.data()) != 0x564D444Bu)  // "KDMV"
        return fail("Not a KDMV sparse extent");

    const std::uint32_t flags = le32(h.data() + 8);
    const std::uint32_t version = le32(h.data() + 4);
    const std::int64_t capacity = le64(h.data() + 0x0C);
    const std::int64_t grainSize = le64(h.data() + 0x14);
    const std::uint32_t gtesPerGt = le32(h.data() + 0x2C);
    const std::int64_t rgdOffset = le64(h.data() + 0x30);
    const std::int64_t gdOffset = le64(h.data() + 0x38);
    const std::int64_t overHead = le64(h.data() + 0x40);
    const std::uint16_t compress =
        static_cast<std::uint16_t>(h[0x4D] | (h[0x4E] << 8));

    if (compress == 1) {
        if (grainSize <= 0 || capacity <= 0 || overHead <= 0)
            return fail("Invalid streamOptimized VMDK geometry");
        ByteStorePtr stream =
            std::make_shared<VmdkStreamOptimizedStore>(
                file, capacity, grainSize, overHead, gtesPerGt, rgdOffset, gdOffset, error);
        if (!static_cast<VmdkStreamOptimizedStore*>(stream.get())->valid())
            return nullptr;
        return stream;
    }
    if (compress != 0)
        return fail("Unsupported VMDK compression algorithm");
    if (grainSize <= 0 || gtesPerGt == 0 || capacity <= 0 || gdOffset <= 0)
        return fail("Invalid VMDK sparse geometry");

    const std::int64_t gtCoverageSectors = grainSize * static_cast<std::int64_t>(gtesPerGt);
    const std::int64_t numGrainTables = (capacity + gtCoverageSectors - 1) / gtCoverageSectors;
    if (numGrainTables <= 0 || numGrainTables > 1000000)
        return fail("Implausible VMDK grain directory size");

    std::vector<std::uint8_t> gd =
        file->readRange(gdOffset * kSector, numGrainTables * 4);
    if (static_cast<std::int64_t>(gd.size()) < numGrainTables * 4)
        return fail("Truncated VMDK grain directory");

    std::vector<std::uint32_t> grainDir(static_cast<std::size_t>(numGrainTables));
    for (std::size_t i = 0; i < grainDir.size(); ++i) grainDir[i] = le32(gd.data() + i * 4);

    const bool zeroedGrainEntries = version >= 2 && bool(flags & 4U);
    return std::make_shared<VmdkSparseStore>(file, capacity, grainSize, gtesPerGt,
                                             zeroedGrainEntries,
                                             std::move(grainDir));
}

ByteStorePtr openVmdkDisk(const ByteStorePtr& file,
                          const std::function<ByteStorePtr(const std::string&)>& resolveSibling,
                          std::string* error) {
    auto fail = [&](const std::string& m) -> ByteStorePtr {
        if (error) *error = m;
        return nullptr;
    };
    if (!file) return fail("Null VMDK source");

    std::vector<std::uint8_t> head = file->readRange(0, 64);
    if (head.size() >= 4 && le32(head.data()) == 0x564D444Bu)
        return openVmdkExtent(file, error);  // single KDMV extent

    // Otherwise expect a text disk descriptor referencing sibling extents.
    std::vector<std::uint8_t> raw = file->readRange(0, 1 << 20);
    std::string text(raw.begin(), raw.end());
    if (text.find("# Disk DescriptorFile") == std::string::npos &&
        text.find("createType") == std::string::npos)
        return fail("Unrecognized VMDK (neither KDMV extent nor disk descriptor)");

    std::vector<ByteStorePtr> parts;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        // Extent lines: <ACCESS> <sizeSectors> <TYPE> "file" [offset]
        if (line.compare(0, 3, "RW ") != 0 && line.compare(0, 7, "RDONLY ") != 0 &&
            line.compare(0, 9, "NOACCESS ") != 0)
            continue;
        std::istringstream ss(line);
        std::string access, type;
        long long sectors = 0;
        ss >> access >> sectors >> type;
        if (sectors <= 0) continue;
        const std::int64_t bytes = sectors * kSector;

        if (type == "ZERO") {
            parts.push_back(std::make_shared<ZeroStore>(bytes));
            continue;
        }
        const std::string name = quotedName(line);
        if (name.empty()) return fail("VMDK descriptor extent without a file name");
        ByteStorePtr sib = resolveSibling ? resolveSibling(name) : nullptr;
        if (!sib) return fail("VMDK extent file not found: " + name);

        if (type == "SPARSE") {
            ByteStorePtr ext = openVmdkExtent(sib, error);
            if (!ext) return nullptr;
            parts.push_back(ext);
        } else if (type == "FLAT" || type == "VMFS") {
            long long offsetSectors = 0;
            const std::size_t firstQuote = line.find('"');
            const std::size_t secondQuote = firstQuote == std::string::npos
                ? std::string::npos : line.find('"', firstQuote + 1);
            if (secondQuote != std::string::npos) {
                std::istringstream tail(line.substr(secondQuote + 1));
                tail >> offsetSectors;
                if (!tail && !line.substr(secondQuote + 1).empty())
                    return fail("Invalid VMDK FLAT extent offset");
            }
            if (offsetSectors < 0 || offsetSectors > sib->capacity() / kSector ||
                bytes > sib->capacity() - offsetSectors * kSector)
                return fail("VMDK FLAT extent lies outside its backing file");
            parts.push_back(std::make_shared<SubStore>(sib, offsetSectors * kSector, bytes));
        } else {
            return fail("Unsupported VMDK extent type: " + type);
        }
    }
    if (parts.empty()) return fail("VMDK descriptor lists no extents");
    return std::make_shared<ConcatStore>(std::move(parts));
}

ByteStorePtr openVmdkDisk(const ByteStorePtr& file, std::string* error) {
    return openVmdkDisk(file, std::function<ByteStorePtr(const std::string&)>(), error);
}

}  // namespace fs
}  // namespace peare
