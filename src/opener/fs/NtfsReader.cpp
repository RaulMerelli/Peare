#include "NtfsReader.h"

#include "BlockDecompressor.h"
#include "PartitionTable.h"
#include "TibxVolumeStore.h"
#include "WimReader.h"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <climits>
#include <cctype>
#include <cstring>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>

namespace peare {
namespace fs {
namespace {

// NTFS update-sequence entries use 512-byte strides, independent of the BPB.
constexpr std::size_t kNtfsUpdateSequenceUnit = 512;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

// Little-endian variable-length integer used in data runs. `sign` extends the
// most-significant bit (the run-offset delta is signed; the length is not).
std::int64_t readVarInt(const std::uint8_t* p, int n, bool sign) {
    std::int64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::int64_t>(p[i]) << (8 * i);
    if (sign && n > 0 && (p[n - 1] & 0x80))
        v |= -(std::int64_t(1) << (8 * n));  // sign-extend
    return v;
}

bool utf16leToUtf8(const std::uint8_t* p, int chars, std::string* result) {
    if (!p || chars < 0 || !result) return false;
    std::string out;
    for (int i = 0; i < chars; ++i) {
        unsigned int ch = le16(p + static_cast<std::size_t>(i) * 2);
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 1 >= chars) return false;
            const unsigned int low = le16(p + static_cast<std::size_t>(i + 1) * 2);
            if (low < 0xdc00 || low > 0xdfff) return false;
            ch = 0x10000 + ((ch - 0xd800) << 10) + (low - 0xdc00);
            ++i;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return false;
        }
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else if (ch < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    *result = std::move(out);
    return true;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void splitPath(const std::string& path, std::vector<std::string>* out) {
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out->push_back(cur);
}

const std::uint32_t kAttrAttributeList = 0x20;
const std::uint32_t kAttrData = 0x80;
const std::uint32_t kAttrIndexRoot = 0x90;
const std::uint32_t kAttrIndexAllocation = 0xA0;
const std::uint32_t kAttrBitmap = 0xB0;
const std::uint32_t kAttrReparsePoint = 0xC0;
const std::uint32_t kWofReparseTag = 0x80000017u;
const std::uint32_t kWofFileProvider = 2;

struct WofPhysicalRun {
    std::uint64_t start, length, physical;
};

class NtfsRunsStore final : public IByteStore {
public:
    NtfsRunsStore(ByteStorePtr source, std::vector<WofPhysicalRun> runs, std::int64_t length)
        : source_(std::move(source)), runs_(std::move(runs)), length_(length) {}
    std::int64_t capacity() const override { return length_; }
    bool validate() const override { return source_ && source_->validate(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || pos >= length_) return 0;
        const int wanted = static_cast<int>(std::min<std::int64_t>(count, length_ - pos));
        int done = 0;
        for (const WofPhysicalRun& run : runs_) {
            const std::uint64_t at = static_cast<std::uint64_t>(pos) + done;
            if (at >= run.start + run.length) continue;
            if (at < run.start) return -1;
            const int n = static_cast<int>(std::min<std::uint64_t>(wanted - done, run.length - (at - run.start)));
            if (run.physical == UINT64_MAX) {
                std::fill(dst + done, dst + done + n, 0);
                done += n;
            } else {
                int copied = 0;
                while (copied < n) {
                    const int got = source_->read(static_cast<std::int64_t>(run.physical + at - run.start + copied),
                                                  dst + done + copied, n - copied);
                    if (got <= 0 || got > n - copied) return -1;
                    copied += got;
                }
                done += n;
            }
            if (done == wanted) return validate() ? done : -1;
        }
        return -1;
    }
private:
    ByteStorePtr source_;
    std::vector<WofPhysicalRun> runs_;
    std::int64_t length_;
};

class WofNtfsStore final : public IByteStore {
public:
    WofNtfsStore(ByteStorePtr source, std::vector<WofPhysicalRun> runs, std::int64_t length)
        : source_(std::move(source)), runs_(std::move(runs)), length_(length) {}
    std::int64_t capacity() const override { return length_; }
    bool validate() const override { return source_ && source_->validate(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || pos >= length_) return 0;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, length_ - pos));
        int done = 0;
        for (std::size_t i = 0; i < runs_.size() && done < amount; ++i) {
            const WofPhysicalRun& r = runs_[i];
            const std::uint64_t at = static_cast<std::uint64_t>(pos) + done;
            if (at >= r.start + r.length) continue;
            if (at < r.start) return -1;
            int remaining = static_cast<int>(std::min<std::uint64_t>(amount - done, r.length - (at - r.start)));
            std::uint64_t physical = r.physical + at - r.start;
            while (remaining) {
                const int got = source_->read(static_cast<std::int64_t>(physical), dst + done, remaining);
                if (got <= 0 || got > remaining) return -1;
                physical += got; done += got; remaining -= got;
            }
        }
        return done == amount && validate() ? done : -1;
    }
private:
    ByteStorePtr source_;
    std::vector<WofPhysicalRun> runs_;
    std::int64_t length_;
};

bool readWofNtfsExact(const ByteStorePtr& source, std::uint64_t offset,
                      std::uint8_t* dst, std::size_t size) {
    if (!source || !source->validate() || source->capacity() < 0 ||
        offset > static_cast<std::uint64_t>(source->capacity()) ||
        size > static_cast<std::uint64_t>(source->capacity()) - offset) return false;
    while (size) {
        const int n = static_cast<int>(std::min<std::size_t>(size, 65536));
        const int got = source->read(static_cast<std::int64_t>(offset), dst, n);
        if (got <= 0 || got > n) return false;
        offset += got; dst += got; size -= static_cast<std::size_t>(got);
    }
    return source->validate();
}

struct LzntRun {
    std::uint64_t start;
    std::uint64_t length;
    std::int64_t lcn;
};

enum class LzntResult { Complete, EndOfBuffer, Invalid };

LzntResult decodeLznt1(const std::vector<std::uint8_t>& input, std::size_t required,
                       std::vector<std::uint8_t>* output) {
    std::size_t pos = 0, written = 0;
    while (written < required) {
        if (input.size() - pos < 2) return LzntResult::Invalid;
        const std::uint16_t header = le16(input.data() + pos);
        pos += 2;
        if (!header) return LzntResult::EndOfBuffer;
        if ((header & 0x7000) != 0x3000) return LzntResult::Invalid;
        const std::size_t size = (header & 0x0fff) + 1;
        if (size > input.size() - pos) return LzntResult::Invalid;
        const std::size_t end = pos + size;
        std::vector<std::uint8_t> chunk;
        chunk.reserve(4096);
        if (!(header & 0x8000)) {
            chunk.insert(chunk.end(), input.begin() + pos, input.begin() + end);
            pos = end;
        } else {
            while (pos < end) {
                const unsigned int flags = input[pos++];
                if (pos == end) return LzntResult::Invalid;
                for (unsigned int bit = 0; bit < 8 && pos < end; ++bit) {
                    if (!(flags & (1u << bit))) {
                        if (chunk.size() == 4096) return LzntResult::Invalid;
                        chunk.push_back(input[pos++]);
                    } else {
                        if (end - pos < 2 || chunk.empty()) return LzntResult::Invalid;
                        const unsigned int token = le16(input.data() + pos);
                        pos += 2;
                        unsigned int bits = 12;
                        for (std::size_t n = chunk.size() - 1; n >= 16; n >>= 1) --bits;
                        const std::size_t distance = (token >> bits) + 1;
                        const std::size_t length = (token & ((1u << bits) - 1)) + 3;
                        if (distance > chunk.size() || length > 4096 - chunk.size()) return LzntResult::Invalid;
                        for (std::size_t i = 0; i < length; ++i)
                            chunk.push_back(chunk[chunk.size() - distance]);
                    }
                }
            }
        }
        if (chunk.empty()) return LzntResult::Invalid;
        if (chunk.size() < 4096 && chunk.size() < required - written) {
            if (input.size() - pos >= 2 && le16(input.data() + pos) == 0)
                return LzntResult::EndOfBuffer;
            return LzntResult::Invalid;
        }
        const std::size_t n = std::min(chunk.size(), required - written);
        std::copy(chunk.begin(), chunk.begin() + n, output->begin() + written);
        written += n;
    }
    return LzntResult::Complete;
}

class LzntFileStore final : public IByteStore {
public:
    LzntFileStore(ByteStorePtr source, std::vector<LzntRun> runs, std::uint64_t size,
                  std::uint64_t initialized, std::uint32_t cluster, std::uint16_t exponent,
                  bool valid)
        : source_(std::move(source)), runs_(std::move(runs)), size_(size), initialized_(initialized),
          cluster_(cluster), valid_(valid) {
        if (!source_ || size > INT64_MAX || initialized > size || !cluster || exponent >= 63 ||
            static_cast<std::uint64_t>(cluster) > (static_cast<std::uint64_t>(INT64_MAX) >> exponent)) {
            valid_ = false;
        } else {
            unitClusters_ = std::uint64_t(1) << exponent;
            unitBytes_ = unitClusters_ * cluster;
        }
    }
    std::int64_t capacity() const override { return size_ <= INT64_MAX ? static_cast<std::int64_t>(size_) : -1; }
    bool validate() const override { return valid_ && source_->validate(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || static_cast<std::uint64_t>(pos) >= size_) return 0;
        const int want = static_cast<int>(std::min<std::uint64_t>(count, size_ - pos));
        int done = 0;
        try {
            while (done < want) {
                const std::uint64_t at = static_cast<std::uint64_t>(pos) + done;
                if (at >= initialized_) { std::memset(dst + done, 0, want - done); break; }
                const std::uint64_t index = at / unitBytes_;
                std::shared_ptr<const std::vector<std::uint8_t> > unit = load(index);
                if (!unit) return -1;
                const std::size_t offset = static_cast<std::size_t>(at % unitBytes_);
                if (offset >= unit->size()) return -1;
                const std::size_t n = std::min<std::size_t>(want - done, unit->size() - offset);
                std::memcpy(dst + done, unit->data() + offset, n);
                done += static_cast<int>(n);
            }
        } catch (const std::exception&) { return -1; }
        return want;
    }
private:
    bool readExact(std::uint64_t at, std::uint8_t* dst, std::size_t count) const {
        const std::int64_t cap = source_->capacity();
        if (cap < 0 || at > static_cast<std::uint64_t>(cap) || count > static_cast<std::uint64_t>(cap) - at) return false;
        while (count) {
            const int n = static_cast<int>(std::min<std::size_t>(count, INT_MAX));
            const int got = source_->read(static_cast<std::int64_t>(at), dst, n);
            if (got <= 0 || got > n) return false;
            at += got; dst += got; count -= got;
        }
        return true;
    }
    std::shared_ptr<const std::vector<std::uint8_t> > load(std::uint64_t index) const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cache_ && cachedIndex_ == index) return cache_;
        }
        const std::uint64_t start = index * unitBytes_;
        const std::uint64_t bytes = std::min(unitBytes_, size_ - start);
        if (bytes > std::vector<std::uint8_t>().max_size()) return {};
        std::shared_ptr<std::vector<std::uint8_t> > out = std::make_shared<std::vector<std::uint8_t> >(static_cast<std::size_t>(bytes), 0);
        const std::uint64_t first = index * unitClusters_;
        const std::uint64_t requiredClusters = bytes / cluster_ + (bytes % cluster_ != 0);
        std::uint64_t cursor = first, allocated = 0;
        bool sparse = false;
        struct Part { std::uint64_t physical, bytes; };
        std::vector<Part> parts;
        for (std::size_t i = 0; i < runs_.size() && cursor - first < unitClusters_; ++i) {
            const LzntRun& r = runs_[i];
            if (r.start + r.length <= cursor) continue;
            if (r.start > cursor) return {};
            const std::uint64_t n = std::min(r.length - (cursor - r.start), unitClusters_ - (cursor - first));
            if (r.lcn < 0) sparse = true;
            else {
                if (sparse || static_cast<std::uint64_t>(r.lcn) > UINT64_MAX - (cursor - r.start)) return {};
                const std::uint64_t lcn = static_cast<std::uint64_t>(r.lcn) + cursor - r.start;
                if (lcn > UINT64_MAX / cluster_ || n > UINT64_MAX / cluster_) return {};
                Part p = { lcn * cluster_, n * cluster_ }; parts.push_back(p);
                allocated += n;
            }
            cursor += n;
        }
        if (cursor - first < requiredClusters) return {};
        const std::size_t needed = static_cast<std::size_t>(std::min(bytes, initialized_ - start));
        if (allocated && !sparse) {
            std::size_t offset = 0;
            for (std::size_t i = 0; i < parts.size() && offset < needed; ++i) {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(parts[i].bytes, needed - offset));
                if (!readExact(parts[i].physical, out->data() + offset, n)) return {};
                offset += n;
            }
            if (offset != needed) return {};
        } else if (allocated) {
            if (allocated > std::vector<std::uint8_t>().max_size() / cluster_) return {};
            std::vector<std::uint8_t> packed(static_cast<std::size_t>(allocated * cluster_));
            std::size_t offset = 0;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                const std::size_t n = static_cast<std::size_t>(parts[i].bytes);
                if (!readExact(parts[i].physical, packed.data() + offset, n)) return {};
                offset += n;
            }
            if (decodeLznt1(packed, needed, out.get()) != LzntResult::Complete) return {};
        }
        if (bytes <= 32u * 1024u * 1024u) {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_ = out; cachedIndex_ = index;
        }
        return out;
    }
    ByteStorePtr source_;
    std::vector<LzntRun> runs_;
    std::uint64_t size_, initialized_, unitClusters_ = 0, unitBytes_ = 0;
    std::uint32_t cluster_;
    bool valid_;
    mutable std::mutex mutex_;
    mutable std::uint64_t cachedIndex_ = 0;
    mutable std::shared_ptr<const std::vector<std::uint8_t> > cache_;
};

