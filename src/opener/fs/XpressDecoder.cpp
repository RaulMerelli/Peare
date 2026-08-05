#include "BlockDecompressor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace peare {
namespace fs {
namespace {

constexpr std::size_t kAlphabetSize = 512;
constexpr unsigned kLongestCode = 15;
constexpr std::size_t kLengthTableBytes = kAlphabetSize / 2;
constexpr std::size_t kLookupSize = std::size_t{1} << kLongestCode;
constexpr std::size_t kMaximumBlockOutput = 65536;

std::uint16_t readLittle16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t readLittle32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

class CanonicalLookup {
public:
    bool initialize(const std::uint8_t* packedLengths)
    {
        lengths_.fill(0);
        table_.fill(0);

        for (std::size_t symbol = 0; symbol < kAlphabetSize; ++symbol) {
            const std::uint8_t packed = packedLengths[symbol / 2];
            lengths_[symbol] = static_cast<std::uint8_t>(
                (symbol & 1u) == 0 ? packed & 0x0fu : packed >> 4);
        }

        std::size_t cursor = 0;
        for (unsigned bitLength = 1; bitLength <= kLongestCode; ++bitLength) {
            const std::size_t repetitions = std::size_t{1} << (kLongestCode - bitLength);
            for (std::size_t symbol = 0; symbol < kAlphabetSize; ++symbol) {
                if (lengths_[symbol] != bitLength) continue;
                if (repetitions > table_.size() - cursor) return false;
                std::fill_n(table_.begin() + static_cast<std::ptrdiff_t>(cursor),
                            repetitions, static_cast<std::uint16_t>(symbol));
                cursor += repetitions;
            }
        }
        return cursor == table_.size();
    }

    std::uint16_t symbolFor(std::uint16_t prefix) const { return table_[prefix]; }
    unsigned lengthOf(std::uint16_t symbol) const { return lengths_[symbol]; }

private:
    std::array<std::uint8_t, kAlphabetSize> lengths_{};
    std::array<std::uint16_t, kLookupSize> table_{};
};

class InterleavedInput {
public:
    InterleavedInput(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size)
    {
    }

    bool begin(std::size_t start)
    {
        if (start > size_ || size_ - start < 4) return false;
        window_ = static_cast<std::uint32_t>(readLittle16(data_ + start)) << 16;
        window_ |= readLittle16(data_ + start + 2);
        cursor_ = start + 4;
        reserveBits_ = 16;
        valid_ = true;
        return true;
    }

    std::uint16_t top15() const
    {
        return static_cast<std::uint16_t>(window_ >> (32u - kLongestCode));
    }

    bool takeBits(unsigned count, std::uint32_t& value)
    {
        if (!valid_ || count > 16) return false;
        value = count == 0 ? 0 : window_ >> (32u - count);
        return discard(count);
    }

    bool discard(unsigned count)
    {
        if (!valid_ || count > 16) return false;
        if (count != 0) window_ <<= count;
        reserveBits_ -= static_cast<int>(count);

        if (reserveBits_ < 0) {
            if (cursor_ > size_ || size_ - cursor_ < 2) {
                valid_ = false;
                return false;
            }
            const unsigned shift = static_cast<unsigned>(-reserveBits_);
            window_ |= static_cast<std::uint32_t>(readLittle16(data_ + cursor_)) << shift;
            cursor_ += 2;
            reserveBits_ += 16;
        }
        return true;
    }

    bool readByte(std::uint8_t& value)
    {
        if (!valid_ || cursor_ >= size_) return false;
        value = data_[cursor_++];
        return true;
    }

    bool readUInt16(std::uint16_t& value)
    {
        if (!valid_ || cursor_ > size_ || size_ - cursor_ < 2) return false;
        value = readLittle16(data_ + cursor_);
        cursor_ += 2;
        return true;
    }

    bool readUInt32(std::uint32_t& value)
    {
        if (!valid_ || cursor_ > size_ || size_ - cursor_ < 4) return false;
        value = readLittle32(data_ + cursor_);
        cursor_ += 4;
        return true;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    std::uint32_t window_ = 0;
    int reserveBits_ = 0;
    bool valid_ = false;
};

bool decodeMatchLength(unsigned nibble, InterleavedInput& input, std::size_t& length)
{
    std::uint64_t encoded = nibble;
    if (nibble == 15) {
        std::uint8_t extension = 0;
        if (!input.readByte(extension)) return false;

        if (extension != 255) {
            encoded = 15u + extension;
        } else {
            std::uint16_t extended16 = 0;
            if (!input.readUInt16(extended16)) return false;
            if (extended16 == 0) {
                std::uint32_t extended32 = 0;
                if (!input.readUInt32(extended32)) return false;
                encoded = extended32;
            } else {
                encoded = extended16;
            }
            if (encoded < 15) return false;
        }
    }

    encoded += 3;
    if (encoded > std::numeric_limits<std::size_t>::max()) return false;
    length = static_cast<std::size_t>(encoded);
    return true;
}

bool reproduce(std::uint8_t* output, std::size_t capacity, std::size_t& position,
               std::size_t distance, std::size_t length)
{
    if (distance == 0 || distance > position || length > capacity - position) return false;
    for (std::size_t i = 0; i < length; ++i)
        output[position + i] = output[position - distance + i];
    position += length;
    return true;
}

} // namespace

XpressHuffmanDecompressor::XpressHuffmanDecompressor() = default;

bool XpressHuffmanDecompressor::tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                                              std::uint8_t* dst, std::size_t dstCap,
                                              std::size_t& outSize)
{
    outSize = 0;
    if (!src || !dst || dstCap == 0 || dstCap > kMaximumBlockOutput ||
        srcLen < kLengthTableBytes + 4)
        return false;

    CanonicalLookup codes;
    if (!codes.initialize(src)) return false;

    InterleavedInput input(src, srcLen);
    if (!input.begin(kLengthTableBytes)) return false;

    std::size_t position = 0;
    while (position < dstCap) {
        const std::uint16_t symbol = codes.symbolFor(input.top15());
        const unsigned symbolBits = codes.lengthOf(symbol);
        if (symbolBits == 0 || !input.discard(symbolBits)) {
            outSize = position;
            return false;
        }

        if (symbol < 256) {
            dst[position++] = static_cast<std::uint8_t>(symbol);
            continue;
        }

        const unsigned matchCode = symbol - 256u;
        const unsigned distanceBits = matchCode >> 4;
        std::size_t matchLength = 0;
        if (!decodeMatchLength(matchCode & 0x0fu, input, matchLength)) {
            outSize = position;
            return false;
        }

        std::uint32_t distanceSuffix = 0;
        if (!input.takeBits(distanceBits, distanceSuffix)) {
            outSize = position;
            return false;
        }
        const std::size_t distance = (std::size_t{1} << distanceBits) + distanceSuffix;

        if (!reproduce(dst, dstCap, position, distance, matchLength)) {
            outSize = position;
            return false;
        }
    }

    outSize = position;
    return true;
}

} // namespace fs
} // namespace peare
