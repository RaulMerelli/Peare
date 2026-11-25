#include "TibLegacyReader.h"
#include "TibHeader.h"
#include "TibFileReader.h"
#include "TibRandomAccessIndex.h"
#include "TibRandomAccessVolume.h"
#include "NtfsReader.h"
#include "WimReader.h"
#include "../../common/compression/DeflateDecoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <set>

namespace peare {
namespace fs {
namespace {

const std::uint64_t kBlockSize = 256 * 1024;
const std::uint64_t kHeaderSize = 12;

bool normalizeWofPath(std::string path, std::string* normalized) {
    if (!normalized || path.empty()) return false;
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.empty()) return false;
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part == ".." || part == ".") return false;
        if (!part.empty()) {
            if (!result.empty()) result.push_back('/');
            result += part;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (result.empty()) return false;
    *normalized = result;
    return true;
}

class TibLegacyPartitionStore final : public IByteStore, public IWofResourceResolver {
public:
    TibLegacyPartitionStore(ByteStorePtr source, std::shared_ptr<const std::vector<ByteStorePtr> > siblings)
        : source_(std::move(source)), siblings_(std::move(siblings)) {}

    std::int64_t capacity() const override { return source_ ? source_->capacity() : -1; }
    bool validate() const override { return source_ && source_->validate(); }
    bool cheapRandomAccess() const override { return source_ && source_->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return source_ ? source_->read(pos, dst, count) : -1;
    }

    ByteStorePtr resolveExternalWofResource(const std::string& path,
                                             const WofWimReference& reference) const override {
        std::string relativePath;
        if (!normalizeWofPath(path, &relativePath) || !siblings_) return nullptr;
        ByteStorePtr match;
        for (const ByteStorePtr& sibling : *siblings_) {
            if (!sibling || sibling.get() == source_.get() || !sibling->validate()) continue;
            NtfsReader volume(sibling);
            if (!volume.valid()) continue;
            ByteStorePtr wim = volume.openFile(relativePath);
            if (!wim || !wim->validate()) continue;
            ByteStorePtr resource = WimReader::openWofResource(wim, reference, true);
            if (!resource) continue;
            if (match) return nullptr;
            match = resource;
        }
        return match;
    }

private:
    ByteStorePtr source_;
    std::shared_ptr<const std::vector<ByteStorePtr> > siblings_;
};

std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p + 2)) << 16);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool exact(const ByteStorePtr& source, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    if (!source || source->capacity() < 0 || offset > std::uint64_t(source->capacity()) ||
        size > std::uint64_t(source->capacity()) - offset) return false;
    std::size_t done = 0;
    while (done < size) {
        const int request = static_cast<int>(std::min<std::size_t>(size - done, 1024 * 1024));
        const int n = source->read(static_cast<std::int64_t>(offset + done), data + done, request);
        if (n <= 0 || n > request) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::uint32_t checksum(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    while (size) {
        const std::size_t n = std::min<std::size_t>(size, 5552);
        for (std::size_t i = 0; i < n; ++i) { a += bytes[i]; b += a; }
        a %= 65521; b %= 65521;
        bytes += n; size -= n;
    }
    return (b << 16) | a;
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

struct Block {
    std::uint64_t offset;
    std::uint32_t packed;
    std::vector<std::uint8_t> bitmap;
};

class TibLegacyVolume final : public IByteStore {
public:
    TibLegacyVolume(ByteStorePtr source, std::uint64_t length, std::uint32_t cluster,
                    std::vector<Block> blocks)
        : source_(std::move(source)), length_(length), cluster_(cluster),
          blocks_(std::move(blocks)), cached_(std::numeric_limits<std::size_t>::max()), failed_(false) {}

    std::int64_t capacity() const override { return static_cast<std::int64_t>(length_); }
    bool validate() const override { return source_ && !failed_.load() && source_->validate(); }

    int read(std::int64_t position, std::uint8_t* out, int count) const override {
        if (!out || position < 0 || count <= 0 || std::uint64_t(position) >= length_ || !validate()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(count),
                                    length_ - static_cast<std::uint64_t>(position)), 1024 * 1024));
        try {
            std::vector<std::uint8_t> staged(amount);
            std::size_t done = 0;
            while (done < amount) {
                const std::uint64_t logical = static_cast<std::uint64_t>(position) + done;
                const std::size_t index = static_cast<std::size_t>(logical / kBlockSize);
                if (index >= blocks_.size() || !load(index)) { failed_.store(true); return -1; }
                const std::size_t within = static_cast<std::size_t>(logical % kBlockSize);
                const std::size_t n = std::min<std::size_t>(amount - done, cache_.size() - within);
                std::memcpy(staged.data() + done, cache_.data() + within, n);
                done += n;
            }
            if (!validate()) return -1;
            std::memcpy(out, staged.data(), amount);
            return static_cast<int>(amount);
        } catch (const std::exception&) { return -1; }
    }

private:
    bool load(std::size_t index) const {
        if (cached_ == index) return true;
        const Block& block = blocks_[index];
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(block.packed) + 4);
        if (!exact(source_, block.offset + block.bitmap.size(), packed.data(), packed.size()) ||
            checksum(packed.data(), block.packed) != le32(packed.data() + block.packed)) return false;
        std::vector<std::uint8_t> clear(static_cast<std::size_t>(kBlockSize), 0);
        std::size_t sourcePos = 0;
        const std::size_t clusters = static_cast<std::size_t>(kBlockSize / cluster_);
        for (std::size_t i = 0; i < clusters; ++i) {
            if ((block.bitmap[i / 8] & (1U << (i % 8))) == 0) continue;
            if (sourcePos > block.packed || cluster_ > block.packed - sourcePos) return false;
            std::memcpy(clear.data() + i * cluster_, packed.data() + sourcePos, cluster_);
            sourcePos += cluster_;
        }
        if (sourcePos != block.packed) return false;
        cache_.swap(clear);
        cached_ = index;
        return true;
    }

    ByteStorePtr source_;
    std::uint64_t length_;
    std::uint32_t cluster_;
    std::vector<Block> blocks_;
    mutable std::mutex mutex_;
    mutable std::size_t cached_;
    mutable std::vector<std::uint8_t> cache_;
    mutable std::atomic<bool> failed_;
};

struct SectorBlock {
    std::uint64_t offset;
    std::uint32_t size;
    bool external;
    std::uint64_t logical;
    std::array<std::uint8_t, 16> hash;
};

struct SectorGeometry {
    std::uint32_t sector;
    std::uint32_t cluster;
    std::uint32_t clustersPerBlock;
    std::uint64_t blockSize;
    std::uint64_t prefix;
    std::uint8_t dedupMode;
    bool rawCodec;
    bool extended;
};

class StoreInflateInput final : public compression::InflateInput {
public:
    StoreInflateInput(ByteStorePtr source, std::uint64_t offset)
        : source_(std::move(source)), offset_(offset), position_(0), available_(0), consumed_(0) {}
    bool readByte(std::uint8_t* value) override {
        if (!value || consumed_ == std::numeric_limits<std::size_t>::max()) return false;
        if (position_ == available_) {
            if (!source_ || source_->capacity() < 0 || offset_ >= std::uint64_t(source_->capacity())) return false;
            const int request = static_cast<int>(std::min<std::uint64_t>(
                buffer_.size(), std::uint64_t(source_->capacity()) - offset_));
            const int got = source_->read(static_cast<std::int64_t>(offset_), buffer_.data(), request);
            if (got <= 0 || got > request) return false;
            available_ = static_cast<std::size_t>(got);
            position_ = 0;
            offset_ += available_;
        }
        *value = buffer_[position_++];
        ++consumed_;
        return true;
    }
    std::size_t consumed() const override { return consumed_; }
    bool rewind(std::size_t count) override {
        if (count > consumed_) return false;
        const std::uint64_t next = offset_ - available_ + position_;
        if (count > next) return false;
        offset_ = next - count;
        position_ = available_ = 0;
        consumed_ -= count;
        return true;
    }
private:
    ByteStorePtr source_;
    std::uint64_t offset_;
    std::array<std::uint8_t, 65536> buffer_;
    std::size_t position_;
    std::size_t available_;
    std::size_t consumed_;
};