class WofFileStore final : public IByteStore {
public:
    WofFileStore(ByteStorePtr compressed, std::uint64_t originalSize, std::uint32_t algorithm)
        : compressed_(std::move(compressed)), originalSize_(static_cast<std::int64_t>(originalSize)) {
        static const std::int64_t chunkSizes[] = {4096, 32768, 8192, 16384};
        if (!compressed_ || !compressed_->validate() || compressed_->capacity() < 0 ||
            originalSize == 0 || originalSize > INT64_MAX || algorithm >= 4)
            return;
        chunkSize_ = chunkSizes[algorithm];
        chunkCount_ = 1 + (originalSize_ - 1) / chunkSize_;
        entrySize_ = originalSize > UINT32_MAX ? 8 : 4;
        if (chunkCount_ - 1 > UINT64_MAX / entrySize_) return;
        tableSize_ = (chunkCount_ - 1) * entrySize_;
        if (tableSize_ > static_cast<std::uint64_t>(compressed_->capacity())) return;
        dataSize_ = static_cast<std::uint64_t>(compressed_->capacity()) - tableSize_;
        if (algorithm == 1)
            lzx_ = std::make_shared<LzxWimDecompressor>(static_cast<std::size_t>(chunkSize_));
        else
            xpress_ = std::make_shared<XpressHuffmanDecompressor>();
        valid_ = validateChunkTable();
    }

    std::int64_t capacity() const override { return valid_ ? originalSize_ : 0; }
    bool validate() const override { return valid_ && compressed_ && compressed_->validate(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || pos >= originalSize_) return 0;
        try {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            const int want = static_cast<int>(std::min<std::int64_t>(
                std::min<std::int64_t>(count, originalSize_ - pos), 1024 * 1024));
            std::vector<std::uint8_t> staged(static_cast<std::size_t>(want));
            int produced = 0;
            while (produced < want) {
                const std::uint64_t current = static_cast<std::uint64_t>(pos + produced);
                const std::uint64_t index = current / static_cast<std::uint64_t>(chunkSize_);
                const int offset = static_cast<int>(current % static_cast<std::uint64_t>(chunkSize_));
                if (!loadChunk(index)) return -1;
                const int n = std::min(want - produced, static_cast<int>(cache_.size()) - offset);
                if (n <= 0) return -1;
                std::copy(cache_.begin() + offset, cache_.begin() + offset + n, staged.begin() + produced);
                produced += n;
            }
            if (!validate()) return -1;
            std::copy(staged.begin(), staged.end(), dst);
            return produced;
        } catch (const std::exception&) { return -1; }
    }

private:
    bool readOffset(std::uint64_t index, std::uint64_t* value) const {
        if (!value || index >= chunkCount_) return false;
        if (index == 0) { *value = 0; return true; }
        const std::uint64_t at = (index - 1) * entrySize_;
        std::uint8_t bytes[8] = {};
        if (!readWofNtfsExact(compressed_, at, bytes, static_cast<std::size_t>(entrySize_))) return false;
        *value = entrySize_ == 4 ? le32(bytes) : le64(bytes);
        return true;
    }

    bool validateChunkTable() const {
        const std::uint64_t entries = chunkCount_ - 1;
        const std::uint64_t limit = dataSize_;
        std::uint64_t previous = 0;
        const std::uint64_t entriesPerBlock = 4096 / entrySize_;
        for (std::uint64_t first = 0; first < entries; first += entriesPerBlock) {
            const std::uint64_t count = std::min(entriesPerBlock, entries - first);
            const std::uint64_t byteOffset = first * entrySize_;
            std::vector<std::uint8_t> block(static_cast<std::size_t>(count * entrySize_));
            if (!readWofNtfsExact(compressed_, byteOffset, block.data(), block.size())) return false;
            for (std::uint64_t i = 0; i < count; ++i) {
                const std::uint8_t* p = block.data() + i * entrySize_;
                const std::uint64_t offset = entrySize_ == 4 ? le32(p) : le64(p);
                if (offset <= previous || offset > limit) return false;
                previous = offset;
            }
        }
        return entries == 0 || previous < limit;
    }

