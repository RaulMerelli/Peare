#include "AcronisFatRestore.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>

namespace peare {
namespace fs {
namespace {

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(u16(p)) | (static_cast<std::uint32_t>(u16(p + 2)) << 16);
}

bool readAll(const ByteStorePtr& store, std::uint64_t offset, std::uint8_t* data, std::size_t length) {
    if (!store || offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - offset)
        return false;
    while (length) {
        int chunk = static_cast<int>(std::min<std::size_t>(length, 1024 * 1024));
        if (!store->readExactlyChecked(static_cast<std::int64_t>(offset), data, chunk)) return false;
        offset += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

class Overlay final : public IByteStore {
public:
    Overlay(ByteStorePtr source, std::uint64_t fatStart, std::size_t fatSize,
            unsigned copies, std::vector<std::uint8_t> fat,
            std::map<std::uint64_t, std::uint8_t> changes)
        : source_(source), fatStart_(fatStart), fatSize_(fatSize), copies_(copies),
          fat_(std::move(fat)), changes_(std::move(changes)) {}

    std::int64_t capacity() const override { return source_->capacity(); }
    bool validate() const override { return source_->validate(); }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || !dst || pos >= capacity()) return 0;
        const std::int64_t available = capacity() - pos;
        const int n = static_cast<int>(std::min<std::int64_t>(count, available));
        if (!source_->readExactlyChecked(pos, dst, n)) return 0;
        const std::uint64_t begin = static_cast<std::uint64_t>(pos);
        const std::uint64_t end = begin + static_cast<unsigned>(n);
        for (unsigned i = 0; i < copies_; ++i) {
            const std::uint64_t start = fatStart_ + static_cast<std::uint64_t>(i) * fatSize_;
            const std::uint64_t a = std::max(begin, start);
            const std::uint64_t b = std::min(end, start + fatSize_);
            if (a < b) std::memcpy(dst + static_cast<std::size_t>(a - begin),
                                   fat_.data() + static_cast<std::size_t>(a - start),
                                   static_cast<std::size_t>(b - a));
        }
        for (std::map<std::uint64_t, std::uint8_t>::const_iterator it = changes_.lower_bound(begin);
             it != changes_.end() && it->first < end; ++it)
            dst[static_cast<std::size_t>(it->first - begin)] = it->second;
        return n;
    }

private:
    ByteStorePtr source_;
    std::uint64_t fatStart_;
    std::size_t fatSize_;
    unsigned copies_;
    std::vector<std::uint8_t> fat_;
    std::map<std::uint64_t, std::uint8_t> changes_;
};

class Builder {
public:
    Builder(ByteStorePtr source, ByteStorePtr bitmap)
        : source_(source), bitmap_(bitmap), bytesPerSector_(0), sectorsPerCluster_(0),
          clusterSize_(0), clusters_(0), type_(0), fatStart_(0), fatSize_(0),
          dataStart_(0), rootStart_(0), rootBytes_(0), rootCluster_(0), copies_(0) {}

    ByteStorePtr build() {
        geometry();
        if (bitmap_ && bitmap_->capacity() != 0) {
            if (bitmap_->capacity() < 0 || !bitmap_->validate()) fail("Invalid bitmap");
            const std::uint64_t required = (static_cast<std::uint64_t>(clusters_) + 7) / 8;
            const std::uint64_t supplied = static_cast<std::uint64_t>(bitmap_->capacity());
            bitmapBytes_.resize(static_cast<std::size_t>(std::min(required, supplied)));
            if (!readAll(bitmap_, 0, bitmapBytes_.data(), bitmapBytes_.size())) fail("Cannot read bitmap");
            std::uint8_t tail[4096];
            std::uint64_t offset = bitmapBytes_.size();
            while (offset < supplied) {
                const std::size_t count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(sizeof(tail), supplied - offset));
                if (!readAll(bitmap_, offset, tail, count)) fail("Cannot read bitmap");
                offset += count;
            }
            if (!bitmap_->validate()) fail("Invalid bitmap");
            sourceFat_ = fat_;
            std::fill(fat_.begin(), fat_.end(), 0);
            const unsigned reservedBytes = type_ == 12 ? 3 : type_ == 16 ? 4 : 8;
            std::fill(fat_.begin(), fat_.begin() + reservedBytes, 0xff);
            fat_[0] = 0xf8;
            visited_.resize((static_cast<std::size_t>(clusters_) + 9) / 8, 0);
            scan();
            for (std::uint32_t c = 2; c < clusters_ + 2; ++c) {
                if (!visited(c) && get(c) != 0) set(c, bad());
            }
        }
        return ByteStorePtr(new Overlay(source_, fatStart_, fatSize_, copies_,
                                        std::move(fat_), std::move(changes_)));
    }

private:
    struct Directory {
        std::uint32_t cluster;
        std::uint32_t parent;
        bool root;
    };