class TibSectorVolume final : public IByteStore {
public:
    TibSectorVolume(ByteStorePtr source, std::vector<SectorBlock> blocks, std::uint64_t length,
                    const SectorGeometry& geometry,
                    std::uint64_t readableLength = std::numeric_limits<std::uint64_t>::max())
        : source_(std::move(source)), blocks_(std::move(blocks)), length_(length), geometry_(geometry),
          readableLength_(std::min(length, readableLength)), cacheBytes_(0), failed_(false) {}
    std::int64_t capacity() const override { return static_cast<std::int64_t>(length_); }
    bool validate() const override { return source_ && !failed_.load() && source_->validate(); }
    void setDedupProvider(const std::shared_ptr<TibDedupProvider>& provider) { provider_ = provider; }
    bool needsDedupProvider() const { return geometry_.dedupMode != 0; }
    int read(std::int64_t position, std::uint8_t* out, int count) const override {
        if (!out || position < 0 || count <= 0 || std::uint64_t(position) >= length_ || !validate()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min<std::uint64_t>(count, length_ - std::uint64_t(position)), 1024 * 1024));
        if (std::uint64_t(position) >= readableLength_ ||
            amount > readableLength_ - std::uint64_t(position)) return -1;
        try {
            std::vector<std::uint8_t> staged(amount);
            std::size_t done = 0;
            while (done < amount) {
                const std::uint64_t logical = std::uint64_t(position) + done + geometry_.prefix;
                std::size_t index = 0;
                std::size_t within = 0;
                std::uint64_t untilNext = std::numeric_limits<std::uint64_t>::max();
                if (geometry_.extended) {
                    const auto next = std::upper_bound(blocks_.begin(), blocks_.end(), logical,
                        [](std::uint64_t pos, const SectorBlock& block) { return pos < block.logical; });
                    if (next == blocks_.begin() || logical - (next - 1)->logical >= geometry_.blockSize) {
                        const std::uint64_t gap = next == blocks_.end()
                            ? std::uint64_t(amount - done) : next->logical - logical;
                        done += static_cast<std::size_t>(std::min<std::uint64_t>(amount - done, gap));
                        continue;
                    }
                    index = static_cast<std::size_t>((next - blocks_.begin()) - 1);
                    within = static_cast<std::size_t>(logical - blocks_[index].logical);
                    if (next != blocks_.end()) untilNext = next->logical - logical;
                } else {
                    const std::uint64_t blockIndex = logical / geometry_.blockSize;
                    if (blockIndex >= blocks_.size()) break;
                    index = static_cast<std::size_t>(blockIndex);
                    within = static_cast<std::size_t>(logical % geometry_.blockSize);
                }
                if (blocks_[index].external && !provider_) return -1;
                if (!load(index)) {
                    if (!blocks_[index].external) failed_.store(true);
                    return -1;
                }
                const CachedBlock& cached = cache_.front();
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(
                    std::min<std::size_t>(amount - done, static_cast<std::size_t>(geometry_.blockSize) - within), untilNext));
                for (const auto& range : cached.unresolved)
                    if (within < range.second && range.first < within + n) return -1;
                if (cached.clusterOffsets.empty()) {
                    std::memcpy(staged.data() + done, cached.data.data() + within, n);
                } else {
                    std::size_t copied = 0;
                    while (copied < n) {
                        const std::size_t position = within + copied;
                        const std::size_t cluster = position / geometry_.cluster;
                        const std::size_t offset = position % geometry_.cluster;
                        const std::size_t take = std::min<std::size_t>(n - copied, geometry_.cluster - offset);
                        const std::size_t stored = cached.clusterOffsets[cluster];
                        if (stored != std::numeric_limits<std::size_t>::max())
                            std::memcpy(staged.data() + done + copied, cached.data.data() + stored + offset, take);
                        copied += take;
                    }
                }
                done += n;
            }
            if (!validate()) return -1;
            std::copy(staged.begin(), staged.begin() + done, out);
            return static_cast<int>(done);
        } catch (const std::exception&) { return -1; }
    }
private:
    struct CachedBlock {
        std::size_t index;
        std::vector<std::uint8_t> data;
        std::vector<std::size_t> clusterOffsets;
        std::vector<std::pair<std::size_t, std::size_t> > unresolved;
    };
    void cacheBlock(CachedBlock block) const {
        cache_.push_front(std::move(block));
        cacheBytes_ += cache_.front().data.size() + cache_.front().clusterOffsets.size() * sizeof(std::size_t);
        const std::size_t budget = 16U * 1024U * 1024U;
        while (cacheBytes_ > budget && cache_.size() > 1) {
            cacheBytes_ -= cache_.back().data.size() + cache_.back().clusterOffsets.size() * sizeof(std::size_t);
            cache_.pop_back();
        }
    }
    bool load(std::size_t index) const {
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->index != index) continue;
            cache_.splice(cache_.begin(), cache_, it);
            return true;
        }
        const SectorBlock& block = blocks_[index];
        std::vector<std::uint8_t> externalData;
        const std::size_t bitmapSize = (std::size_t(geometry_.clustersPerBlock) + 7) / 8;
        if (block.external) {
            if (!provider_ || !provider_->read(block.hash, &externalData) || externalData.size() < bitmapSize)
                return false;
            if (geometry_.clustersPerBlock % 8 &&
                (externalData[bitmapSize - 1] >> (geometry_.clustersPerBlock % 8))) return false;
            std::size_t present = 0;
            for (std::uint32_t i = 0; i < geometry_.clustersPerBlock; ++i)
                if (externalData[i / 8] & (1U << (i % 8))) ++present;
            if (present > (externalData.size() - bitmapSize) / geometry_.cluster ||
                bitmapSize + present * geometry_.cluster != externalData.size()) return false;
            CachedBlock cached;
            cached.index = index;
            cached.clusterOffsets.resize(geometry_.clustersPerBlock, std::numeric_limits<std::size_t>::max());
            std::size_t pos = bitmapSize;
            for (std::uint32_t i = 0; i < geometry_.clustersPerBlock; ++i) {
                if (!(externalData[i / 8] & (1U << (i % 8)))) continue;
                cached.clusterOffsets[i] = pos;
                pos += geometry_.cluster;
            }
            cached.data.swap(externalData);
            cacheBlock(std::move(cached));
            return true;
        }
        std::vector<std::uint8_t> clear;
        std::vector<std::size_t> clusterOffsets(geometry_.clustersPerBlock,
                                                std::numeric_limits<std::size_t>::max());
        std::vector<std::pair<std::size_t, std::size_t> > unresolved;
        if (block.size) {
            if (block.size <= bitmapSize || block.offset > std::uint64_t(source_->capacity()) ||
                block.size > std::uint64_t(source_->capacity()) - block.offset) return false;
            std::vector<std::uint8_t> bitmap(bitmapSize);
            if (!exact(source_, block.offset, bitmap.data(), bitmap.size())) return false;
            if (geometry_.clustersPerBlock % 8 &&
                (bitmap[bitmapSize - 1] >> (geometry_.clustersPerBlock % 8))) return false;
            std::size_t present = 0;
            for (std::uint32_t i = 0; i < geometry_.clustersPerBlock; ++i)
                if (bitmap[i / 8] & (1U << (i % 8))) ++present;
            std::vector<std::uint8_t> payload;
            const std::size_t compressedSize = block.size - bitmapSize;
            ByteStorePtr compressed = std::make_shared<SubStore>(source_, block.offset + bitmapSize, compressedSize);
            StoreInflateInput input(compressed, 0);
            const std::size_t expected = present * geometry_.cluster;
            clear.resize(expected, 0);
            std::size_t payloadStart = 0;
            std::vector<std::size_t> dedupOffsets;
            const std::size_t references = geometry_.dedupMode == 1 ? expected / 4096 : 0;
            const std::size_t dedupBitmap = (references + 7) / 8;
            if (geometry_.dedupMode == 1 && expected % 4096) return false;
            if (expected > payload.max_size() - dedupBitmap) return false;
            const std::size_t maximum = expected + dedupBitmap;
            if (geometry_.rawCodec) {
                if (compressedSize < 4 || compressedSize - 4 > maximum) return false;
                payload.resize(compressedSize - 4);
                if (!exact(compressed, 0, payload.data(), payload.size())) return false;
                std::uint8_t storedChecksum[4];
                if (!exact(compressed, payload.size(), storedChecksum, sizeof(storedChecksum)) ||
                    checksum(payload.data(), payload.size()) != le32(storedChecksum)) return false;
                if (geometry_.dedupMode != 1 && payload.size() != expected) return false;
            } else if (geometry_.dedupMode == 1) {
                if (!compression::inflateZlibStreaming(input, compressedSize, [&](std::uint8_t byte) {
                    if (payload.size() >= maximum) return false;
                    payload.push_back(byte);
                    return true;
                })) return false;
            } else if (!compression::inflateZlibStreamExact(input, expected, &payload) ||
                       input.consumed() != compressedSize) return false;
            if (geometry_.dedupMode == 1) {
                if (payload.size() < dedupBitmap) return false;
                std::size_t referenceCount = 0;
                for (std::size_t i = 0; i < references; ++i)
                    if (payload[i / 8] & (1U << (i % 8))) ++referenceCount;
                const std::size_t required = dedupBitmap + referenceCount * 16 +
                    (references - referenceCount) * 4096;
                if (payload.size() != required) return false;
                payloadStart = dedupBitmap;
                dedupOffsets.reserve(references);
                for (std::size_t i = 0; i < references; ++i) {
                    dedupOffsets.push_back(payloadStart);
                    payloadStart += (payload[i / 8] & (1U << (i % 8))) ? 16 : 4096;
                }
                payloadStart = 0;
            }
            std::size_t pos = payloadStart;
            std::size_t stored = 0;
            std::size_t decodedIndex = std::numeric_limits<std::size_t>::max();
            std::vector<std::uint8_t> decodedReference;
            bool referenceValid = false;
            for (std::uint32_t i = 0; i < geometry_.clustersPerBlock; ++i) {
                if (!(bitmap[i / 8] & (1U << (i % 8)))) continue;
                const std::size_t destination = std::size_t(i) * geometry_.cluster;
                clusterOffsets[i] = stored;
                if (geometry_.dedupMode != 1) {
                    std::memcpy(clear.data() + stored, payload.data() + pos, geometry_.cluster);
                    pos += geometry_.cluster;
                    stored += geometry_.cluster;
                    continue;
                }
                for (std::size_t copied = 0; copied < geometry_.cluster;) {
                    const std::size_t blockIndex = pos / 4096;
                    const std::size_t withinBlock = pos % 4096;
                    const std::size_t n = std::min<std::size_t>(geometry_.cluster - copied, 4096 - withinBlock);
                    if (payload[blockIndex / 8] & (1U << (blockIndex % 8))) {
                        if (provider_ && decodedIndex != blockIndex) {
                            std::array<std::uint8_t, 16> hash;
                            std::copy(payload.data() + dedupOffsets[blockIndex],
                                      payload.data() + dedupOffsets[blockIndex] + hash.size(), hash.begin());
                            referenceValid = provider_->read(hash, &decodedReference) &&
                                             decodedReference.size() == 4096;
                            decodedIndex = blockIndex;
                        }
                        if (provider_ && referenceValid) {
                            std::memcpy(clear.data() + stored + copied,
                                        decodedReference.data() + withinBlock, n);
                        } else {
                            const std::size_t begin = destination + copied;
                            if (!unresolved.empty() && unresolved.back().second == begin)
                                unresolved.back().second += n;
                            else unresolved.push_back(std::make_pair(begin, begin + n));
                        }
                    } else {
                        std::memcpy(clear.data() + stored + copied,
                                    payload.data() + dedupOffsets[blockIndex] + withinBlock, n);
                    }
                    copied += n;
                    pos += n;
                }
                stored += geometry_.cluster;
            }
        }
        CachedBlock cached;
        cached.index = index;
        cached.data.swap(clear);
        cached.clusterOffsets.swap(clusterOffsets);
        cached.unresolved.swap(unresolved);
        cacheBlock(std::move(cached));
        return true;
    }
    ByteStorePtr source_;
    std::vector<SectorBlock> blocks_;
    std::shared_ptr<TibDedupProvider> provider_;
    std::uint64_t length_;
    SectorGeometry geometry_;
    std::uint64_t readableLength_;
    mutable std::mutex mutex_;
    mutable std::list<CachedBlock> cache_;
    mutable std::size_t cacheBytes_;
    mutable std::atomic<bool> failed_;
};