    bool loadChunk(std::uint64_t index) const {
        if (cachedChunk_ == index) return true;
        if (index >= chunkCount_) return false;
        std::uint64_t begin = 0;
        std::uint64_t end = dataSize_;
        if (!readOffset(index, &begin) ||
            (index + 1 < chunkCount_ && !readOffset(index + 1, &end)) ||
            end < begin || end > dataSize_) return false;
        const std::uint64_t outOffset = index * static_cast<std::uint64_t>(chunkSize_);
        const std::size_t outSize = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunkSize_, static_cast<std::uint64_t>(originalSize_) - outOffset));
        const std::uint64_t storedSize = end - begin;
        if (storedSize == 0 || storedSize > outSize || storedSize > SIZE_MAX) return false;
        std::vector<std::uint8_t> input(static_cast<std::size_t>(storedSize));
        if (!readWofNtfsExact(compressed_, tableSize_ + begin, input.data(), input.size())) return false;
        std::vector<std::uint8_t> decodedChunk(outSize);
        if (storedSize == outSize) {
            std::copy(input.begin(), input.end(), decodedChunk.begin());
        } else {
            std::size_t decoded = 0;
            const bool ok = lzx_
                ? lzx_->tryDecompress(input.data(), input.size(), decodedChunk.data(), outSize, decoded)
                : xpress_->tryDecompress(input.data(), input.size(), decodedChunk.data(), outSize, decoded);
            if (!ok || decoded != outSize) return false;
        }
        cache_.swap(decodedChunk);
        cachedChunk_ = index;
        return true;
    }

    ByteStorePtr compressed_;
    std::int64_t originalSize_ = 0;
    std::int64_t chunkSize_ = 0;
    std::uint64_t chunkCount_ = 0;
    std::uint64_t entrySize_ = 0;
    std::uint64_t tableSize_ = 0;
    std::uint64_t dataSize_ = 0;
    bool valid_ = false;
    BlockDecompressorPtr xpress_;
    BlockDecompressorPtr lzx_;
    mutable std::mutex cacheMutex_;
    mutable std::uint64_t cachedChunk_ = UINT64_MAX;
    mutable std::vector<std::uint8_t> cache_;
};

}  // namespace

NtfsReader::NtfsReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try { parse(); }
    catch (...) { if (error_.empty()) error_ = "NTFS parse error"; valid_ = false; }
}

bool NtfsReader::partitionContext(PartitionIdentity* identity,
                                  std::shared_ptr<const IPartitionResolver>* resolver) const {
    return fs::partitionContext(disc_, identity, resolver);
}

bool NtfsReader::applyFixup(std::vector<std::uint8_t>& buf) const {
    if (buf.size() < 8 || (buf.size() % kNtfsUpdateSequenceUnit) != 0)
        return false;
    const std::uint16_t usnOffset = le16(buf.data() + 0x04);
    const std::uint16_t usnCount = le16(buf.data() + 0x06);
    const std::size_t expected = buf.size() / kNtfsUpdateSequenceUnit + 1;
    if (usnCount != expected || usnCount < 2 ||
        static_cast<std::size_t>(usnOffset) + static_cast<std::size_t>(usnCount) * 2 >
            buf.size())
        return false;
    const std::uint16_t usn = le16(buf.data() + usnOffset);
    for (std::uint16_t i = 1; i < usnCount; ++i) {
        const std::size_t tail =
            static_cast<std::size_t>(i) * kNtfsUpdateSequenceUnit - 2;
        if (tail + 2 > buf.size() || le16(buf.data() + tail) != usn) return false;
        buf[tail] = buf[usnOffset + 2 * i];
        buf[tail + 1] = buf[usnOffset + 2 * i + 1];
    }
    return true;
}

std::vector<NtfsReader::Run> NtfsReader::parseRunlist(const std::uint8_t* p, std::size_t len) const {
    std::vector<Run> runs;
    std::size_t pos = 0;
    std::int64_t lcn = 0;
    while (pos < len && p[pos] != 0) {
        const int lenSize = p[pos] & 0x0F;
        const int offSize = (p[pos] >> 4) & 0x0F;
        if (lenSize == 0 || pos + 1 + lenSize + offSize > len) break;
        const std::int64_t runLen = readVarInt(p + pos + 1, lenSize, false);
        if (runLen <= 0) break;
        Run r;
        r.length = runLen;
        if (offSize == 0) {
            r.lcn = -1;  // sparse
        } else {
            lcn += readVarInt(p + pos + 1 + lenSize, offSize, true);
            r.lcn = lcn;
        }
        runs.push_back(r);
        pos += 1 + lenSize + offSize;
    }
    return runs;
}

bool NtfsReader::parseCompressedRunlist(const std::uint8_t* p, std::size_t len,
                                        std::vector<Run>* runs) const {
    if (!p || !runs) return false;
    std::vector<Run> result;
    std::size_t pos = 0;
    std::uint64_t lcn = 0;
    while (pos < len && p[pos] != 0) {
        const unsigned int lengthBytes = p[pos] & 0x0fU;
        const unsigned int offsetBytes = p[pos] >> 4;
        ++pos;
        if (!lengthBytes || lengthBytes > 8 || offsetBytes > 8 ||
            lengthBytes + offsetBytes > len - pos) return false;
        std::uint64_t length = 0, delta = 0;
        for (unsigned int i = 0; i < lengthBytes; ++i)
            length |= std::uint64_t(p[pos + i]) << (i * 8);
        if (!length || length > std::uint64_t(INT64_MAX)) return false;
        pos += lengthBytes;
        Run run;
        run.length = static_cast<std::int64_t>(length);
        if (!offsetBytes) {
            run.lcn = -1;
        } else {
            for (unsigned int i = 0; i < offsetBytes; ++i)
                delta |= std::uint64_t(p[pos + i]) << (i * 8);
            const bool negative = (p[pos + offsetBytes - 1] & 0x80U) != 0;
            if (negative) {
                if (offsetBytes < 8) delta |= (~std::uint64_t(0) << (offsetBytes * 8));
                const std::uint64_t magnitude = ~delta + 1;
                if (magnitude > lcn) return false;
                lcn -= magnitude;
            } else {
                if (delta > std::uint64_t(INT64_MAX) - lcn) return false;
                lcn += delta;
            }
            run.lcn = static_cast<std::int64_t>(lcn);
        }
        pos += offsetBytes;
        result.push_back(run);
    }
    if (pos >= len || p[pos] != 0) return false;
    runs->swap(result);
    return true;
}

ByteStorePtr NtfsReader::runsStore(const std::vector<Run>& runs, std::uint64_t realSize) const {
    if (!disc_ || disc_->capacity() < 0 || realSize > std::uint64_t(INT64_MAX)) return nullptr;
    std::vector<WofPhysicalRun> parts;
    std::uint64_t logical = 0;
    for (const Run& r : runs) {
        if (r.length <= 0 || r.length > INT64_MAX / bytesPerCluster_) return nullptr;
        const std::uint64_t bytes = static_cast<std::uint64_t>(r.length) * bytesPerCluster_;
        if (bytes > UINT64_MAX - logical) return nullptr;
        std::uint64_t physical = UINT64_MAX;
        if (r.lcn >= 0) {
            if (r.lcn > INT64_MAX / bytesPerCluster_) return nullptr;
            physical = static_cast<std::uint64_t>(r.lcn) * bytesPerCluster_;
            if (physical > static_cast<std::uint64_t>(disc_->capacity()) ||
                bytes > static_cast<std::uint64_t>(disc_->capacity()) - physical) return nullptr;
        } else if (r.lcn != -1) return nullptr;
        WofPhysicalRun part = {logical, bytes, physical};
        parts.push_back(part);
        logical += bytes;
    }
    if (logical < realSize) return nullptr;
    return std::make_shared<NtfsRunsStore>(disc_, std::move(parts), static_cast<std::int64_t>(realSize));
}

