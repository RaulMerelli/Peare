#include "AndroidSparseStore.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kSparseMagic = 0xed26ff3aU;
const std::uint16_t kRawChunk = 0xcac1U;
const std::uint16_t kFillChunk = 0xcac2U;
const std::uint16_t kDontCareChunk = 0xcac3U;
const std::uint16_t kCrc32Chunk = 0xcac4U;

std::uint16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t updateCrc(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}

bool readExact(const ByteStorePtr& source, std::int64_t offset,
               std::uint8_t* data, int size) {
    int done = 0;
    while (done < size) {
        const int count = source->read(offset + done, data + done, size - done);
        if (count <= 0 || count > size - done) return false;
        done += count;
    }
    return true;
}

}  // namespace

std::shared_ptr<AndroidSparseStore> AndroidSparseStore::open(
    const ByteStorePtr& source, std::string* error) {
    std::shared_ptr<AndroidSparseStore> result(new AndroidSparseStore(source));
    if (!source || !result->parse(error)) return nullptr;
    return result;
}

bool AndroidSparseStore::parse(std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (source_->capacity() < 28) return fail("Truncated Android sparse header");

    std::uint8_t header[28];
    if (!readExact(source_, 0, header, sizeof(header)))
        return fail("Cannot read Android sparse header");
    if (readLe32(header) != kSparseMagic || readLe16(header + 4) != 1 ||
        readLe16(header + 6) != 0)
        return fail("Unsupported Android sparse signature or version");

    const std::uint16_t fileHeaderSize = readLe16(header + 8);
    const std::uint16_t chunkHeaderSize = readLe16(header + 10);
    const std::uint32_t blockSize = readLe32(header + 12);
    const std::uint32_t totalBlocks = readLe32(header + 16);
    const std::uint32_t totalChunks = readLe32(header + 20);
    imageChecksum_ = readLe32(header + 24);
    if (fileHeaderSize < sizeof(header) || chunkHeaderSize < 12 ||
        blockSize < 4 || (blockSize & 3U) != 0)
        return fail("Invalid Android sparse header sizes");
    if (fileHeaderSize > static_cast<std::uint64_t>(source_->capacity()) ||
        totalChunks > static_cast<std::uint64_t>(source_->capacity()) / chunkHeaderSize)
        return fail("Android sparse header exceeds the input bounds");
    if (totalBlocks > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) / blockSize)
        return fail("Android sparse output size overflows");

    outputSize_ = static_cast<std::int64_t>(totalBlocks) * blockSize;
    std::int64_t inputOffset = fileHeaderSize;
    std::int64_t outputOffset = 0;
    for (std::uint32_t index = 0; index < totalChunks; ++index) {
        if (inputOffset > source_->capacity() - chunkHeaderSize)
            return fail("Truncated Android sparse chunk header");
        std::vector<std::uint8_t> chunkHeader(chunkHeaderSize);
        if (!readExact(source_, inputOffset, chunkHeader.data(), chunkHeaderSize))
            return fail("Cannot read Android sparse chunk header");
        const std::uint16_t type = readLe16(chunkHeader.data());
        const std::uint32_t blocks = readLe32(chunkHeader.data() + 4);
        const std::uint32_t totalSize = readLe32(chunkHeader.data() + 8);
        if (totalSize < chunkHeaderSize || inputOffset > source_->capacity() - totalSize)
            return fail("Android sparse chunk exceeds the input bounds");

        const std::uint64_t outputLength64 = static_cast<std::uint64_t>(blocks) * blockSize;
        if (outputLength64 > static_cast<std::uint64_t>(outputSize_ - outputOffset))
            return fail("Android sparse chunks exceed the declared output size");
        const std::int64_t outputLength = static_cast<std::int64_t>(outputLength64);
        const std::int64_t payloadOffset = inputOffset + chunkHeaderSize;
        const std::uint32_t payloadSize = totalSize - chunkHeaderSize;

        if (type == kRawChunk) {
            if (payloadSize != outputLength64)
                return fail("Android sparse RAW chunk has an invalid size");
            regions_.push_back({ChunkKind::Raw, outputOffset, outputLength,
                                payloadOffset, 0});
            outputOffset += outputLength;
        } else if (type == kFillChunk) {
            if (payloadSize != 4 || outputLength == 0)
                return fail("Android sparse FILL chunk has an invalid size");
            std::uint8_t value[4];
            if (!readExact(source_, payloadOffset, value, sizeof(value)))
                return fail("Cannot read Android sparse FILL value");
            regions_.push_back({ChunkKind::Fill, outputOffset, outputLength,
                                0, readLe32(value)});
            outputOffset += outputLength;
        } else if (type == kDontCareChunk) {
            if (payloadSize != 0 || outputLength == 0)
                return fail("Android sparse DONT_CARE chunk has an invalid size");
            regions_.push_back({ChunkKind::Zero, outputOffset, outputLength, 0, 0});
            outputOffset += outputLength;
        } else if (type == kCrc32Chunk) {
            if (blocks != 0 || payloadSize != 4)
                return fail("Android sparse CRC32 chunk has an invalid size");
            std::uint8_t value[4];
            if (!readExact(source_, payloadOffset, value, sizeof(value)))
                return fail("Cannot read Android sparse CRC32 value");
            checksums_.push_back({outputOffset, readLe32(value)});
        } else {
            return fail("Unknown Android sparse chunk type");
        }
        inputOffset += totalSize;
    }

    if (outputOffset != outputSize_ || inputOffset != source_->capacity())
        return fail("Android sparse chunk totals do not match the header");
    return true;
}