enum class RandomMapStatus { NotRandom, Invalid, Ok };

RandomMapStatus openRandomMap(const ByteStorePtr& source, std::uint64_t origin,
        std::uint64_t offset, std::uint64_t mapSize, std::uint32_t pageSize,
        std::uint64_t length, std::uint64_t readableLength, std::uint64_t sector,
        std::uint64_t clusterSectors, ByteStorePtr* volume) {
    std::uint8_t size = 0;
    if (!exact(source, offset, &size, 1)) return RandomMapStatus::Invalid;
    std::vector<std::uint8_t> header(std::size_t(size) + 1);
    if (!exact(source, offset, header.data(), header.size())) return RandomMapStatus::Invalid;
    const std::uint16_t tags[] = {2, 3, 4, 5, 6, 0x14, 0x8c, 0x9c, 0xd2};
    std::uint64_t values[9] = {};
    bool seen[9] = {}, random = false, raw = false, invalid = false;
    for (std::size_t pos = 1; pos < header.size();) {
        if (header.size() - pos < 3) return RandomMapStatus::Invalid;
        const std::uint16_t tag = le16(header.data() + pos);
        std::size_t n = header[pos + 2];
        pos += 3;
        if (n & 0x80) {
            if (pos == header.size()) return RandomMapStatus::Invalid;
            n = ((n & 0x7f) << 8) | header[pos++];
        }
        if (n > header.size() - pos) return RandomMapStatus::Invalid;
        if (tag == 0x9b) random = true;
        if (tag == 0x63) { if (n || raw) invalid = true; raw = true; }
        for (std::size_t i = 0; i < 9; ++i) {
            if (tag != tags[i]) continue;
            if (seen[i] || n > (i >= 7 ? 4U : 8U) || (!n && i != 3)) invalid = true;
            else for (std::size_t j = 0; j < n; ++j) values[i] |= std::uint64_t(header[pos + j]) << (j * 8);
            seen[i] = true;
        }
        pos += n;
    }
    if (!random) return RandomMapStatus::NotRandom;
    if (invalid || !pageSize || (pageSize & (pageSize - 1)) || !mapSize || mapSize != header.size() ||
        !seen[0] || !seen[1] || !seen[2] || !seen[4] || !seen[5] || !seen[6] ||
        (!seen[7] && !seen[8])) return RandomMapStatus::Invalid;
    for (std::size_t i = 0; i < 9; ++i)
        if (i != 3 && values[i] > std::numeric_limits<std::uint32_t>::max()) return RandomMapStatus::Invalid;
    if (origin > std::uint64_t(source->capacity())) return RandomMapStatus::Invalid;
    ByteStorePtr stream = std::make_shared<SubStore>(source, static_cast<std::int64_t>(origin),
        source->capacity() - static_cast<std::int64_t>(origin));
    const TibRandomAccessIndex::PageResolver resolver = [stream, pageSize](std::uint32_t page) -> ByteStorePtr {
        const std::uint64_t at = std::uint64_t(page) * pageSize;
        if (at > std::uint64_t(stream->capacity()) || pageSize > std::uint64_t(stream->capacity()) - at)
            return ByteStorePtr();
        return std::make_shared<SubStore>(stream, static_cast<std::int64_t>(at), pageSize);
    };
    std::shared_ptr<TibRandomAccessIndex> index = std::make_shared<TibRandomAccessIndex>(
        pageSize, static_cast<std::uint32_t>(values[4]), static_cast<std::uint32_t>(values[5]),
        seen[8], static_cast<std::uint32_t>(values[8]),
        seen[7], static_cast<std::uint32_t>(values[7]), resolver);
    TibRandomAccessGeometry g = {static_cast<std::uint32_t>(values[0]),
        static_cast<std::uint32_t>(values[1]), static_cast<std::uint32_t>(values[2]),
        static_cast<std::uint32_t>(values[6]), static_cast<std::uint32_t>(values[5]), values[3], raw};
    if (!index->valid() || !index->count() || g.sectorBytes != sector || g.sectorsPerBit != clusterSectors ||
        !g.sectorBytes || !g.sectorsPerBit || !g.bitsPerGroup ||
        !g.sectorsPerStoredPage || !g.indexEntriesPerGroup) return RandomMapStatus::Invalid;
    std::uint32_t first = 0;
    const TibRandomAccessIndex::Result initial = index->reference(0, &first);
    if (initial != TibRandomAccessIndex::Result::Ok && initial != TibRandomAccessIndex::Result::Absent)
        return RandomMapStatus::Invalid;
    *volume = std::make_shared<TibRandomAccessVolume>(stream, stream, index, pageSize, length, readableLength, g);
    return RandomMapStatus::Ok;
}