NtfsReader::Record NtfsReader::parseRecord(std::vector<std::uint8_t>& buf) const {
    Record rec;
    if (buf.size() < 0x30 || std::memcmp(buf.data(), "FILE", 4) != 0) return rec;
    if (!applyFixup(buf)) return rec;
    rec.sequence = le16(buf.data() + 0x10);
    rec.baseReference = le64(buf.data() + 0x20);
    const std::uint16_t flags = le16(buf.data() + 0x16);
    rec.isDirectory = (flags & 0x0002) != 0;
    std::size_t focus = le16(buf.data() + 0x14);  // first attribute offset

    while (focus + 4 <= buf.size()) {
        const std::uint32_t type = le32(buf.data() + focus);
        if (type == 0xFFFFFFFFu) break;
        if (focus + 0x10 > buf.size()) break;
        const std::uint32_t length = le32(buf.data() + focus + 0x04);
        if (length < 0x10 || focus + length > buf.size()) break;
        const std::uint8_t* a = buf.data() + focus;

        Attr attr;
        attr.type = type;
        attr.nonResident = a[0x08] != 0;
        const std::uint8_t nameLen = a[0x09];
        const std::uint16_t nameOff = le16(a + 0x0A);
        attr.flags = le16(a + 0x0C);
        attr.id = le16(a + 0x0E);
        if (nameLen) {
            if (static_cast<std::uint32_t>(nameOff) + static_cast<std::uint32_t>(nameLen) * 2 > length ||
                !utf16leToUtf8(a + nameOff, nameLen, &attr.name)) {
                focus += length;
                continue;
            }
        }

        if (!attr.nonResident) {
            if (length < 0x18) { focus += length; continue; }
            const std::uint32_t dataLen = le32(a + 0x10);
            const std::uint16_t dataOff = le16(a + 0x14);
            if (dataOff <= length && dataLen <= length - dataOff) {
                attr.residentData.assign(a + dataOff, a + dataOff + dataLen);
                attr.realSize = dataLen;
            }
        } else {
            if (length < 0x40) { focus += length; continue; }
            attr.lowestVcn = le64(a + 0x10);
            attr.compressionUnit = le16(a + 0x22);
            attr.allocatedSize = le64(a + 0x28);
            attr.realSize = le64(a + 0x30);  // real (used) size
            attr.initializedSize = le64(a + 0x38);
            const std::uint16_t runsOff = le16(a + 0x20);
            if (runsOff < length) {
                std::vector<Run> checked;
                attr.strictRunsValid = parseCompressedRunlist(a + runsOff, length - runsOff, &checked);
                const std::uint64_t highest = le64(a + 0x18);
                std::uint64_t total = 0;
                for (const Run& run : checked) {
                    const std::uint64_t n = static_cast<std::uint64_t>(run.length);
                    if (n > UINT64_MAX - total) { attr.strictRunsValid = false; break; }
                    total += n;
                }
                if (highest < attr.lowestVcn || highest - attr.lowestVcn == UINT64_MAX ||
                    total != highest - attr.lowestVcn + 1) attr.strictRunsValid = false;
                if (attr.strictRunsValid) attr.strictRuns = std::move(checked);
                if (attr.flags & 0x0001)
                    attr.compressedRunsValid = parseCompressedRunlist(a + runsOff, length - runsOff, &attr.runs);
                else
                    attr.runs = parseRunlist(a + runsOff, length - runsOff);
            }
        }
        rec.attrs.push_back(std::move(attr));
        focus += length;
    }
    rec.valid = true;
    return rec;
}

NtfsReader::Record NtfsReader::readRecordRaw(std::uint64_t index) const {
    Record rec;
    if (!mftStore_ || index > static_cast<std::uint64_t>(INT64_MAX / mftRecordSize_))
        return rec;
    std::vector<std::uint8_t> buf(mftRecordSize_);
    if (!readWofNtfsExact(mftStore_, index * mftRecordSize_, buf.data(), buf.size())) return rec;
    return parseRecord(buf);
}

void NtfsReader::normalizeAttributes(Record* rec) const {
    if (!rec) return;
    typedef std::tuple<std::uint32_t, std::string, std::uint16_t> Key;
    std::map<Key, std::vector<Attr> > groups;
    std::vector<Attr> resident;
    for (std::size_t i = 0; i < rec->attrs.size(); ++i) {
        const Attr& a = rec->attrs[i];
        if (a.nonResident)
            groups[Key(a.type, a.name, a.id)].push_back(a);
        else
            resident.push_back(a);
    }
    for (std::map<Key, std::vector<Attr> >::iterator it = groups.begin();
         it != groups.end(); ++it) {
        std::vector<Attr>& pieces = it->second;
        std::sort(pieces.begin(), pieces.end(), [](const Attr& a, const Attr& b) {
            return a.lowestVcn < b.lowestVcn;
        });
        Attr merged = pieces.front();
        merged.runs.clear();
        merged.strictRuns.clear();
        std::uint64_t currentVcn = 0;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const Attr& piece = pieces[i];
            if (!piece.strictRunsValid || piece.lowestVcn != currentVcn)
                merged.strictRunsValid = false;
            merged.strictRuns.insert(merged.strictRuns.end(), piece.strictRuns.begin(), piece.strictRuns.end());
            if (((merged.flags | piece.flags) & 0x0001) &&
                (!(merged.flags & 0x0001) || !(piece.flags & 0x0001) ||
                 !piece.compressedRunsValid || piece.lowestVcn != currentVcn))
                merged.compressedRunsValid = false;
            if (piece.lowestVcn > currentVcn) {
                Run gap;
                gap.lcn = -1;
                gap.length = static_cast<std::int64_t>(piece.lowestVcn - currentVcn);
                merged.runs.push_back(gap);
                currentVcn = piece.lowestVcn;
            }
            for (std::size_t r = 0; r < piece.runs.size(); ++r) {
                merged.runs.push_back(piece.runs[r]);
                const std::uint64_t length = static_cast<std::uint64_t>(piece.runs[r].length);
                if (length > UINT64_MAX - currentVcn) {
                    merged.compressedRunsValid = false;
                    merged.strictRunsValid = false;
                }
                else currentVcn += length;
            }
            if (piece.realSize > merged.realSize) merged.realSize = piece.realSize;
            merged.flags |= piece.flags;
        }
        resident.push_back(merged);
    }
    rec->attrs.swap(resident);
}

void NtfsReader::mergeAttributeList(std::uint64_t baseIndex, Record* rec) const {
    if (!rec || !rec->valid || !mftStore_) return;
    typedef std::tuple<std::uint64_t, std::uint16_t, std::uint32_t, std::uint16_t,
                       std::uint64_t, std::string> Reference;
    std::set<Reference> references;
    for (std::size_t a = 0; a < rec->attrs.size(); ++a) {
        if (rec->attrs[a].type != kAttrAttributeList) continue;
        const std::vector<std::uint8_t> bytes = attrBytes(rec->attrs[a]);
        if (bytes.size() != rec->attrs[a].realSize) { rec->valid = false; return; }
        std::size_t pos = 0;
        while (pos + 26 <= bytes.size()) {
            const std::uint32_t type = le32(bytes.data() + pos);
            if (type == 0xFFFFFFFFu) break;
            const std::uint16_t length = le16(bytes.data() + pos + 4);
            if (length < 26 || pos + length > bytes.size()) { rec->valid = false; return; }
            const std::uint8_t nameLength = bytes[pos + 6], nameOffset = bytes[pos + 7];
            std::string name;
            if (nameLength && (nameOffset < 26 || std::size_t(nameOffset) + std::size_t(nameLength) * 2 > length ||
                !utf16leToUtf8(bytes.data() + pos + nameOffset, nameLength, &name))) {
                rec->valid = false; return;
            }
            const std::uint64_t fileReference = le64(bytes.data() + pos + 16);
            const std::uint64_t ref = fileReference & 0x0000FFFFFFFFFFFFull;
            if (ref != baseIndex) references.insert(Reference(ref, static_cast<std::uint16_t>(fileReference >> 48),
                type, le16(bytes.data() + pos + 24), le64(bytes.data() + pos + 8), name));
            pos += length;
        }
    }

    std::set<std::tuple<std::uint32_t, std::string, std::uint16_t, std::uint64_t> > seen;
    for (std::size_t i = 0; i < rec->attrs.size(); ++i) {
        const Attr& a = rec->attrs[i];
        seen.insert(std::make_tuple(a.type, a.name, a.id, a.lowestVcn));
    }
    for (std::set<Reference>::const_iterator it = references.begin();
         it != references.end(); ++it) {
        const Record extension = readRecordRaw(std::get<0>(*it));
        const std::uint64_t expectedBase = baseIndex | (std::uint64_t(rec->sequence) << 48);
        if (!extension.valid || extension.sequence != std::get<1>(*it) ||
            extension.baseReference != expectedBase) { rec->valid = false; return; }
        bool found = false;
        for (std::size_t i = 0; i < extension.attrs.size(); ++i) {
            const Attr& a = extension.attrs[i];
            if (a.type != std::get<2>(*it) || a.id != std::get<3>(*it) ||
                a.lowestVcn != std::get<4>(*it) || a.name != std::get<5>(*it)) continue;
            if (found) { rec->valid = false; return; }
            found = true;
            const std::tuple<std::uint32_t, std::string, std::uint16_t, std::uint64_t> key(
                a.type, a.name, a.id, a.lowestVcn);
            if (seen.insert(key).second) rec->attrs.push_back(a);
        }
        if (!found) { rec->valid = false; return; }
    }
    normalizeAttributes(rec);
}

NtfsReader::Record NtfsReader::readRecord(std::uint64_t index) const {
    Record rec = readRecordRaw(index);
    mergeAttributeList(index, &rec);
    return rec;
}

const NtfsReader::Attr* NtfsReader::findAttr(const Record& rec, std::uint32_t type,
                                             const std::string& name) const {
    for (const Attr& a : rec.attrs)
        if (a.type == type && a.name == name) return &a;
    return nullptr;
}

std::vector<std::uint8_t> NtfsReader::attrBytes(const Attr& a) const {
    if (!a.nonResident) return a.residentData;
    ByteStorePtr store = runsStore(a.runs, a.realSize);
    if (!store || a.realSize > std::vector<std::uint8_t>().max_size()) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(a.realSize));
    if (!readWofNtfsExact(store, 0, bytes.data(), bytes.size())) return {};
    return bytes;
}