    struct LongEntry {
        std::uint64_t offset;
        std::uint8_t ordinal;
        std::uint8_t checksum;
        bool valid;
    };

    static void fail(const char* message) { throw std::runtime_error(message); }

    void geometry() {
        if (!source_ || source_->capacity() < 512 || !source_->validate()) fail("Invalid source");
        std::uint8_t boot[512];
        if (!readAll(source_, 0, boot, sizeof(boot))) fail("Cannot read boot sector");
        bytesPerSector_ = u16(boot + 11);
        sectorsPerCluster_ = boot[13];
        const std::uint32_t reserved = u16(boot + 14);
        copies_ = boot[16];
        const std::uint32_t rootEntries = u16(boot + 17);
        const std::uint32_t total = u16(boot + 19) ? u16(boot + 19) : u32(boot + 32);
        const std::uint32_t fatSectors = u16(boot + 22) ? u16(boot + 22) : u32(boot + 36);
        if (boot[510] != 0x55 || boot[511] != 0xaa || bytesPerSector_ < 512 ||
            bytesPerSector_ > 4096 || (bytesPerSector_ & (bytesPerSector_ - 1)) ||
            !sectorsPerCluster_ || (sectorsPerCluster_ & (sectorsPerCluster_ - 1)) ||
            !reserved || !copies_ || !fatSectors || !total) fail("Invalid FAT geometry");
        clusterSize_ = bytesPerSector_ * sectorsPerCluster_;
        const std::uint64_t rootSectors = (static_cast<std::uint64_t>(rootEntries) * 32 + bytesPerSector_ - 1) / bytesPerSector_;
        const std::uint64_t firstData = static_cast<std::uint64_t>(reserved) +
            static_cast<std::uint64_t>(copies_) * fatSectors + rootSectors;
        if (firstData >= total || static_cast<std::uint64_t>(total) * bytesPerSector_ >
            static_cast<std::uint64_t>(source_->capacity())) fail("FAT geometry exceeds source");
        clusters_ = static_cast<std::uint32_t>((total - firstData) / sectorsPerCluster_);
        if (!clusters_) fail("Empty FAT data region");
        type_ = clusters_ < 4085 ? 12 : clusters_ < 65525 ? 16 : 32;
        if (clusters_ > 0x0fffffedU) fail("Too many FAT clusters");
        if ((type_ == 32 && (rootEntries || u16(boot + 22))) ||
            (type_ != 32 && (!rootEntries || !u16(boot + 22)))) fail("Inconsistent FAT type");
        const std::uint64_t fatBytes = static_cast<std::uint64_t>(fatSectors) * bytesPerSector_;
        const std::uint64_t needed = type_ == 12 ? (static_cast<std::uint64_t>(clusters_ + 2) * 3 + 1) / 2 :
            static_cast<std::uint64_t>(clusters_ + 2) * (type_ == 16 ? 2 : 4);
        if (fatBytes < needed || fatBytes > std::numeric_limits<std::size_t>::max()) fail("Invalid FAT size");
        fatStart_ = static_cast<std::uint64_t>(reserved) * bytesPerSector_;
        fatSize_ = static_cast<std::size_t>(fatBytes);
        rootStart_ = fatStart_ + fatBytes * copies_;
        rootBytes_ = static_cast<std::uint64_t>(rootEntries) * 32;
        dataStart_ = firstData * bytesPerSector_;
        rootCluster_ = type_ == 32 ? u32(boot + 44) : 0;
        const unsigned flags = type_ == 32 ? u16(boot + 40) : 0;
        const unsigned active = (flags & 0x80) ? (flags & 15) : 0;
        if (active >= copies_ || (type_ == 32 && !validCluster(rootCluster_))) fail("Invalid active FAT or root cluster");
        fat_.resize(fatSize_);
        if (!readAll(source_, fatStart_ + fatBytes * active, fat_.data(), fat_.size())) fail("Cannot read active FAT");
    }