bool sectorMapHeader(const std::uint8_t* header, std::size_t available,
                     std::uint64_t* values, bool* strictValid, std::uint8_t* dedupMode = nullptr,
                     bool* rawCodec = nullptr, bool* extended = nullptr) {
    if (!available) return false;
    const std::size_t size = header[0];
    if (size < 16 || size + 1 > available) return false;
    std::fill(values, values + 5, 0);
    bool seen[5] = {};
    bool validStrict = true;
    bool dedupBlocks = false, dedupChunks = false, raw = false;
    bool hasFa = false, hasF4 = false;
    std::uint64_t fa = 0, f4 = 0;
    for (std::size_t pos = 1; pos < size + 1;) {
        if (size + 1 - pos < 3) return false;
        const std::uint16_t tag = le16(header + pos);
        std::size_t n = header[pos + 2];
        pos += 3;
        if (n & 0x80) {
            if (pos == size + 1) return false;
            n = ((n & 0x7f) << 8) | header[pos++];
        }
        if (n > size + 1 - pos) return false;
        if (tag == 0x9b) return false;
        if (tag == 0xd4) dedupBlocks = true;
        if (tag == 0xf7) dedupChunks = true;
        if (tag == 0x63) {
            if (n != 0 || raw) return false;
            raw = true;
        }
        if (tag == 0xfa || tag == 0xf4) {
            bool& seenTag = tag == 0xfa ? hasFa : hasF4;
            std::uint64_t& value = tag == 0xfa ? fa : f4;
            if (seenTag || !n || n > 8) return false;
            seenTag = true;
            for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(header[pos + i]) << (8 * i);
        }
        const int index = tag >= 2 && tag <= 4 ? tag - 2 : tag == 6 ? 3 : tag == 5 ? 4 : -1;
        if (index < 0) { pos += n; continue; }
        if (n > 8) return false;
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(header[pos + i]) << (8 * i);
        if (index >= 0 && (seen[index] || (!n && index != 4))) validStrict = false;
        if (tag >= 2 && tag <= 4) { values[tag - 2] = value; seen[tag - 2] = true; }
        if (tag == 6) { values[3] = value; seen[3] = true; }
        if (tag == 5) { values[4] = value; seen[4] = true; }
        pos += n;
    }
    const bool isExtended = hasFa || hasF4;
    if (isExtended) values[2] = hasFa ? fa : f4;
    const std::size_t recordSize = isExtended ? 20 : 12;
    if (!seen[0] || !seen[1] || (!isExtended && !seen[2]) || !seen[3] || values[0] < 512 || values[0] > 4096 ||
        (values[0] & (values[0] - 1)) || !values[1] || (values[1] & (values[1] - 1)) ||
        values[1] > 256 || !values[2] || values[2] > 65536 || !values[3] ||
        values[3] > std::numeric_limits<std::size_t>::max() / recordSize ||
        values[3] > std::vector<SectorBlock>().max_size() || values[4] >= values[1] * values[2] ||
        values[0] * values[1] * values[2] > 0x7fffffffU) return false;
    if (strictValid) *strictValid = validStrict;
    if (dedupMode) *dedupMode = dedupBlocks ? 1 : dedupChunks ? 2 : 0;
    if (rawCodec) *rawCodec = raw;
    if (extended) *extended = isExtended;
    return true;
}

bool readSectorMap(const ByteStorePtr& source, std::uint64_t offset, std::uint64_t origin,
                   std::vector<SectorBlock>* blocks, SectorGeometry* geometry, std::uint64_t* mapEnd = nullptr,
                   bool* strictValid = nullptr, bool* hasInitialBlock = nullptr,
                   const std::uint64_t* recordedSize = nullptr) {
    if (offset < origin) return false;
    if (recordedSize && (!*recordedSize || offset > std::uint64_t(source->capacity()) ||
        *recordedSize > std::uint64_t(source->capacity()) - offset)) return false;
    std::array<std::uint8_t, 256> header = {};
    if (!exact(source, offset, header.data(), 1)) return false;
    const std::size_t size = header[0];
    if (recordedSize && *recordedSize < 1 + size) return false;
    if (!exact(source, offset + 1, header.data() + 1, size)) return false;
    std::uint64_t values[5] = {};
    bool validStrict = false;
    std::uint8_t dedupMode = 0;
    bool rawCodec = false;
    bool extended = false;
    if (!sectorMapHeader(header.data(), size + 1, values, &validStrict, &dedupMode, &rawCodec, &extended)) return false;
    const std::size_t count = static_cast<std::size_t>(values[3]);
    const std::size_t recordSize = extended ? 20 : 12;
    const std::size_t expected = count * recordSize;
    const std::uint64_t start = offset + 1 + size;
    if (start > std::uint64_t(source->capacity())) return false;
    const std::uint64_t compressedSize = recordedSize ? *recordedSize - 1 - size : 0;
    std::vector<std::uint8_t> map;
    std::uint64_t mapBytes = 0;
    if (rawCodec) {
        if (expected > std::numeric_limits<std::size_t>::max() - 4) return false;
        const std::size_t sizeWithChecksum = expected + 4;
        if ((recordedSize && compressedSize != sizeWithChecksum) ||
            sizeWithChecksum > std::uint64_t(source->capacity()) - start) return false;
        std::vector<std::uint8_t> raw(sizeWithChecksum);
        if (!exact(source, start, raw.data(), raw.size()) ||
            checksum(raw.data(), expected) != le32(raw.data() + expected)) return false;
        raw.resize(expected);
        map.swap(raw);
        mapBytes = sizeWithChecksum;
    } else {
        ByteStorePtr bounded = recordedSize ? std::make_shared<SubStore>(source, start, compressedSize) : source;
        StoreInflateInput input(bounded, recordedSize ? 0 : start);
        if (!compression::inflateZlibStreamExact(input, expected, &map)) return false;
        if (recordedSize && input.consumed() != compressedSize) return false;
        if (input.consumed() > std::uint64_t(source->capacity()) - start) return false;
        mapBytes = input.consumed();
    }
    std::vector<SectorBlock> result;
    result.reserve(count);
    std::uint64_t previous = 0;
    std::uint64_t maxEnd = 0;
    std::uint64_t logicalNext = 0;
    const std::uint64_t cluster = values[0] * values[1];
    const std::uint64_t blockSize = cluster * values[2];
    bool referencesStart = false;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t record[20];
        for (std::size_t j = 0; j < recordSize; ++j) record[j] = map[j * count + i];
        const std::uint64_t encoded = le64(record), magnitude = encoded >> 1;
        const std::uint32_t length = le32(record + 8);
        std::uint64_t logical = 0;
        if (extended) {
            const std::uint64_t delta = le64(record + 12);
            if (!length || delta > std::numeric_limits<std::uint64_t>::max() - logicalNext) return false;
            const std::uint64_t group = logicalNext + delta;
            if (group == std::numeric_limits<std::uint64_t>::max() ||
                group > std::numeric_limits<std::uint64_t>::max() / cluster) return false;
            logical = group * cluster;
            if (logical > std::numeric_limits<std::uint64_t>::max() - blockSize) return false;
            logicalNext = group + 1;
        }
        if (dedupMode == 2 && encoded == 1) {
            if (!length) return false;
            SectorBlock block = {0, length, true, logical};
            result.push_back(block);
            continue;
        }
        if (encoded & 1U) { if (magnitude > previous) return false; previous -= magnitude; }
        else { if (magnitude > std::numeric_limits<std::uint64_t>::max() - previous) return false; previous += magnitude; }
        const std::uint64_t limit = recordedSize ? std::uint64_t(source->capacity()) - origin
                                                 : offset - origin;
        if (previous > limit || length > limit - previous) return false;
        if (previous == 0 && length) referencesStart = true;
        SectorBlock block = {previous + origin, length, false, logical};
        result.push_back(block);
        previous += length;
        maxEnd = std::max(maxEnd, previous);
    }
    const bool initialBlock = referencesStart && result.front().size;
    if (!extended && !recordedSize && maxEnd != offset - origin) return false;
    geometry->sector = static_cast<std::uint32_t>(values[0]);
    geometry->cluster = static_cast<std::uint32_t>(values[0] * values[1]);
    geometry->clustersPerBlock = static_cast<std::uint32_t>(values[2]);
    geometry->blockSize = values[0] * values[1] * values[2];
    geometry->prefix = values[4] * values[0];
    geometry->dedupMode = dedupMode;
    geometry->rawCodec = rawCodec;
    geometry->extended = extended;
    if (mapEnd) *mapEnd = start + mapBytes;
    if (strictValid) *strictValid = validStrict;
    if (hasInitialBlock) *hasInitialBlock = initialBlock;
    blocks->swap(result);
    return true;
}