void NtfsReader::parseIndexNode(const std::uint8_t* node, std::size_t nodeLen,
                                std::size_t entriesOffset, std::vector<DirEntry>& out,
                                bool includeSystem) const {
    std::size_t pos = entriesOffset;
    while (pos + 0x10 <= nodeLen) {
        const std::uint8_t* e = node + pos;
        const std::uint16_t entryLen = le16(e + 0x08);
        const std::uint16_t keyLen = le16(e + 0x0A);
        const std::uint16_t flags = le16(e + 0x0C);
        if (entryLen < 0x10 || pos + entryLen > nodeLen) break;
        if (flags & 0x02) break;  // End entry: no key
        if (keyLen >= 0x42 && pos + 0x10 + keyLen <= nodeLen) {
            const std::uint64_t ref = le64(e) & 0x0000FFFFFFFFFFFFull;  // MFT index
            const std::uint8_t* key = e + 0x10;  // $FILE_NAME
            const std::uint32_t fnFlags = le32(key + 0x38);
            const std::uint64_t realSize = le64(key + 0x30);
            const std::uint8_t nameLen = key[0x40];
            const std::uint8_t nameSpace = key[0x41];
            if (nameSpace != 2 && 0x42 + nameLen * 2 <= keyLen) {  // skip DOS 8.3 names
                DirEntry de;
                if (!utf16leToUtf8(key + 0x42, nameLen, &de.name)) {
                    pos += entryLen;
                    continue;
                }
                de.mftRef = ref;
                de.isDirectory = (fnFlags & 0x10000000u) != 0;
                de.size = realSize;
                // Hide NTFS metafiles ($...) and the directory self/parent links.
                if (!de.name.empty() && (includeSystem || de.name[0] != '$') && de.name != "." && de.name != "..")
                    out.push_back(de);
            }
        }
        pos += entryLen;
    }
}

std::vector<NtfsReader::DirEntry> NtfsReader::readDirectory(const Record& rec, bool includeSystem, bool* complete) const {
    if (complete) *complete = true;
    std::vector<DirEntry> out;
    const Attr* root = findAttr(rec, kAttrIndexRoot, "$I30");
    if (!root || root->nonResident) {
        if (complete) *complete = false;
        return out;
    }
    const std::vector<std::uint8_t>& rb = root->residentData;
    if (rb.size() < 0x20) {
        if (complete) *complete = false;
        return out;
    }
    const std::uint32_t offFirst = le32(rb.data() + 0x10);
    const std::uint32_t totalEntries = le32(rb.data() + 0x14);
    if (offFirst > rb.size() - 0x10 || totalEntries > rb.size() - 0x10 ||
        offFirst > totalEntries) {
        if (complete) *complete = false;
        return out;
    }
    parseIndexNode(rb.data(), rb.size(), 0x10 + offFirst, out, includeSystem);

    const Attr* alloc = findAttr(rec, kAttrIndexAllocation, "$I30");
    if (alloc) {
        const std::uint32_t indexBlockSize = le32(rb.data() + 0x08);
        if (indexBlockSize >= bytesPerSector_ && indexBlockSize <= (1U << 20) &&
            (indexBlockSize % bytesPerSector_) == 0) {
            ByteStorePtr content = alloc->nonResident
                ? runsStore(alloc->runs, alloc->realSize)
                : std::make_shared<MemoryStore>(alloc->residentData);
            if (!content && complete) *complete = false;
            if (content) {
                for (std::int64_t off = 0;
                     off <= content->capacity() - static_cast<std::int64_t>(indexBlockSize);
                     off += indexBlockSize) {
                    std::vector<std::uint8_t> block(indexBlockSize);
                    if (!readWofNtfsExact(content, static_cast<std::uint64_t>(off), block.data(), block.size())) {
                        if (complete) *complete = false;
                        return {};
                    }
                    if (std::memcmp(block.data(), "INDX", 4) != 0 ||
                        !applyFixup(block))
                        continue;
                    if (block.size() < 0x28) continue;
                    const std::uint32_t bOffFirst = le32(block.data() + 0x18);
                    const std::uint32_t bTotal = le32(block.data() + 0x1C);
                    if (bOffFirst > block.size() - 0x18 ||
                        bTotal > block.size() - 0x18 || bOffFirst > bTotal)
                        continue;
                    parseIndexNode(block.data(), block.size(), 0x18 + bOffFirst, out, includeSystem);
                }
            }
        }
    }

    std::vector<DirEntry> unique;
    std::set<std::pair<std::uint64_t, std::string> > seen;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::pair<std::uint64_t, std::string> key(out[i].mftRef, toLower(out[i].name));
        if (seen.insert(key).second) unique.push_back(out[i]);
    }
    return unique;
}

bool NtfsReader::resolvePath(const std::string& path, std::uint64_t* mftRef) const {
    std::uint64_t current = 5;  // root directory record
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& comp : parts) {
        const std::string want = toLower(comp);
        std::uint64_t next = 0;
        bool found = false;
        bool cached = false;
        {
            std::lock_guard<std::mutex> lock(directoryMutex_);
            const auto directory = directoryCache_.find(current);
            if (directory != directoryCache_.end()) {
                cached = true;
                const auto entry = directory->second.find(want);
                if (entry != directory->second.end()) { next = entry->second; found = true; }
            }
        }
        if (!cached) {
            Record rec = readRecord(current);
            if (!rec.valid || !rec.isDirectory) return false;
            bool complete = false;
            const std::vector<DirEntry> entries = readDirectory(rec, false, &complete);
            if (!complete) return false;
            std::map<std::string, std::uint64_t> names;
            for (const DirEntry& entry : entries) names.insert(std::make_pair(toLower(entry.name), entry.mftRef));
            std::lock_guard<std::mutex> lock(directoryMutex_);
            const auto inserted = directoryCache_.insert(std::make_pair(current, std::move(names)));
            const auto entry = inserted.first->second.find(want);
            if (entry != inserted.first->second.end()) { next = entry->second; found = true; }
        }
        if (!found) return false;
        current = next;
    }
    *mftRef = current;
    return true;
}

bool NtfsReader::resolveWofPath(const std::string& path, std::uint64_t* mftRef) const {
    if (!mftRef) return false;
    std::vector<std::string> parts;
    splitPath(path, &parts);
    if (parts.empty()) return false;
    std::uint64_t current = 5;
    for (const std::string& part : parts) {
        if (part == "." || part == ".." || part.find(':') != std::string::npos) return false;
        Record rec = readRecord(current);
        if (!rec.valid || !rec.isDirectory) return false;
        bool complete = false;
        const std::vector<DirEntry> entries = readDirectory(rec, true, &complete);
        if (!complete) return false;
        const std::string wanted = toLower(part);
        bool found = false;
        for (const DirEntry& entry : entries) {
            if (toLower(entry.name) != wanted) continue;
            if (found && current != entry.mftRef) return false;
            current = entry.mftRef;
            found = true;
        }
        if (!found) return false;
    }
    *mftRef = current;
    return true;
}

bool NtfsReader::readWofStrictRecordRaw(std::uint64_t index, Record* record) const {
    if (!record || !mftStore_ || !mftRecordSize_ ||
        index > static_cast<std::uint64_t>(INT64_MAX / mftRecordSize_)) return false;
    std::vector<std::uint8_t> bytes(mftRecordSize_);
    if (!readWofNtfsExact(mftStore_, index * mftRecordSize_, bytes.data(), bytes.size())) return false;
    std::vector<std::uint8_t> fixed(bytes);
    if (fixed.size() < 0x30 || std::memcmp(fixed.data(), "FILE", 4) || !applyFixup(fixed) ||
        !(le16(fixed.data() + 0x16) & 1U)) return false;
    const std::uint32_t used = le32(fixed.data() + 0x18);
    std::size_t pos = le16(fixed.data() + 0x14);
    if (used > fixed.size() || pos < 0x30 || pos > used || used - pos < 4) return false;
    bool terminated = false;
    while (pos <= used && used - pos >= 4) {
        const std::uint32_t type = le32(fixed.data() + pos);
        if (type == 0xffffffffU) { terminated = true; break; }
        if (used - pos < 16) return false;
        const std::uint32_t length = le32(fixed.data() + pos + 4);
        if (length < 16 || length > used - pos) return false;
        const std::uint8_t* attr = fixed.data() + pos;
        if (attr[8] > 1) return false;
        const std::size_t nameSize = std::size_t(attr[9]) * 2;
        const std::size_t nameOffset = le16(attr + 10);
        if (nameSize && (nameOffset < 16 || nameOffset > length || nameSize > length - nameOffset)) return false;
        std::string name;
        if (nameSize && !utf16leToUtf8(attr + nameOffset, attr[9], &name)) return false;
        if (!attr[8]) {
            if (length < 24) return false;
            const std::size_t dataSize = le32(attr + 16), dataOffset = le16(attr + 20);
            if (dataOffset < 24 || dataOffset > length || dataSize > length - dataOffset) return false;
        } else {
            if (length < 64 || le16(attr + 32) < 64 || le16(attr + 32) >= length) return false;
        }
        pos += length;
    }
    if (!terminated) return false;
    Record result = parseRecord(bytes);
    if (!result.valid) return false;
    *record = std::move(result);
    return true;
}