    bool validCluster(std::uint32_t c) const { return c >= 2 && c < clusters_ + 2; }
    bool visited(std::uint32_t c) const { return (visited_[c / 8] & (1U << (c % 8))) != 0; }
    void markVisited(std::uint32_t c) { visited_[c / 8] |= static_cast<std::uint8_t>(1U << (c % 8)); }
    std::uint32_t bad() const { return type_ == 12 ? 0xff7U : type_ == 16 ? 0xfff7U : 0x0ffffff7U; }
    std::uint32_t eoc() const { return bad() + 1; }
    std::uint32_t outputEoc() const { return type_ == 12 ? 0xfffU : type_ == 16 ? 0xffffU : 0x0fffffffU; }
    bool allocated(std::uint32_t value) const {
        return value >= eoc() || (value < bad() - 7 && validCluster(value));
    }
    std::uint32_t get(std::uint32_t c) const {
        const std::size_t p = type_ == 12 ? c + c / 2 : static_cast<std::size_t>(c) * (type_ == 16 ? 2 : 4);
        if (type_ == 12) return (u16(&sourceFat_[p]) >> ((c & 1) ? 4 : 0)) & 0xfff;
        return type_ == 16 ? u16(&sourceFat_[p]) : u32(&sourceFat_[p]) & 0x0fffffffU;
    }
    void set(std::uint32_t c, std::uint32_t value) {
        const std::size_t p = type_ == 12 ? c + c / 2 : static_cast<std::size_t>(c) * (type_ == 16 ? 2 : 4);
        if (type_ == 12) {
            unsigned v = u16(&fat_[p]);
            v = (c & 1) ? ((v & 0x000f) | (value << 4)) : ((v & 0xf000) | value);
            fat_[p] = static_cast<std::uint8_t>(v);
            fat_[p + 1] = static_cast<std::uint8_t>(v >> 8);
        } else {
            const unsigned n = type_ == 16 ? 2 : 4;
            for (unsigned i = 0; i < n; ++i) fat_[p + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }
    std::uint64_t clusterOffset(std::uint32_t c) const {
        return dataStart_ + static_cast<std::uint64_t>(c - 2) * clusterSize_;
    }
    bool selected(std::uint32_t c) const {
        const std::uint32_t bit = c - 2;
        return bit / 8 < bitmapBytes_.size() && (bitmapBytes_[bit / 8] & (1U << (bit % 8))) != 0;
    }
    bool validLongName(const std::vector<LongEntry>& entries, const std::uint8_t* name) const {
        if (entries.empty() || entries.size() > 20 || entries[0].ordinal != (0x40 | entries.size())) return false;
        std::uint8_t sum = 0;
        for (unsigned i = 0; i < 11; ++i)
            sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) + name[i]);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const unsigned expected = static_cast<unsigned>(entries.size() - i) | (i == 0 ? 0x40 : 0);
            if (!entries[i].valid || entries[i].ordinal != expected || entries[i].checksum != sum) return false;
        }
        return true;
    }
    std::vector<std::uint32_t> chain(std::uint32_t first, std::uint64_t count, bool directory) {
        std::vector<std::uint32_t> result;
        std::uint32_t c = first;
        for (;;) {
            if (!validCluster(c) || visited(c)) fail("Invalid, looping or cross-linked chain");
            const std::uint32_t next = get(c);
            if (!allocated(next)) fail("Invalid allocated cluster");
            markVisited(c);
            result.push_back(c);
            if (!directory && result.size() == count) {
                set(c, outputEoc());
                break;
            }
            set(c, next >= eoc() ? outputEoc() : next);
            if (next >= eoc()) {
                if (!directory) fail("File chain is too short");
                break;
            }
            c = next;
        }
        return result;
    }
    void scan() {
        std::vector<Directory> pending(1, Directory{rootCluster_, 0, true});
        while (!pending.empty()) {
            Directory d = pending.back();
            pending.pop_back();
            std::vector<std::pair<std::uint64_t, std::uint64_t> > regions;
            if (d.root && type_ != 32) {
                regions.push_back(std::make_pair(rootStart_, rootBytes_));
            } else {
                std::vector<std::uint32_t> list = chain(d.cluster, 0, true);
                for (std::size_t i = 0; i < list.size(); ++i)
                    regions.push_back(std::make_pair(clusterOffset(list[i]), clusterSize_));
            }
            std::vector<LongEntry> lfn;
            std::uint64_t index = 0;
            bool end = false;
            for (std::size_t r = 0; r < regions.size() && !end; ++r) {
                std::vector<std::uint8_t> sector;
                std::uint64_t sectorStart = 0;
                for (std::uint64_t off = 0; off < regions[r].second; off += 32, ++index) {
                    if (sector.empty() || off - sectorStart >= sector.size()) {
                        sectorStart = off;
                        const std::size_t length = static_cast<std::size_t>(
                            std::min<std::uint64_t>(bytesPerSector_, regions[r].second - off));
                        sector.resize(length);
                        if (!readAll(source_, regions[r].first + off, sector.data(), length))
                            fail("Cannot read directory");
                    }
                    std::uint8_t entry[32];
                    const std::uint64_t pos = regions[r].first + off;
                    std::memcpy(entry, sector.data() + static_cast<std::size_t>(off - sectorStart), sizeof(entry));
                    if (!d.root && index < 2) {
                        const char* name = index == 0 ? ".          " : "..         ";
                        const std::uint32_t ref = u16(entry + 26) | (type_ == 32 ? static_cast<std::uint32_t>(u16(entry + 20)) << 16 : 0);
                        const std::uint32_t expected = index == 0 ? d.cluster : d.parent;
                        if (std::memcmp(entry, name, 11) || !(entry[11] & 0x10) ||
                            (entry[11] & 0x08) || ref != expected)
                            fail("Invalid dot directory entry");
                        continue;
                    }
                    if (entry[0] == 0) { end = true; break; }
                    if (entry[0] == 0xe5) { lfn.clear(); continue; }
                    if (entry[11] == 0x0f) {
                        if (entry[0] & 0x40) lfn.clear();
                        if (lfn.size() < 21)
                            lfn.push_back(LongEntry{pos, entry[0], entry[13], u16(entry + 26) == 0});
                        continue;
                    }
                    if (entry[11] & 0x08) { lfn.clear(); continue; }
                    const bool dir = (entry[11] & 0x10) != 0;
                    const std::uint32_t first = u16(entry + 26) | (type_ == 32 ? static_cast<std::uint32_t>(u16(entry + 20)) << 16 : 0);
                    const std::uint32_t size = u32(entry + 28);
                    if (dir && size != 0) fail("Invalid directory size");
                    if (!dir && size == 0) {
                        lfn.clear();
                        continue;
                    }
                    if (!validCluster(first) || !allocated(get(first))) fail("Invalid entry cluster");
                    if (!selected(first)) {
                        changes_[pos] = 0xe5;
                        if (validLongName(lfn, entry))
                            for (std::size_t i = 0; i < lfn.size(); ++i) changes_[lfn[i].offset] = 0xe5;
                    } else if (dir) {
                        pending.push_back(Directory{first, d.root ? 0U : d.cluster, false});
                    } else {
                        const std::uint64_t count = (static_cast<std::uint64_t>(size) + clusterSize_ - 1) / clusterSize_;
                        if (count > clusters_) fail("File is larger than data region");
                        chain(first, count, false);
                    }
                    lfn.clear();
                }
            }
            if (!d.root && index < 2) fail("Directory is too short");
        }
    }

    ByteStorePtr source_, bitmap_;
    std::uint32_t bytesPerSector_, sectorsPerCluster_, clusterSize_, clusters_;
    unsigned type_;
    std::uint64_t fatStart_;
    std::size_t fatSize_;
    std::uint64_t dataStart_, rootStart_, rootBytes_;
    std::uint32_t rootCluster_;
    unsigned copies_;
    std::vector<std::uint8_t> fat_, sourceFat_, bitmapBytes_, visited_;
    std::map<std::uint64_t, std::uint8_t> changes_;
};

} // namespace

ByteStorePtr reconstructAcronisFat(const ByteStorePtr& source, const ByteStorePtr& bitmap,
                                   std::string* error) {
    if (error) error->clear();
    try {
        return Builder(source, bitmap).build();
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    } catch (...) {
        if (error) *error = "FAT reconstruction failed";
    }
    return ByteStorePtr();
}

} // namespace fs
} // namespace peare