bool sameGeometry(const SectorGeometry& a, const SectorGeometry& b) {
    return a.sector == b.sector && a.cluster == b.cluster &&
           a.clustersPerBlock == b.clustersPerBlock && a.prefix == b.prefix &&
           a.dedupMode == b.dedupMode && a.rawCodec == b.rawCodec && a.extended == b.extended;
}

bool collectSectorFragments(const ByteStorePtr& source, std::uint64_t lastOffset,
                            std::vector<SectorBlock>* blocks, SectorGeometry* geometry,
                            bool* fragmented) {
    const std::size_t window = 1024 * 1024;
    struct Fragment {
        std::uint64_t offset, end;
        SectorGeometry geometry;
        std::vector<SectorBlock> blocks;
        bool validStrict;
    };
    std::uint64_t start = 32;
    std::vector<Fragment> fragments;
    *fragmented = false;
    while (start <= lastOffset) {
        const std::uint64_t remaining = lastOffset - start;
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining + 1, window));
        const std::size_t extra = static_cast<std::size_t>(std::min<std::uint64_t>(
            255, std::uint64_t(source->capacity()) - start - length));
        std::vector<std::uint8_t> data(length + extra);
        if (!exact(source, start, data.data(), data.size())) return false;
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint64_t offset = start + i;
            std::uint64_t values[5];
            if (!sectorMapHeader(data.data() + i, data.size() - i, values, nullptr)) continue;
            std::vector<SectorBlock> fragment;
            SectorGeometry current = {};
            std::uint64_t mapEnd = 0;
            bool validStrict = false;
            if (!readSectorMap(source, offset, 32, &fragment, &current, &mapEnd, &validStrict)) continue;
            if (offset != lastOffset && mapEnd > lastOffset) continue;
            Fragment candidate = {offset, mapEnd, current, std::move(fragment), validStrict};
            fragments.push_back(std::move(candidate));
        }
        start += length;
    }
    if (fragments.empty() || fragments.back().offset != lastOffset) return false;
    if (fragments.size() == 1) return true;
    std::vector<SectorBlock> combined;
    std::uint64_t previousMapEnd = 32;
    const SectorGeometry selected = fragments.front().geometry;
    for (Fragment& fragment : fragments) {
        if (!fragment.validStrict) return false;
        if (!sameGeometry(selected, fragment.geometry) || fragment.offset < previousMapEnd) return false;
        std::uint64_t previousEnd = previousMapEnd;
        for (const SectorBlock& block : fragment.blocks) {
            if (block.external) return false;
            if (!block.size) continue;
            if (block.offset < previousEnd || block.offset > fragment.offset ||
                block.size > fragment.offset - block.offset) return false;
            previousEnd = block.offset + block.size;
        }
        if (fragment.blocks.size() > combined.max_size() - combined.size()) return false;
        if (selected.extended && !combined.empty() && !fragment.blocks.empty() &&
            fragment.blocks.front().logical < combined.back().logical + selected.blockSize) return false;
        combined.insert(combined.end(), fragment.blocks.begin(), fragment.blocks.end());
        previousMapEnd = fragment.end;
    }
    *fragmented = true;
    blocks->swap(combined);
    *geometry = selected;
    return true;
}

bool sectorVolumeLength(const ByteStorePtr& source, const SectorGeometry& geometry, std::uint64_t mapOffset,
                        std::uint64_t* length) {
    if (!source || !length || source->capacity() < 80) return false;
    const std::uint64_t size = static_cast<std::uint64_t>(source->capacity());
    std::array<std::uint8_t, 48> footer = {};
    if (!exact(source, size - footer.size(), footer.data(), footer.size()) ||
        std::memcmp(footer.data() + 44, "\xa2\xb9\x24\xce", 4)) return false;
    const std::uint64_t sliceSize = le64(footer.data() + 8);
    if (sliceSize > size - 32 || sliceSize < 8) return false;
    const std::uint64_t sliceEnd = 32 + sliceSize;
    std::array<std::uint8_t, 8> trailerEnd = {};
    if (!exact(source, sliceEnd - 8, trailerEnd.data(), trailerEnd.size()) ||
        le32(trailerEnd.data() + 4) != 0x94e18a2bU) return false;
    const std::uint32_t trailerSize = le32(trailerEnd.data());
    if (trailerSize < 4 || trailerSize > sliceSize - 8) return false;
    const std::uint64_t trailerStart = sliceEnd - 8 - trailerSize;
    std::array<std::uint8_t, 11> first = {};
    if (!exact(source, trailerStart, first.data(), 3) || le16(first.data()) != 0 ||
        !first[2] || first[2] > 8 || 3U + first[2] > trailerSize ||
        !exact(source, trailerStart + 3, first.data() + 3, first[2])) return false;
    std::uint64_t metadataOffset = 0;
    for (unsigned i = 0; i < first[2]; ++i) metadataOffset |= std::uint64_t(first[3 + i]) << (i * 8);
    if (metadataOffset > trailerStart || metadataOffset > size - 32) return false;
    metadataOffset += 32;
    if (metadataOffset >= trailerStart) return false;
    const std::size_t tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(
        65535, trailerStart - metadataOffset));
    std::vector<std::uint8_t> tail(tailSize);
    if (!exact(source, trailerStart - tailSize, tail.data(), tail.size())) return false;
    bool matched = false;
    std::uint64_t foundLength = 0;
    for (std::size_t start = 0; start + 2 <= tail.size(); ++start) {
        if (le16(tail.data() + start) != tail.size() - start) continue;
        std::uint64_t sector = 0, clusters = 0, sectors = 0, recordedMap = 0;
        bool hasMap = false, hasSector = false, hasClusters = false, hasStart = false, hasSize = false;
        bool valid = true;
        std::size_t pos = start + 2;
        while (pos + 3 <= tail.size()) {
            const std::uint16_t tag = le16(tail.data() + pos);
            const std::size_t n = tail[pos + 2];
            pos += 3;
            if (n > tail.size() - pos) { valid = false; break; }
            if (tag == 0 || tag == 2 || tag == 3 || tag == 0x11 || tag == 0x12) {
                if (!n || n > 8) { valid = false; break; }
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(tail[pos + i]) << (i * 8);
                bool* seen = tag == 0 ? &hasMap : tag == 2 ? &hasSector : tag == 3 ? &hasClusters : tag == 0x11 ? &hasStart : &hasSize;
                if (*seen) { valid = false; break; }
                *seen = true;
                if (tag == 0) recordedMap = value;
                else if (tag == 2) sector = value;
                else if (tag == 3) clusters = value;
                else if (tag == 0x12) sectors = value;
            }
            pos += n;
        }
        if (!valid || pos != tail.size() || !hasMap || mapOffset < 32 || recordedMap != mapOffset - 32 ||
            !hasSector || !hasClusters || !hasStart || !hasSize ||
            sector != geometry.sector || clusters != geometry.cluster / geometry.sector || !sectors ||
            sectors > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / sector) continue;
        if (matched) return false;
        matched = true;
        foundLength = sectors * sector;
    }
    if (!matched) return false;
    *length = foundLength;
    return true;
}

bool openIndexedVolumes(const ByteStorePtr& source, std::uint64_t origin, std::uint64_t end,
                        std::vector<TibLegacyPartition>* partitions, bool* recognized);