bool NtfsReader::readWofStrictRecord(std::uint64_t index, Record* record) const {
    Record base;
    if (!readWofStrictRecordRaw(index, &base) || base.baseReference) return false;
    typedef std::tuple<std::uint64_t, std::uint32_t, std::uint16_t,
                       std::uint64_t, std::string> Reference;
    std::set<Reference> references;
    std::map<std::uint64_t, Record> extensions;
    for (const Attr& list : base.attrs) {
        if (list.type != kAttrAttributeList) continue;
        ByteStorePtr store;
        if (list.nonResident) {
            if (!list.strictRunsValid || list.initializedSize < list.realSize || (list.flags & 0xc001U)) return false;
            for (const Run& run : list.strictRuns) if (run.lcn < 0) return false;
            store = runsStore(list.strictRuns, list.realSize);
        } else store = std::make_shared<MemoryStore>(list.residentData);
        if (!store || list.realSize > std::vector<std::uint8_t>().max_size()) return false;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(list.realSize));
        if (!readWofNtfsExact(store, 0, data.data(), data.size())) return false;
        std::size_t pos = 0;
        while (pos < data.size()) {
            if (data.size() - pos < 4) return false;
            const std::uint32_t type = le32(data.data() + pos);
            if (type == 0xffffffffU) break;
            if (data.size() - pos < 26) return false;
            const std::uint16_t size = le16(data.data() + pos + 4);
            if (size < 26 || size > data.size() - pos) return false;
            const std::uint8_t nameLength = data[pos + 6], nameOffset = data[pos + 7];
            std::string name;
            if (nameLength && (nameOffset < 26 || std::size_t(nameOffset) + std::size_t(nameLength) * 2 > size ||
                !utf16leToUtf8(data.data() + pos + nameOffset, nameLength, &name))) return false;
            const std::uint64_t vcn = le64(data.data() + pos + 8);
            const std::uint64_t ref = le64(data.data() + pos + 16);
            const std::uint64_t number = ref & 0x0000ffffffffffffULL;
            const std::uint16_t attrId = le16(data.data() + pos + 24);
            const Reference key(number, type, attrId, vcn, name);
            if (!references.insert(key).second) return false;
            const Record* owner = &base;
            if (number != index) {
                auto found = extensions.find(number);
                if (found == extensions.end()) {
                    Record extension;
                    if (!readWofStrictRecordRaw(number, &extension)) return false;
                    found = extensions.insert(std::make_pair(number, std::move(extension))).first;
                }
                owner = &found->second;
                if (owner->sequence != static_cast<std::uint16_t>(ref >> 48) ||
                    owner->baseReference != (index | (std::uint64_t(base.sequence) << 48))) return false;
                for (const Attr& attr : owner->attrs)
                    if (attr.type == kAttrAttributeList) return false;
            } else if (static_cast<std::uint16_t>(ref >> 48) != base.sequence) return false;
            unsigned int matches = 0;
            for (const Attr& attr : owner->attrs)
                if (attr.type == type && attr.id == attrId && attr.lowestVcn == vcn && attr.name == name) ++matches;
            if (matches != 1) return false;
            pos += size;
        }
    }
    for (const auto& extension : extensions) {
        for (const Attr& attr : extension.second.attrs) {
            const Reference key(extension.first, attr.type, attr.id, attr.lowestVcn, attr.name);
            if (!references.count(key)) return false;
            base.attrs.push_back(attr);
        }
    }
    normalizeAttributes(&base);
    *record = std::move(base);
    return true;
}

NtfsReader::WofLookup NtfsReader::resolveWofPathStrict(const std::string& path,
                                                       std::uint64_t* mftRef) const {
    if (!mftRef || path.size() > static_cast<std::size_t>(INT_MAX)) return WofLookup::Error;
    try {
        Record upcase;
        if (!readWofStrictRecord(10, &upcase) || upcase.isDirectory) return WofLookup::Error;
        const auto uniqueAttr = [](const Record& record, std::uint32_t type, const std::string& name,
                                   const Attr** result) -> bool {
            *result = nullptr;
            for (const Attr& attr : record.attrs) {
                if (attr.type != type || attr.name != name) continue;
                if (*result) return false;
                *result = &attr;
            }
            return true;
        };
        const Attr* table = nullptr;
        if (!uniqueAttr(upcase, kAttrData, std::string(), &table)) return WofLookup::Error;
        if (!table || table->realSize != 131072 || (table->flags & 0xc001U)) return WofLookup::Error;
        ByteStorePtr tableStore;
        if (table->nonResident) {
            if (!table->strictRunsValid || table->initializedSize < table->realSize) return WofLookup::Error;
            for (const Run& run : table->strictRuns) if (run.lcn < 0) return WofLookup::Error;
            tableStore = runsStore(table->strictRuns, table->realSize);
        } else {
            tableStore = std::make_shared<MemoryStore>(table->residentData);
        }
        std::vector<std::uint8_t> upcaseBytes(131072);
        if (!readWofNtfsExact(tableStore, 0, upcaseBytes.data(), upcaseBytes.size())) return WofLookup::Error;
        std::vector<std::string> parts;
        splitPath(path, &parts);
        if (parts.empty()) return WofLookup::Error;
        std::uint64_t current = 5;
        for (const std::string& part : parts) {
            if (part == "." || part == ".." || part.find(':') != std::string::npos ||
                part.size() > static_cast<std::size_t>(INT_MAX)) return WofLookup::Error;
            const QByteArray utf8(part.data(), static_cast<int>(part.size()));
            const QString target = QString::fromUtf8(utf8);
            if (target.toUtf8() != utf8 || target.isEmpty()) return WofLookup::Error;
            Record dir;
            if (!readWofStrictRecord(current, &dir) || !dir.isDirectory) return WofLookup::Error;
            const Attr* root = nullptr;
            if (!uniqueAttr(dir, kAttrIndexRoot, "$I30", &root)) return WofLookup::Error;
            if (!root || root->nonResident) return WofLookup::Error;
            const Attr* alloc = nullptr;
            const Attr* bitmap = nullptr;
            if (!uniqueAttr(dir, kAttrIndexAllocation, "$I30", &alloc) ||
                !uniqueAttr(dir, kAttrBitmap, "$I30", &bitmap)) return WofLookup::Error;
            const std::vector<std::uint8_t>& data = root->residentData;
            if (data.size() < 32 || le32(data.data()) != 0x30U || le32(data.data() + 4) != 1U ||
                data[28] > 1) return WofLookup::Error;
            const bool large = data[28] == 1;
            if (large != bool(alloc) || large != bool(bitmap)) return WofLookup::Error;
            bool found = false;
            std::uint64_t next = 0;
            std::uint16_t sequence = 0;
            std::map<std::uint64_t, std::vector<std::uint64_t> > edges;
            const std::uint64_t rootNode = UINT64_MAX;
            const auto parseNode = [&](const std::vector<std::uint8_t>& node, std::size_t base,
                                       std::uint64_t nodeId) -> bool {
              if (base > node.size() || node.size() - base < 16) return false;
              const std::uint32_t first = le32(node.data() + base), total = le32(node.data() + base + 4);
              if (first < 16 || first > total || total > node.size() - base || node[base + 12] > 1) return false;
              const bool internal = node[base + 12] == 1;
              std::size_t pos = base + first, end = base + total;
              bool terminated = false;
              std::vector<std::uint64_t>& children = edges[nodeId];
              while (pos <= end && end - pos >= 16) {
                const std::uint8_t* entry = node.data() + pos;
                const std::uint16_t length = le16(entry + 8), keySize = le16(entry + 10), flags = le16(entry + 12);
                if (length < 16 || length > end - pos || (flags & ~3U) || bool(flags & 1U) != internal ||
                    length < 16U + (internal ? 8U : 0U) ||
                    keySize > length - 16U - (internal ? 8U : 0U)) return false;
                if (internal) children.push_back(le64(entry + length - 8));
                if (flags & 2U) { terminated = true; break; }
                if (keySize < 66) return false;
                const std::uint8_t* key = entry + 16;
                const std::size_t units = key[64];
                if (units > (keySize - 66) / 2) return false;
                if (units == static_cast<std::size_t>(target.size())) {
                    bool equal = true;
                    for (std::size_t i = 0; i < units; ++i) {
                        const std::uint16_t a = le16(key + 66 + i * 2);
                        const std::uint16_t b = target.at(static_cast<int>(i)).unicode();
                        if (le16(upcaseBytes.data() + std::size_t(a) * 2) !=
                            le16(upcaseBytes.data() + std::size_t(b) * 2)) { equal = false; break; }
                    }
                    if (equal) {
                        const std::uint64_t fullRef = le64(entry);
                        const std::uint64_t ref = fullRef & 0x0000ffffffffffffULL;
                        const std::uint16_t seq = static_cast<std::uint16_t>(fullRef >> 48);
                        if (found && (ref != next || seq != sequence)) return false;
                        next = ref;
                        sequence = seq;
                        found = true;
                    }
                }
                pos += length;
              }
              return terminated;
            };
            if (!parseNode(data, 16, rootNode)) return WofLookup::Error;
            if (large) {
                const std::uint32_t blockSize = le32(data.data() + 8);
                if (!blockSize || blockSize < bytesPerSector_ || blockSize % bytesPerSector_ ||
                    blockSize > static_cast<std::uint32_t>(INT_MAX)) return WofLookup::Error;
                const auto strictStore = [&](const Attr* attr) -> ByteStorePtr {
                    if (!attr || (attr->flags & 0xc001U)) return nullptr;
                    if (!attr->nonResident) return std::make_shared<MemoryStore>(attr->residentData);
                    if (!attr->strictRunsValid || attr->initializedSize < attr->realSize) return nullptr;
                    return runsStore(attr->strictRuns, attr->realSize);
                };
                ByteStorePtr blocks = strictStore(alloc), bits = strictStore(bitmap);
                if (!blocks || !bits || blocks->capacity() < 0 || bits->capacity() < 0 ||
                    blocks->capacity() % blockSize) return WofLookup::Error;
                const std::uint64_t count = static_cast<std::uint64_t>(blocks->capacity()) / blockSize;
                if (count / 8 + (count % 8 != 0) > static_cast<std::uint64_t>(bits->capacity())) return WofLookup::Error;
                const std::uint32_t unit = bytesPerCluster_ <= blockSize ? bytesPerCluster_ : bytesPerSector_;
                if (!unit || blockSize % unit) return WofLookup::Error;
                std::vector<std::uint8_t> map(4096), block(blockSize);
                for (std::uint64_t byte = 0; byte < static_cast<std::uint64_t>(bits->capacity());) {
                    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(map.size(),
                        static_cast<std::uint64_t>(bits->capacity()) - byte));
                    if (!readWofNtfsExact(bits, byte, map.data(), n)) return WofLookup::Error;
                    for (std::size_t i = 0; i < n; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
                        if (!(map[i] & (1U << bit))) continue;
                        if (byte + i > UINT64_MAX / 8) return WofLookup::Error;
                        const std::uint64_t index = (byte + i) * 8 + bit;
                        if (index >= count || index > UINT64_MAX / blockSize) return WofLookup::Error;
                        const std::uint64_t off = index * blockSize, vcn = off / unit;
                        if (!readWofNtfsExact(blocks, off, block.data(), block.size()) ||
                            std::memcmp(block.data(), "INDX", 4) || !applyFixup(block) ||
                            le64(block.data() + 16) != vcn || !parseNode(block, 24, vcn)) return WofLookup::Error;
                    }
                    byte += n;
                }
                std::set<std::uint64_t> seen;
                std::vector<std::uint64_t> pending(1, rootNode);
                while (!pending.empty()) {
                    const std::uint64_t node = pending.back(); pending.pop_back();
                    const auto entry = edges.find(node);
                    if (entry == edges.end() || !seen.insert(node).second) return WofLookup::Error;
                    pending.insert(pending.end(), entry->second.begin(), entry->second.end());
                }
                if (seen.size() != edges.size()) return WofLookup::Error;
            }
            if (!found) return WofLookup::NotFound;
            Record selected;
            if (!readWofStrictRecord(next, &selected) || selected.sequence != sequence) return WofLookup::Error;
            current = next;
        }
        *mftRef = current;
        return WofLookup::Found;
    } catch (...) { return WofLookup::Error; }
}

