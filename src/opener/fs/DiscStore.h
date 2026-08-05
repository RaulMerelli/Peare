#pragma once

// Read-only, positioned byte-store layer for the DiscUtils-compatible file
// system stack. This is a faithful C++11 port of the READ side of DiscUtils'
// stream/buffer model:
//
//   IByteStore   <-  DiscUtils.Streams.IBuffer   (positioned, no cursor)
//   MemoryStore  <-  in-memory buffer
//   SubStore     <-  DiscUtils.Streams.SubStream / SubBuffer   (a window)
//   ConcatStore  <-  DiscUtils.Streams.ConcatStream            (concatenation)
//   StripedStore <-  DiscUtils.Streams.StripedStream           (RAID/LDM stripes)
//   ZeroStore    <-  DiscUtils.Streams.ZeroStream              (implicit zeros)
//
// Only the read path is ported (Peare opens images read-only). The types are
// Qt-free so the parsers/codecs can use them directly, and they are the layers
// that ISO9660, WIM (and later others) compose to expose file content lazily.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace peare {
namespace fs {

// Positioned random-access byte store == DiscUtils IBuffer (read side). There is
// no notion of a current position; every read specifies its own position.
class IByteStore {
public:
    virtual ~IByteStore() {}

    // Total number of readable bytes (== IBuffer.Capacity).
    virtual std::int64_t capacity() const = 0;

    // Read up to count bytes starting at pos into dst. Returns the number of
    // bytes actually copied (0 at or past EOF). Implementations clamp to the
    // available range and never throw for an out-of-range request
    // (== IBuffer.Read(pos, buffer, offset, count)).
    virtual int read(std::int64_t pos, std::uint8_t* dst, int count) const = 0;

    // True when positioned reads have roughly constant cost. Sequentially
    // compressed members override this: probing a trailer would otherwise
    // force decompression of the whole file merely while building the tree.
    virtual bool cheapRandomAccess() const { return true; }

    // Convenience helpers layered on read().
    void readExactly(std::int64_t pos, std::uint8_t* dst, int count) const {
        int done = 0;
        while (done < count) {
            const int n = read(pos + done, dst + done, count - done);
            if (n <= 0) break;
            done += n;
        }
        // Anything not produced by the store reads as implicit zero.
        for (; done < count; ++done) dst[done] = 0;
    }

    std::vector<std::uint8_t> readRange(std::int64_t pos, std::int64_t count) const {
        std::vector<std::uint8_t> out;
        const std::int64_t total = capacity();
        if (pos < 0 || pos >= total || count <= 0) return out;
        const std::int64_t avail = total - pos;
        const std::int64_t n = count < avail ? count : avail;
        out.resize(static_cast<std::size_t>(n));
        readExactly(pos, out.data(), static_cast<int>(n));
        return out;
    }

    std::vector<std::uint8_t> readAll() const { return readRange(0, capacity()); }
};

typedef std::shared_ptr<IByteStore> ByteStorePtr;

// Whole buffer held in memory (== an in-memory IBuffer).
class MemoryStore : public IByteStore {
public:
    explicit MemoryStore(std::vector<std::uint8_t> data) : data_(std::move(data)) {}
    MemoryStore(const std::uint8_t* p, std::size_t n) : data_(p, p + n) {}

    std::int64_t capacity() const override {
        return static_cast<std::int64_t>(data_.size());
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= static_cast<std::int64_t>(data_.size()))
            return 0;
        const std::size_t off = static_cast<std::size_t>(pos);
        const std::size_t avail = data_.size() - off;
        const std::size_t n =
            static_cast<std::size_t>(count) < avail ? static_cast<std::size_t>(count) : avail;
        std::copy(data_.begin() + static_cast<std::ptrdiff_t>(off),
                  data_.begin() + static_cast<std::ptrdiff_t>(off + n), dst);
        return static_cast<int>(n);
    }

    const std::vector<std::uint8_t>& bytes() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

// References externally-owned memory without copying it (e.g. a memory-mapped
// file). `owner` (type-erased) is held so the backing stays alive for as long as
// this store — or any window over it — exists. Reads are just pointer copies;
// when the backing is an mmap, the OS pages data in lazily on access.
class ExternalStore : public IByteStore {
public:
    ExternalStore(const std::uint8_t* data, std::int64_t length, std::shared_ptr<void> owner)
        : data_(data), length_(length), owner_(std::move(owner)) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!data_ || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t avail = length_ - pos;
        const int n = count < avail ? count : static_cast<int>(avail);
        std::copy(data_ + pos, data_ + pos + n, dst);
        return n;
    }

private:
    const std::uint8_t* data_;
    std::int64_t length_;
    std::shared_ptr<void> owner_;
};