bool openSectorDisk(const ByteStorePtr& source, std::vector<TibLegacyPartition>* partitions,
                    const std::shared_ptr<TibDedupProvider>& provider, bool* needsProviderDuringProbe) {
    if (source->capacity() < 512) return false;
    std::array<std::uint8_t, 32> header = {};
    if (!exact(source, 0, header.data(), header.size()) || le32(header.data() + 4) != 32 ||
        le32(header.data() + 28) != 32) return false;
    const std::uint32_t expectedChecksum = le32(header.data() + 24);
    std::fill(header.begin() + 24, header.begin() + 28, 0);
    if (checksum(header.data(), header.size()) != expectedChecksum) return false;
    const std::uint64_t sourceSize = static_cast<std::uint64_t>(source->capacity());
    std::array<std::uint8_t, 48> footer = {};
    if (sourceSize >= footer.size() && exact(source, sourceSize - footer.size(), footer.data(), footer.size()) &&
        std::memcmp(footer.data() + 44, "\xa2\xb9\x24\xce", 4) == 0) {
        const std::uint64_t sliceSize = le64(footer.data() + 8);
        bool recognized = false;
        if (sliceSize <= sourceSize - 32 &&
            openIndexedVolumes(source, 32, 32 + sliceSize, partitions, &recognized)) return true;
        if (recognized) return false;
    }
    bool indexed = false;
    if (openIndexedVolumes(source, 32, sourceSize, partitions, &indexed)) return true;
    if (indexed) return false;
    const std::uint64_t length = static_cast<std::uint64_t>(source->capacity());
    const std::size_t window = 1024 * 1024;
    std::uint64_t end = length;
    std::vector<SectorBlock> blocks;
    SectorGeometry geometry = {};
    std::uint64_t mapOffset = 0;
    bool hasInitialBlock = false;
    while (end > 32) {
        const std::uint64_t start = end > window ? std::max<std::uint64_t>(32, end - window) : 32;
        const std::size_t candidates = static_cast<std::size_t>(end - start);
        const std::size_t extra = static_cast<std::size_t>(std::min<std::uint64_t>(255, length - end));
        std::vector<std::uint8_t> data(candidates + extra);
        if (!exact(source, start, data.data(), data.size())) return false;
        for (std::size_t i = candidates; i-- > 0;) {
            std::uint64_t values[5];
            if (!sectorMapHeader(data.data() + i, data.size() - i, values, nullptr)) continue;
            if (readSectorMap(source, start + i, 32, &blocks, &geometry,
                              nullptr, nullptr, &hasInitialBlock)) {
                mapOffset = start + i;
                break;
            }
        }
        if (!blocks.empty()) break;
        if (start == 32) break;
        end = start;
    }
    if (blocks.empty()) return false;
    bool fragmented = false;
    if (!geometry.extended && !hasInitialBlock &&
        (!collectSectorFragments(source, mapOffset, &blocks, &geometry, &fragmented) || !fragmented)) return false;
    std::uint64_t volumeLength = 0;
    const bool hasMetadataLength = geometry.extended && sectorVolumeLength(source, geometry, mapOffset, &volumeLength);
    if (geometry.extended && !hasMetadataLength) return false;
    if (!hasMetadataLength) {
        std::uint64_t probeLength = 0;
        if (geometry.extended) {
            probeLength = std::min<std::uint64_t>(blocks.back().logical + geometry.blockSize,
                                                  std::numeric_limits<std::int64_t>::max());
        } else {
            if (blocks.size() > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / geometry.blockSize) return false;
            probeLength = std::uint64_t(blocks.size()) * geometry.blockSize;
        }
        const std::shared_ptr<TibSectorVolume> probe =
            std::make_shared<TibSectorVolume>(source, blocks, probeLength, geometry);
        probe->setDedupProvider(provider);
        std::array<std::uint8_t, 512> boot = {};
        if (!exact(probe, 0, boot.data(), boot.size())) {
            if (!provider && geometry.dedupMode && needsProviderDuringProbe)
                *needsProviderDuringProbe = true;
            return false;
        }
        if (!sectorVolumeLength(source, geometry, mapOffset, &volumeLength)) {
            if (std::memcmp(boot.data() + 3, "NTFS    ", 8) || boot[510] != 0x55 || boot[511] != 0xaa ||
                le16(boot.data() + 11) != geometry.sector ||
                std::uint64_t(boot[13]) * geometry.sector != geometry.cluster) return false;
            const std::uint64_t sectors = le64(boot.data() + 40);
            if (!sectors || sectors >= std::uint64_t(std::numeric_limits<std::int64_t>::max()) / geometry.sector) return false;
            volumeLength = (sectors + 1) * geometry.sector;
        }
    }
    if (geometry.extended) {
        if (blocks.back().logical >= volumeLength + geometry.prefix) return false;
    } else {
        if ((blocks.size() - 1) * geometry.blockSize >= volumeLength + geometry.prefix) return false;
        if (fragmented && std::uint64_t(blocks.size()) * geometry.blockSize < volumeLength + geometry.prefix) return false;
    }
    const std::shared_ptr<TibSectorVolume> volume =
        std::make_shared<TibSectorVolume>(source, std::move(blocks), volumeLength, geometry);
    volume->setDedupProvider(provider);
    TibLegacyPartition partition = {1, volume};
    partitions->push_back(std::move(partition));
    return true;
}