ByteStorePtr NtfsReader::openWofBacking(const std::string& path, WofLookup* status) const {
    if (status) *status = WofLookup::Error;
    std::uint64_t ref = 0;
    if (status) {
        const WofLookup result = resolveWofPathStrict(path, &ref);
        if (result != WofLookup::Found) { *status = result; return nullptr; }
    } else if (!resolveWofPath(path, &ref)) return nullptr;
    Record rec;
    if (status) {
        if (!readWofStrictRecord(ref, &rec)) return nullptr;
    } else rec = readRecord(ref);
    if (!rec.valid || rec.isDirectory || findAttr(rec, kAttrReparsePoint, std::string())) return nullptr;
    const Attr* data = findAttr(rec, kAttrData, std::string());
    if (status && data) {
        unsigned count = 0;
        for (const Attr& attr : rec.attrs)
            if (attr.type == kAttrData && attr.name.empty()) ++count;
        if (count != 1) return nullptr;
    }
    if (!data || (data->flags & 0xc001)) return nullptr;
    if (!data->nonResident) {
        ByteStorePtr content = std::make_shared<MemoryStore>(data->residentData);
        if (status) *status = WofLookup::Found;
        return content;
    }
    if (!data->strictRunsValid || data->realSize > static_cast<std::uint64_t>(INT64_MAX) ||
        data->initializedSize < data->realSize || !disc_ || disc_->capacity() < 0) return nullptr;
    std::vector<WofPhysicalRun> runs;
    std::uint64_t logical = 0;
    for (const Run& run : data->strictRuns) {
        if (run.lcn < 0 || run.length <= 0 ||
            static_cast<std::uint64_t>(run.lcn) > UINT64_MAX / bytesPerCluster_ ||
            static_cast<std::uint64_t>(run.length) > UINT64_MAX / bytesPerCluster_) return nullptr;
        const std::uint64_t physical = static_cast<std::uint64_t>(run.lcn) * bytesPerCluster_;
        const std::uint64_t length = static_cast<std::uint64_t>(run.length) * bytesPerCluster_;
        if (length > UINT64_MAX - logical || physical > static_cast<std::uint64_t>(disc_->capacity()) ||
            length > static_cast<std::uint64_t>(disc_->capacity()) - physical) return nullptr;
        WofPhysicalRun segment = {logical, length, physical};
        runs.push_back(segment);
        logical += length;
    }
    if (logical < data->realSize) return nullptr;
    ByteStorePtr content = std::make_shared<WofNtfsStore>(disc_, std::move(runs), static_cast<std::int64_t>(data->realSize));
    if (status) *status = WofLookup::Found;
    return content;
}

ByteStorePtr NtfsReader::openWofWim(const Record& rec, const Attr& data) const {
    const Attr* reparse = findAttr(rec, kAttrReparsePoint, std::string());
    if (!reparse || reparse->nonResident || reparse->residentData.size() < 96) return nullptr;
    const std::uint8_t* p = reparse->residentData.data();
    if (le32(p) != kWofReparseTag || le16(p + 4) != 88 || le32(p + 8) != 1 ||
        le32(p + 12) != 1 || le32(p + 16) != 2 || le32(p + 20) != 0) return nullptr;
    const std::uint64_t id = le64(p + 24);
    WofWimReference reference;
    std::copy(p + 32, p + 52, reference.contentHash.begin());
    std::copy(p + 52, p + 72, reference.tableHash.begin());
    reference.size = le64(p + 72);
    reference.storedSize = le64(p + 80);
    reference.offset = le64(p + 88);
    if (reference.size != data.realSize) return nullptr;

    WofLookup lookup = WofLookup::Error;
    ByteStorePtr catalog = openWofBacking("System Volume Information/WimOverlay.dat", &lookup);
    if (lookup != WofLookup::Found) return nullptr;
    std::uint8_t header[24];
    if (!readWofNtfsExact(catalog, 0, header, sizeof(header)) ||
        le32(header) != 0x66436f57u || le32(header + 4) != 1 || le32(header + 8) != 40) return nullptr;
    const std::uint32_t count = le32(header + 12);
    const std::uint64_t end = 24 + std::uint64_t(count) * 40;
    if (end > static_cast<std::uint64_t>(catalog->capacity())) return nullptr;
    std::uint32_t location = 0, locationSize = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t item[40];
        if (!readWofNtfsExact(catalog, 24 + std::uint64_t(i) * 40, item, sizeof(item))) return nullptr;
        if (le64(item) != id) continue;
        if (found) return nullptr;
        found = true;
        location = le32(item + 8);
        locationSize = le32(item + 12);
        if (le32(item + 16) > 1) return nullptr;
        reference.imageIndex = le32(item + 20);
        std::copy(item + 24, item + 40, reference.guid.begin());
    }
    if (!found || !reference.imageIndex || location < end || locationSize < 106 ||
        location > static_cast<std::uint64_t>(catalog->capacity()) ||
        locationSize > static_cast<std::uint64_t>(catalog->capacity()) - location ||
        ((locationSize - 104) & 1)) return nullptr;
    std::uint8_t locationData[104];
    if (!readWofNtfsExact(catalog, location, locationData, sizeof(locationData)) ||
        le32(locationData + 8) != locationSize ||
        le32(locationData + 24) != locationSize - 20 ||
        le32(locationData + 16) != 5 || le32(locationData + 20) != 1 ||
        le32(locationData + 28) != 5 || le32(locationData + 32) != 6 ||
        le32(locationData + 40) != 0x48) return nullptr;
    std::string path;
    std::uint8_t name[4098];
    std::uint64_t remaining = locationSize - 104, at = std::uint64_t(location) + 104;
    std::size_t carry = 0;
    bool firstNameUnit = true;
    while (remaining) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 4096));
        if (!readWofNtfsExact(catalog, at, name + carry, n)) return nullptr;
        at += n; remaining -= n;
        std::size_t bytes = carry + n;
        if (firstNameUnit) {
            if (bytes < 2 || le16(name) != '\\') return nullptr;
            firstNameUnit = false;
        }
        if (!remaining) {
            if (bytes < 2 || le16(name + bytes - 2) != 0) return nullptr;
            bytes -= 2;
        }
        carry = 0;
        if (remaining && bytes >= 2 && le16(name + bytes - 2) >= 0xd800 &&
            le16(name + bytes - 2) <= 0xdbff) { bytes -= 2; carry = 2; }
        for (std::size_t i = 0; i < bytes; i += 2) if (!le16(name + i)) return nullptr;
        std::string part;
        if (!utf16leToUtf8(name, static_cast<int>(bytes / 2), &part)) return nullptr;
        if (part.size() > path.max_size() - path.size()) return nullptr;
        path += part;
        if (carry) { name[0] = name[bytes]; name[1] = name[bytes + 1]; }
    }
    PartitionIdentity current, target;
    std::shared_ptr<const IPartitionResolver> resolver;
    const bool knownPartition = partitionContext(&current, &resolver);
    if (!resolver) {
        ByteStorePtr wim = openWofBacking(path, &lookup);
        if (lookup == WofLookup::Found)
            return wim ? WimReader::openWofResource(wim, reference, true) : nullptr;
        if (lookup != WofLookup::NotFound) return nullptr;
        const std::uint32_t tableType = le32(locationData + 68);
        if (tableType == 0) {
            target.scheme = PartitionScheme::Gpt;
            std::copy(locationData + 48, locationData + 64, target.partitionGuid.begin());
            std::copy(locationData + 72, locationData + 88, target.diskGuid.begin());
        } else if (tableType == 1) {
            target.scheme = PartitionScheme::Mbr;
            target.startOffset = le64(locationData + 48);
            target.mbrDiskSignature = le32(locationData + 72);
            if (le64(locationData + 56) || le32(locationData + 76) ||
                le32(locationData + 80) || le32(locationData + 84)) return nullptr;
        } else return nullptr;
        const auto external = std::dynamic_pointer_cast<const IWofResourceResolver>(disc_);
        if (external) return external->resolveExternalWofResource(path, reference);
        const auto tibx = std::dynamic_pointer_cast<TibxVolumeStore>(disc_);
        if (!tibx) return nullptr;
        ByteStorePtr sibling;
        if (tibx->openWofSibling(target, &sibling) != PartitionResolveResult::Found || !sibling)
            return nullptr;
        NtfsReader backing(sibling);
        if (!backing.valid()) return nullptr;
        wim = backing.openWofBacking(path, &lookup);
        if (lookup != WofLookup::Found || !wim) return nullptr;
        return WimReader::openWofResource(wim, reference, true);
    }
    if (!knownPartition) return nullptr;
    const std::uint32_t tableType = le32(locationData + 68);
    if (tableType == 0) {
        target.scheme = PartitionScheme::Gpt;
        std::copy(locationData + 48, locationData + 64,
                  target.partitionGuid.begin());
        std::copy(locationData + 72, locationData + 88,
                  target.diskGuid.begin());
    } else if (tableType == 1) {
        target.scheme = PartitionScheme::Mbr;
        target.startOffset = le64(locationData + 48);
        target.mbrDiskSignature = le32(locationData + 72);
        if (le64(locationData + 56) || le32(locationData + 76) ||
            le32(locationData + 80) || le32(locationData + 84)) return nullptr;
    } else {
        return nullptr;
    }
    ByteStorePtr volume;
    if (resolver->open(target, &volume) != PartitionResolveResult::Found || !volume)
        return nullptr;
    NtfsReader backing(volume);
    if (!backing.valid()) return nullptr;
    ByteStorePtr wim = backing.openWofBacking(path, &lookup);
    if (lookup != WofLookup::Found) return nullptr;
    return wim ? WimReader::openWofResource(wim, reference) : nullptr;
}