// A [start, start+length) window over a parent store == DiscUtils SubStream /
// SubBuffer. Zero-copy: forwards positioned reads to the parent.
class SubStore : public IByteStore {
public:
    SubStore(ByteStorePtr parent, std::int64_t start, std::int64_t length)
        : parent_(std::move(parent)), start_(start), length_(length) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!parent_ || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t avail = length_ - pos;
        const int n = count < avail ? count : static_cast<int>(avail);
        return parent_->read(start_ + pos, dst, n);
    }

    const ByteStorePtr& parent() const { return parent_; }
    std::int64_t start() const { return start_; }

private:
    ByteStorePtr parent_;
    std::int64_t start_;
    std::int64_t length_;
};

// View over raw CD-ROM Mode 1 sectors. A 2352-byte physical sector contains
// 16 bytes of sync/header before its 2048-byte user-data area.
class OpticalMode1Store : public IByteStore {
public:
    explicit OpticalMode1Store(ByteStorePtr parent) : parent_(std::move(parent)) {}

    std::int64_t capacity() const override {
        return parent_ ? (parent_->capacity() / kRawSectorSize) * kLogicalSectorSize : 0;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        const std::int64_t total = capacity();
        if (!parent_ || pos < 0 || count <= 0 || pos >= total) return 0;
        const std::int64_t avail = total - pos;
        int want = count < avail ? count : static_cast<int>(avail);
        int produced = 0;
        while (want > 0) {
            const std::int64_t logical = pos + produced;
            const std::int64_t sector = logical / kLogicalSectorSize;
            const std::int64_t sectorOffset = logical - sector * kLogicalSectorSize;
            const int n = static_cast<int>(std::min<std::int64_t>(
                kLogicalSectorSize - sectorOffset, want));
            const int got = parent_->read(sector * kRawSectorSize + kRawPrefixSize + sectorOffset,
                                          dst + produced, n);
            if (got <= 0) break;
            produced += got;
            want -= got;
        }
        return produced;
    }

    static bool detectIso9660(const IByteStore& raw) {
        std::uint8_t id[5];
        const std::int64_t pos = 16 * kRawSectorSize + kRawPrefixSize + 1;
        return raw.read(pos, id, 5) == 5 && std::memcmp(id, "CD001", 5) == 0;
    }

    static bool detectUdf(const IByteStore& raw) {
        std::uint8_t descriptor[6];
        for (int i = 0; i < 64; ++i) {
            const std::int64_t pos = (16 + i) * kRawSectorSize + kRawPrefixSize;
            if (pos + 6 > raw.capacity()) break;
            if (raw.read(pos, descriptor, 6) != 6) break;
            const char* id = reinterpret_cast<const char*>(descriptor + 1);
            if (std::memcmp(id, "NSR02", 5) == 0 || std::memcmp(id, "NSR03", 5) == 0)
                return true;
            if (std::memcmp(id, "BEA01", 5) != 0 && std::memcmp(id, "BOOT2", 5) != 0 &&
                std::memcmp(id, "CD001", 5) != 0 && std::memcmp(id, "CDW02", 5) != 0 &&
                std::memcmp(id, "TEA01", 5) != 0)
                break;
        }
        return false;
    }

private:
    static const std::int64_t kLogicalSectorSize = 2048;
    static const std::int64_t kRawSectorSize = 2352;
    static const std::int64_t kRawPrefixSize = 16;

    ByteStorePtr parent_;
};

// View over raw CD-ROM Mode 2/Form 1 style sectors. DiscUtils.OpticalDisk's
// Mode2Buffer exposes a 2048-byte logical stream by skipping the 24-byte raw
// sector prefix in every 2352-byte physical sector.
class OpticalMode2Store : public IByteStore {
public:
    explicit OpticalMode2Store(ByteStorePtr parent) : parent_(std::move(parent)) {}

    std::int64_t capacity() const override {
        return parent_ ? (parent_->capacity() / kRawSectorSize) * kLogicalSectorSize : 0;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        const std::int64_t total = capacity();
        if (!parent_ || pos < 0 || count <= 0 || pos >= total) return 0;
        const std::int64_t avail = total - pos;
        int want = count < avail ? count : static_cast<int>(avail);
        int produced = 0;
        while (want > 0) {
            const std::int64_t logical = pos + produced;
            const std::int64_t sector = logical / kLogicalSectorSize;
            const std::int64_t sectorOffset = logical - sector * kLogicalSectorSize;
            const int n = static_cast<int>(std::min<std::int64_t>(
                kLogicalSectorSize - sectorOffset, want));
            const int got = parent_->read(sector * kRawSectorSize + kRawPrefixSize + sectorOffset,
                                          dst + produced, n);
            if (got <= 0) break;
            produced += got;
            want -= got;
        }
        return produced;
    }

    static bool detectIso9660(const IByteStore& raw) {
        std::uint8_t id[5];
        const std::int64_t pos = 16 * kRawSectorSize + kRawPrefixSize + 1;
        return raw.read(pos, id, 5) == 5 && std::memcmp(id, "CD001", 5) == 0;
    }