bool openIndexedVolumes(const ByteStorePtr& source, std::uint64_t origin, std::uint64_t end,
                        std::vector<TibLegacyPartition>* partitions, bool* recognized) {
    if (recognized) *recognized = false;
    if (!source || source->capacity() < 0 || end > std::uint64_t(source->capacity()) ||
        end < origin + 8) return false;
    std::array<std::uint8_t, 8> trailer = {};
    if (!exact(source, end - trailer.size(), trailer.data(), trailer.size()) ||
        le32(trailer.data() + 4) != 0x94e18a2bU) return false;
    const std::uint32_t trailerSize = le32(trailer.data());
    if (trailerSize < 4 || trailerSize > end - trailer.size() - origin) return false;
    const std::uint64_t trailerStart = end - trailer.size() - trailerSize;
    std::array<std::uint8_t, 11> locator = {};
    if (!exact(source, trailerStart, locator.data(), 3) || le16(locator.data()) != 0 ||
        !locator[2] || locator[2] > 8 || 3U + locator[2] > trailerSize ||
        !exact(source, trailerStart + 3, locator.data() + 3, locator[2])) return false;
    std::uint64_t relative = 0;
    for (unsigned i = 0; i < locator[2]; ++i) relative |= std::uint64_t(locator[3 + i]) << (i * 8);
    if (relative > trailerStart - origin) return false;
    std::uint32_t randomPageSize = 0;
    bool pageSizeSeen = false, listHeaderValid = true;
    if (trailerSize <= 262144) {
        std::vector<std::uint8_t> listHeader(trailerSize);
        if (!exact(source, trailerStart, listHeader.data(), listHeader.size())) return false;
        for (std::size_t at = 0; at < listHeader.size();) {
            if (listHeader.size() - at < 3) { listHeaderValid = false; break; }
            const std::uint16_t tag = le16(listHeader.data() + at);
            std::size_t n = listHeader[at + 2];
            at += 3;
            if (n & 0x80) {
                if (at == listHeader.size()) { listHeaderValid = false; break; }
                n = ((n & 0x7f) << 8) | listHeader[at++];
            }
            if (n > listHeader.size() - at) { listHeaderValid = false; break; }
            if (tag == 3 && !pageSizeSeen) {
                if (!n || n > 4) listHeaderValid = false;
                else for (std::size_t i = 0; i < n; ++i) randomPageSize |= std::uint32_t(listHeader[at + i]) << (i * 8);
                pageSizeSeen = true;
            }
            at += n;
        }
    }
    if (!listHeaderValid) randomPageSize = 0;
    std::uint64_t pos = relative + origin;
    std::vector<TibLegacyPartition> result;
    std::set<unsigned int> indexes;
    while (pos < trailerStart) {
        std::array<std::uint8_t, 2> prefix = {};
        if (!exact(source, pos, prefix.data(), prefix.size())) return false;
        const std::size_t frameSize = le16(prefix.data());
        if (frameSize < 2 || frameSize > trailerStart - pos) return false;
        std::vector<std::uint8_t> frame(frameSize - 2);
        if (!exact(source, pos + 2, frame.data(), frame.size())) return false;
        std::uint64_t values[6] = {};
        bool seen[6] = {};
        std::uint64_t mapSize = 0;
        bool hasMapSize = false, invalidMapSize = false;
        std::uint64_t readableSectors = 0;
        bool hasReadableSectors = false, invalidReadableSectors = false;
        std::uint64_t dedupTablePosition = 0, dedupTableSize = 0;
        bool hasDedupPosition = false, hasDedupSize = false, invalidDedup = false;
        std::uint64_t filterOffset = 0, filterSize = 0;
        bool hasFilterOffset = false, hasFilterSize = false, invalidFilter = false;
        std::size_t at = 0;
        while (at < frame.size()) {
            if (frame.size() - at < 3) return false;
            const std::uint16_t tag = le16(frame.data() + at);
            std::size_t n = frame[at + 2];
            at += 3;
            if (n & 0x80) {
                if (at == frame.size()) return false;
                n = ((n & 0x7f) << 8) | frame[at++];
            }
            if (n > frame.size() - at) return false;
            const int which = tag == 0 ? 0 : tag == 2 ? 1 : tag == 3 ? 2 :
                              tag == 0x11 ? 3 : tag == 0x12 ? 4 : tag == 0x45 ? 5 : -1;
            if (tag == 1) {
                if (!n || n > 8 || hasMapSize) invalidMapSize = true;
                else for (std::size_t j = 0; j < n; ++j) mapSize |= std::uint64_t(frame[at + j]) << (j * 8);
                hasMapSize = true;
            }
            if (tag == 0x13) {
                if (!n || n > 8 || hasReadableSectors) invalidReadableSectors = true;
                else for (std::size_t j = 0; j < n; ++j) readableSectors |= std::uint64_t(frame[at + j]) << (j * 8);
                hasReadableSectors = true;
            }
            if (tag == 0x5d || tag == 0x5e) {
                bool& seenDedup = tag == 0x5d ? hasDedupPosition : hasDedupSize;
                if (!n || n > 8 || seenDedup) invalidDedup = true;
                else {
                    std::uint64_t& value = tag == 0x5d ? dedupTablePosition : dedupTableSize;
                    for (std::size_t j = 0; j < n; ++j) value |= std::uint64_t(frame[at + j]) << (j * 8);
                }
                seenDedup = true;
            }
            if (tag == 0xcc || tag == 0xcd) {
                bool& present = tag == 0xcc ? hasFilterOffset : hasFilterSize;
                std::uint64_t& value = tag == 0xcc ? filterOffset : filterSize;
                if (n > 8 || present) invalidFilter = true;
                else for (std::size_t j = 0; j < n; ++j)
                    value |= std::uint64_t(frame[at + j]) << (j * 8);
                present = true;
            }
            if (which >= 0) {
                if (!n || n > 8 || seen[which]) return false;
                for (std::size_t j = 0; j < n; ++j) values[which] |= std::uint64_t(frame[at + j]) << (j * 8);
                seen[which] = true;
            }
            at += n;
        }
        if (seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && seen[5]) {
            if (recognized) *recognized = true;
            if (invalidMapSize || invalidReadableSectors || invalidFilter) return false;
            if (!values[1] || !values[2] || !values[4] || !values[5] ||
                values[5] > std::numeric_limits<unsigned int>::max() ||
                values[0] > trailerStart - origin ||
                values[4] > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / values[1]) return false;
            const unsigned int index = static_cast<unsigned int>(values[5]);
            if (!indexes.insert(index).second) return false;
            const std::uint64_t length = values[4] * values[1];
            const std::uint64_t readableLength = hasReadableSectors && readableSectors < values[4]
                ? readableSectors * values[1] : length;
            std::vector<SectorBlock> blocks;
            SectorGeometry geometry = {};
            ByteStorePtr randomVolume;
            const RandomMapStatus randomStatus = openRandomMap(source, origin, values[0] + origin,
                hasMapSize ? mapSize : 0, randomPageSize, length, readableLength,
                values[1], values[2], &randomVolume);
            if (randomStatus == RandomMapStatus::Invalid) return false;
            if (randomStatus == RandomMapStatus::NotRandom &&
                (!readSectorMap(source, values[0] + origin, origin, &blocks, &geometry,
                               nullptr, nullptr, nullptr, hasMapSize ? &mapSize : nullptr) ||
                 geometry.sector != values[1] || geometry.cluster / geometry.sector != values[2])) return false;
            if (randomStatus == RandomMapStatus::NotRandom && geometry.dedupMode == 2 &&
                (invalidDedup || !hasDedupPosition || !hasDedupSize ||
                 blocks.size() > std::numeric_limits<std::uint32_t>::max() / 16 ||
                 dedupTableSize != std::uint64_t(blocks.size()) * 16)) return false;
            if (randomStatus == RandomMapStatus::NotRandom && geometry.dedupMode == 2) {
                if (dedupTablePosition > end - origin || dedupTableSize > end - origin - dedupTablePosition)
                    return false;
                for (std::size_t i = 0; i < blocks.size(); ++i)
                    if (!exact(source, origin + dedupTablePosition + i * 16,
                               blocks[i].hash.data(), blocks[i].hash.size())) return false;
            }
            if (randomStatus == RandomMapStatus::NotRandom && geometry.extended) {
                if (blocks.back().logical >= length + geometry.prefix) return false;
            } else if (randomStatus == RandomMapStatus::NotRandom &&
                       (blocks.size() - 1 > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / geometry.blockSize ||
                       (blocks.size() - 1) * geometry.blockSize >= length + geometry.prefix)) return false;
            ByteStorePtr content = randomVolume;
            if (randomStatus == RandomMapStatus::NotRandom)
                content = std::make_shared<TibSectorVolume>(source, std::move(blocks), length, geometry, readableLength);
            TibLegacyPartition partition = {index, content};
            if (filterSize) {
                if (filterOffset > std::uint64_t(source->capacity()) - origin ||
                    filterSize > std::uint64_t(source->capacity()) - origin - filterOffset ||
                    filterSize > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return false;
                partition.fatClusterFilter = std::make_shared<SubStore>(source,
                    static_cast<std::int64_t>(origin + filterOffset), static_cast<std::int64_t>(filterSize));
            }
            result.push_back(std::move(partition));
        }
        pos += frameSize;
    }
    if (result.empty()) return false;
    std::sort(result.begin(), result.end(), [](const TibLegacyPartition& a, const TibLegacyPartition& b) {
        return a.index < b.index;
    });
    partitions->swap(result);
    return true;
}

bool openLegacyCompressed(const ByteStorePtr& source, std::vector<TibLegacyPartition>* partitions,
                          bool* recognized) {
    if (recognized) *recognized = false;
    if (!source || source->capacity() < 32) return false;
    const std::uint64_t size = static_cast<std::uint64_t>(source->capacity());
    std::array<std::uint8_t, 12> footer = {};
    if (!exact(source, size - footer.size(), footer.data(), footer.size()) ||
        le32(footer.data() + 8) != 0x179631b4U || le64(footer.data()) != size - 24) return false;
    return openIndexedVolumes(source, kHeaderSize, size - footer.size(), partitions, recognized);
}

struct Stream {
    std::uint64_t length;
    std::uint32_t hiddenSectors;
    std::uint32_t bytesPerSector;
    ByteStorePtr volume;
    std::uint64_t end;
};

bool powerOfTwo(std::uint32_t value) { return value && !(value & (value - 1)); }

bool readCandidate(const ByteStorePtr& source, std::uint64_t start, std::uint32_t cluster,
                   Stream* result) {
    const std::size_t bitmapSize = static_cast<std::size_t>((kBlockSize / cluster + 7) / 8);
    std::array<std::uint8_t, 512> boot = {};
    std::vector<std::uint8_t> bitmap(bitmapSize);
    if (!exact(source, start, bitmap.data(), bitmap.size()) || !(bitmap[0] & 1U) ||
        !exact(source, start + bitmapSize, boot.data(), boot.size()) ||
        boot[510] != 0x55 || boot[511] != 0xaa) return false;
    const std::uint32_t sector = le16(boot.data() + 11);
    const std::uint32_t sectorsPerCluster = boot[13];
    if (!powerOfTwo(sector) || sector < 512 || sector > 4096 ||
        !powerOfTwo(sectorsPerCluster) || sector * sectorsPerCluster != cluster) return false;
    const std::uint32_t sectors = le16(boot.data() + 19) ? le16(boot.data() + 19) : le32(boot.data() + 32);
    const std::uint64_t length = std::uint64_t(sectors) * sector;
    if (!length || length > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return false;
    const std::uint64_t count64 = (length + kBlockSize - 1) / kBlockSize;
    if (count64 > std::uint64_t(source->capacity()) / 4 ||
        count64 > std::numeric_limits<std::size_t>::max()) return false;
    const std::size_t count = static_cast<std::size_t>(count64);
    std::vector<Block> blocks;
    blocks.reserve(std::min<std::size_t>(count, 4096));
    std::uint64_t offset = start;
    for (std::size_t i = 0; i < count; ++i) {
        Block block;
        block.offset = offset;
        block.bitmap.resize(bitmapSize);
        if (!exact(source, offset, block.bitmap.data(), bitmapSize)) return false;
        const std::uint64_t remaining = length - std::uint64_t(i) * kBlockSize;
        const std::size_t valid = static_cast<std::size_t>((std::min(kBlockSize, remaining) + cluster - 1) / cluster);
        std::size_t present = 0;
        for (std::size_t bit = 0; bit < block.bitmap.size() * 8; ++bit) {
            if ((block.bitmap[bit / 8] & (1U << (bit % 8))) == 0) continue;
            if (bit >= valid) return false;
            ++present;
        }
        block.packed = static_cast<std::uint32_t>(present * cluster);
        const std::uint64_t stored = bitmapSize + std::uint64_t(block.packed) + 4;
        if (offset > std::uint64_t(source->capacity()) || stored > std::uint64_t(source->capacity()) - offset) return false;
        offset += stored;
        blocks.push_back(std::move(block));
    }

    std::vector<SectorBlock> indexed;
    SectorGeometry geometry = {};
    std::uint64_t mapEnd = 0;
    if (!readSectorMap(source, offset, kHeaderSize, &indexed, &geometry, &mapEnd) ||
        !geometry.rawCodec || geometry.extended || geometry.dedupMode != 0 ||
        geometry.sector != sector || geometry.cluster != cluster ||
        geometry.clustersPerBlock != kBlockSize / cluster || indexed.size() != count)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (indexed[i].external || indexed[i].offset != blocks[i].offset ||
            indexed[i].size != bitmapSize + std::uint64_t(blocks[i].packed) + 4) return false;
    }
    offset = mapEnd;
    if (count > (std::uint64_t(source->capacity()) - offset) / 16) return false;
    offset += std::uint64_t(count) * 16;
    result->length = length;
    result->hiddenSectors = le32(boot.data() + 28);
    result->bytesPerSector = sector;
    result->volume = std::make_shared<TibLegacyVolume>(source, length, cluster, std::move(blocks));
    result->end = offset;
    return true;
}

bool nextStream(const ByteStorePtr& source, std::uint64_t offset, Stream* stream) {
    bool found = false;
    for (std::uint32_t cluster = 512; cluster <= kBlockSize; cluster *= 2) {
        Stream candidate;
        if (!readCandidate(source, offset, cluster, &candidate)) continue;
        if (found) return false;
        *stream = std::move(candidate);
        found = true;
    }
    return found;
}

} // namespace