void NtfsReader::parse() {
    std::vector<std::uint8_t> bpb(512);
    if (!readWofNtfsExact(disc_, 0, bpb.data(), bpb.size()) || std::memcmp(bpb.data() + 3, "NTFS    ", 8) != 0) {
        error_ = "Not an NTFS volume";
        return;
    }
    const std::uint16_t bytesPerSector = le16(bpb.data() + 0x0B);
    const std::uint8_t secPerClus = bpb[0x0D];
    if (bytesPerSector < 512 || bytesPerSector > 4096 ||
        (bytesPerSector & (bytesPerSector - 1)) != 0 || secPerClus == 0 ||
        (secPerClus & (secPerClus - 1)) != 0) {
        error_ = "Invalid NTFS BPB";
        return;
    }
    bytesPerSector_ = bytesPerSector;
    bytesPerCluster_ = static_cast<std::uint32_t>(bytesPerSector) * secPerClus;
    if (bytesPerCluster_ == 0 || bytesPerCluster_ > (2U << 20)) {
        error_ = "Invalid NTFS cluster size";
        return;
    }
    const std::uint64_t totalSectors = le64(bpb.data() + 0x28);
    if (totalSectors == 0 || totalSectors >
        static_cast<std::uint64_t>(disc_->capacity()) / bytesPerSector_) {
        error_ = "NTFS volume exceeds source bounds";
        return;
    }
    const std::uint64_t mftLcn = le64(bpb.data() + 0x30);
    const std::int8_t rawRec = static_cast<std::int8_t>(bpb[0x40]);
    if (mftLcn > std::uint64_t(INT64_MAX) / bytesPerCluster_ || rawRec == 0 || rawRec < -20) {
        error_ = "Invalid MFT location or record size";
        return;
    }
    mftRecordSize_ = rawRec >= 0 ? static_cast<std::uint32_t>(rawRec) * bytesPerCluster_
                                 : (1u << (-rawRec));
    if (mftRecordSize_ < bytesPerSector_ || mftRecordSize_ > (1u << 20) ||
        (mftRecordSize_ % bytesPerSector_) != 0) {
        error_ = "Invalid MFT record size";
        return;
    }

    // Bootstrap: read $MFT (record 0) directly, then use its $DATA runlist as the
    // positioned store for every other record.
    std::vector<std::uint8_t> rec0(mftRecordSize_);
    if (!readWofNtfsExact(disc_, mftLcn * bytesPerCluster_,
                          rec0.data(), rec0.size())) { error_ = "Truncated MFT"; return; }
    Record mft = parseRecord(rec0);
    const Attr* mftData = findAttr(mft, kAttrData, std::string());
    if (!mftData || !mftData->nonResident || !mftData->strictRunsValid || mftData->runs.empty()) {
        error_ = "Cannot locate $MFT data runs";
        return;
    }
    mftStore_ = runsStore(mftData->runs, mftData->realSize);
    valid_ = bool(mftStore_);
}

std::vector<DiscEntry> NtfsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    listChecked(dirPath, &out);
    return out;
}

bool NtfsReader::listChecked(const std::string& dirPath, std::vector<DiscEntry>* entries) const {
    if (!entries || !valid_) return false;
    std::vector<DiscEntry> out;
    std::uint64_t ref = 0;
    if (!resolvePath(dirPath, &ref)) return false;
    Record rec = readRecord(ref);
    if (!rec.valid || !rec.isDirectory) return false;
    bool complete = false;
    const std::vector<DirEntry> directory = readDirectory(rec, false, &complete);
    if (!complete) return false;
    for (const DirEntry& e : directory) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.isDirectory ? 0 : static_cast<std::int64_t>(e.size);
        out.push_back(d);
    }
    entries->swap(out);
    return true;
}

ByteStorePtr NtfsReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    std::uint64_t ref = 0;
    if (!resolvePath(path, &ref)) return nullptr;
    Record rec = readRecord(ref);
    if (!rec.valid || rec.isDirectory) return nullptr;
    const Attr* data = findAttr(rec, kAttrData, std::string());
    if (!data) return nullptr;
    if (data->flags & 0x4001) {
        std::vector<LzntRun> runs;
        std::uint64_t start = 0;
        bool valid = data->nonResident && !(data->flags & 0x4000) && data->compressedRunsValid;
        for (std::size_t i = 0; i < data->runs.size(); ++i) {
            const Run& run = data->runs[i];
            if (run.length <= 0 || static_cast<std::uint64_t>(run.length) > UINT64_MAX - start) {
                valid = false; break;
            }
            LzntRun entry = { start, static_cast<std::uint64_t>(run.length), run.lcn };
            runs.push_back(entry);
            start += entry.length;
        }
        return std::make_shared<LzntFileStore>(disc_, std::move(runs), data->realSize,
                                                data->initializedSize, bytesPerCluster_,
                                                data->compressionUnit, valid);
    }
    const Attr* reparse = findAttr(rec, kAttrReparsePoint, std::string());
    if (reparse && reparse->residentData.size() >= 4 &&
        le32(reparse->residentData.data()) == kWofReparseTag) {
        if (reparse->residentData.size() < 24) return nullptr;
        const std::uint8_t* info = reparse->residentData.data();
        if (le32(info + 8) == 1 && le32(info + 12) == 1)
            return openWofWim(rec, *data);
        if (le16(info + 4) != 16 || le32(info + 8) != 1 ||
            le32(info + 12) != kWofFileProvider || le32(info + 16) != 1)
            return nullptr;
        const Attr* compressed = findAttr(rec, kAttrData, "WofCompressedData");
        if (!compressed || !compressed->nonResident) return nullptr;
        ByteStorePtr stream = runsStore(compressed->runs, compressed->realSize);
        ByteStorePtr wof = std::make_shared<WofFileStore>(
            std::move(stream), data->realSize, le32(info + 20));
        return wof->capacity() == static_cast<std::int64_t>(data->realSize) ? wof : nullptr;
    }
    if (!data->nonResident)
        return std::make_shared<MemoryStore>(data->residentData);
    return runsStore(data->runs, data->realSize);
}

}  // namespace fs
}  // namespace peare