    static bool detectUdf(const IByteStore& raw) {
        std::uint8_t descriptor[6];
        for (int i = 0; i < 64; ++i) {
            const std::int64_t logicalSector = 16 + i;
            const std::int64_t pos = logicalSector * kRawSectorSize + kRawPrefixSize;
            if (pos + 6 > raw.capacity()) break;
            if (raw.read(pos, descriptor, 6) != 6) break;
            const char* id = reinterpret_cast<const char*>(descriptor + 1);
            if (std::memcmp(id, "NSR02", 5) == 0 || std::memcmp(id, "NSR03", 5) == 0)
                return true;
            if (std::memcmp(id, "BEA01", 5) != 0 && std::memcmp(id, "BOOT2", 5) != 0 &&
                std::memcmp(id, "CD001", 5) != 0 && std::memcmp(id, "CDW02", 5) != 0 &&
                std::memcmp(id, "TEA01", 5) != 0)
                break;
        }
        return false;
    }

private:
    static const std::int64_t kLogicalSectorSize = 2048;
    static const std::int64_t kRawSectorSize = 2352;
    static const std::int64_t kRawPrefixSize = 24;

    ByteStorePtr parent_;
};

// Concatenation of N stores presented as one contiguous store == DiscUtils
// ConcatStream. A read that spans part boundaries is split across the covered
// parts. The covering part for a position is found by binary search.
class ConcatStore : public IByteStore {
public:
    explicit ConcatStore(std::vector<ByteStorePtr> parts) : parts_(std::move(parts)) {
        std::int64_t offset = 0;
        starts_.reserve(parts_.size());
        for (const ByteStorePtr& part : parts_) {
            starts_.push_back(offset);
            offset += part ? part->capacity() : 0;
        }
        total_ = offset;
    }

    std::int64_t capacity() const override { return total_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= total_) return 0;
        std::int64_t avail = total_ - pos;
        int want = count < avail ? count : static_cast<int>(avail);

        int produced = 0;
        std::size_t idx = locate(pos);
        while (want > 0 && idx < parts_.size()) {
            const std::int64_t partStart = starts_[idx];
            const ByteStorePtr& part = parts_[idx];
            const std::int64_t within = (pos + produced) - partStart;
            const int n = part ? part->read(within, dst + produced, want) : 0;
            if (n <= 0) {
                // Move to the next part at its start.
                ++idx;
                continue;
            }
            produced += n;
            want -= n;
            if (within + n >= (part ? part->capacity() : 0)) ++idx;
        }
        return produced;
    }

private:
    // Index of the part whose range covers absolute position pos.
    std::size_t locate(std::int64_t pos) const {
        std::size_t lo = 0, hi = starts_.size();
        while (lo + 1 < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (starts_[mid] <= pos) lo = mid; else hi = mid;
        }
        return lo;
    }

    std::vector<ByteStorePtr> parts_;
    std::vector<std::int64_t> starts_;
    std::int64_t total_ = 0;
};

// Interleaves fixed-size chunks from N stores == DiscUtils StripedStream. A
// logical stripe 0 comes from part 0, stripe 1 from part 1, then wraps.
class StripedStore : public IByteStore {
public:
    StripedStore(std::int64_t stripeSize, std::vector<ByteStorePtr> parts)
        : stripeSize_(stripeSize), parts_(std::move(parts)) {
        total_ = 0;
        for (const ByteStorePtr& part : parts_)
            total_ += part ? part->capacity() : 0;
    }

    std::int64_t capacity() const override { return total_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (stripeSize_ <= 0 || parts_.empty() || pos < 0 || count <= 0 || pos >= total_)
            return 0;
        const std::int64_t avail = total_ - pos;
        int want = count < avail ? count : static_cast<int>(avail);
        int produced = 0;
        while (want > 0) {
            const std::int64_t logical = pos + produced;
            const std::int64_t stripe = logical / stripeSize_;
            const std::int64_t stripeOffset = logical - stripe * stripeSize_;
            const std::size_t partIndex = static_cast<std::size_t>(stripe % parts_.size());
            const std::int64_t stripeInPart = stripe / static_cast<std::int64_t>(parts_.size());
            const std::int64_t partPos = stripeInPart * stripeSize_ + stripeOffset;
            const int n = static_cast<int>(std::min<std::int64_t>(stripeSize_ - stripeOffset, want));
            const ByteStorePtr& part = parts_[partIndex];
            const int got = part ? part->read(partPos, dst + produced, n) : 0;
            if (got <= 0) break;
            produced += got;
            want -= got;
        }
        return produced;
    }

private:
    std::int64_t stripeSize_;
    std::vector<ByteStorePtr> parts_;
    std::int64_t total_ = 0;
};

// A store of a fixed length whose bytes are all zero == DiscUtils ZeroStream.
// Used as the implicit backing for sparse/unallocated regions.
class ZeroStore : public IByteStore {
public:
    explicit ZeroStore(std::int64_t length) : length_(length) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t avail = length_ - pos;
        const int n = count < avail ? count : static_cast<int>(avail);
        std::fill(dst, dst + n, std::uint8_t(0));
        return n;
    }

private:
    std::int64_t length_;
};

}  // namespace fs
}  // namespace peare