void attachTibLegacyDedupProvider(std::vector<TibLegacyPartition>* partitions,
                                  const std::shared_ptr<TibDedupProvider>& provider) {
    if (!partitions || !provider) return;
    for (TibLegacyPartition& partition : *partitions) {
        const auto sector = std::dynamic_pointer_cast<TibSectorVolume>(partition.content);
        if (sector) sector->setDedupProvider(provider);
    }
}

bool tibLegacyNeedsDedupProvider(const std::vector<TibLegacyPartition>& partitions) {
    for (const TibLegacyPartition& partition : partitions) {
        const auto sector = std::dynamic_pointer_cast<TibSectorVolume>(partition.content);
        if (sector && sector->needsDedupProvider()) return true;
    }
    return false;
}

ByteStorePtr TibLegacyFilteredVolume::resolveExternalWofResource(
    const std::string& path, const WofWimReference& reference) const {
    const auto resolver = std::dynamic_pointer_cast<const IWofResourceResolver>(source_);
    return resolver ? resolver->resolveExternalWofResource(path, reference) : nullptr;
}

std::vector<ByteStorePtr> makeTibLegacyPartitionStores(
    const std::vector<TibLegacyPartition>& partitions) {
    std::shared_ptr<std::vector<ByteStorePtr> > siblings =
        std::make_shared<std::vector<ByteStorePtr> >();
    siblings->reserve(partitions.size());
    for (const TibLegacyPartition& partition : partitions)
        siblings->push_back(partition.content);

    const std::shared_ptr<const std::vector<ByteStorePtr> > context = siblings;
    std::vector<ByteStorePtr> result;
    result.reserve(partitions.size());
    for (const TibLegacyPartition& partition : partitions)
        result.push_back(std::make_shared<TibLegacyPartitionStore>(partition.content, context));
    return result;
}

bool openTibLegacyDisk(const ByteStorePtr& source, std::vector<TibLegacyPartition>* partitions,
                       std::string* error, const std::shared_ptr<TibDedupProvider>& provider,
                       bool* needsProviderDuringProbe) {
    if (needsProviderDuringProbe) *needsProviderDuringProbe = false;
    if (!source || !partitions) return fail(error, "Missing TIB source");
    std::array<std::uint8_t, 12> header = {};
    if (exact(source, 0, header.data(), header.size()) && le32(header.data()) == 0xa2b924ceU) {
        tibheader::Header parsed;
        if (!tibheader::read(source, &parsed, error)) return false;
        if (!openSectorDisk(source, partitions, provider, needsProviderDuringProbe))
            return fail(error, "Unsupported TIB sector stream");
        if (error) error->clear();
        return true;
    }
    if (!exact(source, 0, header.data(), header.size()) || le32(header.data()) != 0x44686eb4U ||
        le32(header.data() + 8) != 1) return fail(error, "Unsupported TIB header");
    bool indexed = false;
    if (openLegacyCompressed(source, partitions, &indexed)) {
        if (error) error->clear();
        return true;
    }
    if (indexed) return fail(error, "Could not read all TIB volume maps");
    std::vector<Stream> streams;
    std::uint64_t offset = kHeaderSize;
    std::array<std::uint8_t, 516> mbr = {};
    for (;;) {
        if (exact(source, offset, mbr.data(), mbr.size()) && le32(mbr.data()) == 0 &&
            mbr[514] == 0x55 && mbr[515] == 0xaa) break;
        Stream stream;
        if (!nextStream(source, offset, &stream) || stream.end <= offset)
            return fail(error, "Unsupported TIB stream or metadata");
        offset = stream.end;
        streams.push_back(std::move(stream));
    }
    if (streams.empty()) return fail(error, "TIB contains no volumes");
    std::vector<TibLegacyPartition> result;
    std::vector<bool> used(streams.size(), false);
    for (unsigned i = 0; i < 4; ++i) {
        const std::uint8_t* entry = mbr.data() + 4 + 446 + 16 * i;
        if (!entry[4] || !le32(entry + 12)) continue;
        const std::uint64_t first = le32(entry + 8);
        const std::uint64_t length = std::uint64_t(le32(entry + 12)) * 512;
        std::size_t match = streams.size();
        for (std::size_t j = 0; j < streams.size(); ++j) {
            if (used[j] || streams[j].length != length ||
                std::uint64_t(streams[j].hiddenSectors) * streams[j].bytesPerSector != first * 512) continue;
            if (match != streams.size()) return fail(error, "Ambiguous TIB partition mapping");
            match = j;
        }
        if (match == streams.size()) return fail(error, "TIB partition stream not found");
        used[match] = true;
        TibLegacyPartition part = {i + 1, streams[match].volume};
        result.push_back(std::move(part));
    }
    if (result.size() != streams.size()) return fail(error, "Unmapped TIB stream");
    partitions->swap(result);
    if (error) error->clear();
    return true;
}

} // namespace fs
} // namespace peare