int AndroidSparseStore::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (!dst || count <= 0 || pos < 0 || pos >= outputSize_) return 0;
    const std::int64_t wanted = std::min<std::int64_t>(count, outputSize_ - pos);
    std::int64_t done = 0;
    while (done < wanted) {
        const std::int64_t current = pos + done;
        auto region = std::lower_bound(regions_.begin(), regions_.end(), current,
            [](const Region& item, std::int64_t offset) {
                return item.outputOffset + item.length <= offset;
            });
        if (region == regions_.end() || current < region->outputOffset) break;
        const std::int64_t within = current - region->outputOffset;
        const int amount = static_cast<int>(std::min<std::int64_t>(
            wanted - done, region->length - within));
        if (region->kind == ChunkKind::Raw) {
            const int actual = source_->read(region->sourceOffset + within, dst + done, amount);
            if (actual <= 0 || actual > amount) break;
            done += actual;
            continue;
        }
        if (region->kind == ChunkKind::Zero) {
            std::memset(dst + done, 0, static_cast<std::size_t>(amount));
        } else {
            const std::uint8_t value[4] = {
                static_cast<std::uint8_t>(region->fillValue),
                static_cast<std::uint8_t>(region->fillValue >> 8),
                static_cast<std::uint8_t>(region->fillValue >> 16),
                static_cast<std::uint8_t>(region->fillValue >> 24)
            };
            for (int i = 0; i < amount; ++i)
                dst[done + i] = value[(within + i) & 3];
        }
        done += amount;
    }
    return static_cast<int>(done);
}

bool AndroidSparseStore::validate() const {
    const std::size_t bufferSize = 64 * 1024;
    std::vector<std::uint8_t> buffer(bufferSize, 0);
    std::uint32_t crc = 0xffffffffU;
    std::size_t checksumIndex = 0;
    std::int64_t position = 0;
    for (const Region& region : regions_) {
        while (checksumIndex < checksums_.size() &&
               checksums_[checksumIndex].outputOffset == position) {
            if ((crc ^ 0xffffffffU) != checksums_[checksumIndex].value) return false;
            ++checksumIndex;
        }
        std::int64_t remaining = region.length;
        std::int64_t within = 0;
        while (remaining > 0) {
            const int amount = static_cast<int>(std::min<std::int64_t>(remaining, buffer.size()));
            if (region.kind == ChunkKind::Raw) {
                if (!readExact(source_, region.sourceOffset + within, buffer.data(), amount))
                    return false;
            } else if (region.kind == ChunkKind::Zero) {
                std::memset(buffer.data(), 0, static_cast<std::size_t>(amount));
            } else {
                const std::uint8_t value[4] = {
                    static_cast<std::uint8_t>(region.fillValue),
                    static_cast<std::uint8_t>(region.fillValue >> 8),
                    static_cast<std::uint8_t>(region.fillValue >> 16),
                    static_cast<std::uint8_t>(region.fillValue >> 24)
                };
                for (int i = 0; i < amount; ++i)
                    buffer[static_cast<std::size_t>(i)] = value[(within + i) & 3];
            }
            crc = updateCrc(crc, buffer.data(), static_cast<std::size_t>(amount));
            remaining -= amount;
            within += amount;
            position += amount;
        }
    }
    while (checksumIndex < checksums_.size() &&
           checksums_[checksumIndex].outputOffset == position) {
        if ((crc ^ 0xffffffffU) != checksums_[checksumIndex].value) return false;
        ++checksumIndex;
    }
    return checksumIndex == checksums_.size() &&
        (imageChecksum_ == 0 || (crc ^ 0xffffffffU) == imageChecksum_);
}

}  // namespace fs
}  // namespace peare
