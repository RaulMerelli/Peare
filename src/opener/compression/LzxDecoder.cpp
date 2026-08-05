#include "peare/lzx_frontends.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

const unsigned kMinWindowOrder = 15;
const unsigned kMaxWindowOrder = 21;
const unsigned kFrameSize = 32768;
const unsigned kLiteralCount = 256;
const unsigned kPrimaryLengths = 7;
const unsigned kLengthSymbols = 249;
const unsigned kPretreeSymbols = 20;
const unsigned kAlignedSymbols = 8;
const unsigned kMinimumMatch = 2;
const std::int32_t kWimE8FileSize = 12000000;

enum BlockType {
    BlockVerbatim = 1,
    BlockAligned = 2,
    BlockUncompressed = 3
};

enum StreamKind {
    StreamWim,
    StreamClassic
};

const std::uint32_t kPositionBase[] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192,
    12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072,
    196608, 262144, 393216, 524288, 655360, 786432, 917504,
    1048576, 1179648, 1310720, 1441792, 1572864, 1703936,
    1835008, 1966080, 2097152
};

bool validWindowSize(std::size_t value)
{
    if (value < (std::size_t(1) << kMinWindowOrder) ||
        value > (std::size_t(1) << kMaxWindowOrder))
        return false;
    return (value & (value - 1)) == 0;
}

unsigned positionSlotCount(std::size_t windowSize)
{
    for (unsigned i = 0; i < sizeof(kPositionBase) / sizeof(kPositionBase[0]); ++i) {
        if (kPositionBase[i] == windowSize)
            return i;
    }
    return 0;
}

unsigned extraBitCount(unsigned slot)
{
    if (slot < 4)
        return 0;
    if (slot >= 38)
        return 17;
    return (slot >> 1) - 1;
}

class WordBitReader {
public:
    WordBitReader(const void *data, std::size_t size)
        : bytes_(static_cast<const std::uint8_t *>(data)), size_(size), pos_(0),
          word_(0), bitsLeft_(0)
    {
    }

    bool readBits(unsigned count, std::uint32_t &value)
    {
        if (count > 32)
            return false;
        value = 0;
        while (count != 0) {
            if (bitsLeft_ == 0 && !loadWord())
                return false;
            const unsigned take = std::min(count, bitsLeft_);
            const unsigned shift = bitsLeft_ - take;
            const std::uint32_t mask = take == 32 ? 0xffffffffu : ((std::uint32_t(1) << take) - 1u);
            value = (value << take) | ((word_ >> shift) & mask);
            bitsLeft_ -= take;
            count -= take;
        }
        return true;
    }

    bool readBit(std::uint32_t &value) { return readBits(1, value); }

    void alignToWord()
    {
        bitsLeft_ = 0;
        word_ = 0;
    }

    bool prepareUncompressedHeader()
    {
        if (bitsLeft_ == 0) {
            if (size_ - pos_ < 2)
                return false;
            pos_ += 2;
        } else {
            alignToWord();
        }
        return true;
    }

    bool readRaw(void *target, std::size_t count)
    {
        if (bitsLeft_ != 0 || count > size_ - pos_)
            return false;
        std::memcpy(target, bytes_ + pos_, count);
        pos_ += count;
        return true;
    }

    bool skipRaw(std::size_t count)
    {
        if (bitsLeft_ != 0 || count > size_ - pos_)
            return false;
        pos_ += count;
        return true;
    }

    std::size_t bytesRemaining() const
    {
        return size_ - pos_;
    }

private:
    bool loadWord()
    {
        if (size_ - pos_ < 2)
            return false;
        word_ = std::uint16_t(bytes_[pos_]) |
                (std::uint16_t(bytes_[pos_ + 1]) << 8);
        pos_ += 2;
        bitsLeft_ = 16;
        return true;
    }

    const std::uint8_t *bytes_;
    std::size_t size_;
    std::size_t pos_;
    std::uint32_t word_;
    unsigned bitsLeft_;
};

class CanonicalTree {
public:
    CanonicalTree() : maxLength_(0), empty_(true) {}

    bool build(const std::vector<std::uint8_t> &lengths, unsigned allowedMax)
    {
        counts_.assign(allowedMax + 1, 0);
        firstCode_.assign(allowedMax + 1, 0);
        firstSymbol_.assign(allowedMax + 1, 0);
        symbols_.clear();
        maxLength_ = 0;
        empty_ = true;

        for (std::size_t i = 0; i < lengths.size(); ++i) {
            const unsigned len = lengths[i];
            if (len > allowedMax)
                return false;
            if (len != 0) {
                ++counts_[len];
                maxLength_ = std::max(maxLength_, len);
                empty_ = false;
            }
        }
        if (empty_)
            return true;

        int available = 1;
        for (unsigned len = 1; len <= allowedMax; ++len) {
            available = (available << 1) - int(counts_[len]);
            if (available < 0)
                return false;
        }

        std::uint32_t code = 0;
        unsigned symbolBase = 0;
        for (unsigned len = 1; len <= allowedMax; ++len) {
            code = (code + counts_[len - 1]) << 1;
            firstCode_[len] = code;
            firstSymbol_[len] = symbolBase;
            symbolBase += counts_[len];
        }

        symbols_.resize(symbolBase);
        std::vector<unsigned> next = firstSymbol_;
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned len = lengths[symbol];
            if (len != 0)
                symbols_[next[len]++] = symbol;
        }
        return true;
    }

    bool decode(WordBitReader &reader, unsigned &symbol) const
    {
        if (empty_)
            return false;
        std::uint32_t code = 0;
        for (unsigned len = 1; len <= maxLength_; ++len) {
            std::uint32_t bit;
            if (!reader.readBit(bit))
                return false;
            code = (code << 1) | bit;
            const std::uint32_t first = firstCode_[len];
            if (code >= first) {
                const std::uint32_t index = code - first;
                if (index < counts_[len]) {
                    symbol = symbols_[firstSymbol_[len] + index];
                    return true;
                }
            }
        }
        return false;
    }

    bool empty() const { return empty_; }

private:
    std::vector<unsigned> counts_;
    std::vector<std::uint32_t> firstCode_;
    std::vector<unsigned> firstSymbol_;
    std::vector<unsigned> symbols_;
    unsigned maxLength_;
    bool empty_;
};

std::uint32_t readLe32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void writeLe32(std::uint8_t *p, std::uint32_t value)
{
    p[0] = std::uint8_t(value);
    p[1] = std::uint8_t(value >> 8);
    p[2] = std::uint8_t(value >> 16);
    p[3] = std::uint8_t(value >> 24);
}

void undoClassicE8(std::uint8_t *data, std::size_t size, std::int32_t fileSize)
{
    if (fileSize <= 0 || size <= 10)
        return;
    std::size_t pos = 0;
    const std::size_t limit = size - 10;
    while (pos < limit) {
        if (data[pos] != 0xe8) {
            ++pos;
            continue;
        }
        const std::int32_t absolute = static_cast<std::int32_t>(readLe32(data + pos + 1));
        const std::int32_t current = static_cast<std::int32_t>(pos);
        if (absolute >= -current && absolute < fileSize) {
            const std::int32_t relative = absolute >= 0 ? absolute - current : absolute + fileSize;
            writeLe32(data + pos + 1, static_cast<std::uint32_t>(relative));
        }
        pos += 5;
    }
}

void undoWimE8(std::uint8_t *data, std::size_t size)
{
    if (size <= 10)
        return;
    std::size_t pos = 0;
    const std::size_t limit = size - 10;
    while (pos < limit) {
        if (data[pos] != 0xe8) {
            ++pos;
            continue;
        }
        const std::int32_t absolute = static_cast<std::int32_t>(readLe32(data + pos + 1));
        const std::int32_t current = static_cast<std::int32_t>(pos);
        bool translate = false;
        std::int32_t relative = 0;
        if (absolute >= 0) {
            if (absolute < kWimE8FileSize) {
                relative = absolute - current;
                translate = true;
            }
        } else if (absolute >= -current) {
            relative = absolute + kWimE8FileSize;
            translate = true;
        }
        if (translate)
            writeLe32(data + pos + 1, static_cast<std::uint32_t>(relative));
        pos += 5;
    }
}

class LzxDecoder {
public:
    LzxDecoder(std::size_t windowSize, StreamKind kind)
        : windowSize_(windowSize), kind_(kind), slotCount_(positionSlotCount(windowSize)),
          mainLengths_(kLiteralCount + slotCount_ * 8, 0),
          lengthLengths_(kLengthSymbols, 0), e8Used_(false), classicE8FileSize_(0)
    {
        recent_[0] = recent_[1] = recent_[2] = 1;
    }

    bool decode(const void *input, std::size_t inputSize, void *output, std::size_t outputSize)
    {
        if (!input || !output || inputSize == 0 || outputSize == 0 || slotCount_ == 0)
            return false;
        if (kind_ == StreamWim && outputSize > windowSize_)
            return false;

        std::fill(mainLengths_.begin(), mainLengths_.end(), 0);
        std::fill(lengthLengths_.begin(), lengthLengths_.end(), 0);
        recent_[0] = recent_[1] = recent_[2] = 1;
        e8Used_ = false;
        classicE8FileSize_ = 0;

        WordBitReader reader(input, inputSize);
        if (kind_ == StreamClassic) {
            std::uint32_t enabled;
            if (!reader.readBit(enabled))
                return false;
            if (enabled) {
                std::uint32_t high, low;
                if (!reader.readBits(16, high) || !reader.readBits(16, low))
                    return false;
                classicE8FileSize_ = (high << 16) | low;
                e8Used_ = classicE8FileSize_ != 0;
            }
        }

        std::uint8_t *out = static_cast<std::uint8_t *>(output);
        std::size_t outPos = 0;
        while (outPos < outputSize) {
            unsigned type;
            std::size_t blockSize;
            if (!readBlockHeader(reader, type, blockSize))
                return false;
            if (blockSize == 0 || blockSize > outputSize - outPos)
                return false;

            if (type == BlockUncompressed) {
                if (!decodeUncompressed(reader, out, outPos, blockSize))
                    return false;
                if (kind_ == StreamWim)
                    e8Used_ = true;
            } else {
                if (!decodeCompressed(reader, type, out, outPos, blockSize, outputSize))
                    return false;
                if (kind_ == StreamWim && mainLengths_[0xe8] != 0)
                    e8Used_ = true;
            }
        }

        if (e8Used_) {
            if (kind_ == StreamClassic)
                undoClassicE8(out, outputSize, static_cast<std::int32_t>(classicE8FileSize_));
            else
                undoWimE8(out, outputSize);
        }
        return true;
    }

private:
    bool readCodeLengths(WordBitReader &reader, std::vector<std::uint8_t> &lengths,
                         std::size_t begin, std::size_t count)
    {
        std::vector<std::uint8_t> pretreeLengths(kPretreeSymbols, 0);
        for (unsigned i = 0; i < kPretreeSymbols; ++i) {
            std::uint32_t value;
            if (!reader.readBits(4, value))
                return false;
            pretreeLengths[i] = static_cast<std::uint8_t>(value);
        }
        CanonicalTree pretree;
        if (!pretree.build(pretreeLengths, 16) || pretree.empty())
            return false;

        const std::size_t end = begin + count;
        std::size_t index = begin;
        while (index < end) {
            unsigned code;
            if (!pretree.decode(reader, code))
                return false;
            if (code <= 16) {
                int value = int(lengths[index]) - int(code);
                if (value < 0)
                    value += 17;
                lengths[index++] = static_cast<std::uint8_t>(value);
            } else if (code == 17 || code == 18) {
                std::uint32_t extra;
                const unsigned bits = code == 17 ? 4 : 5;
                const unsigned base = code == 17 ? 4 : 20;
                if (!reader.readBits(bits, extra))
                    return false;
                const std::size_t run = base + extra;
                if (run > end - index)
                    return false;
                std::fill(lengths.begin() + index, lengths.begin() + index + run, 0);
                index += run;
            } else if (code == 19) {
                std::uint32_t extra;
                unsigned delta;
                if (!reader.readBit(extra) || !pretree.decode(reader, delta))
                    return false;
                const std::size_t run = 4 + extra;
                if (run > end - index || delta > 16)
                    return false;
                int value = int(lengths[index]) - int(delta);
                if (value < 0)
                    value += 17;
                std::fill(lengths.begin() + index, lengths.begin() + index + run,
                          static_cast<std::uint8_t>(value));
                index += run;
            } else {
                return false;
            }
        }
        return true;
    }

    bool readBlockHeader(WordBitReader &reader, unsigned &type, std::size_t &blockSize)
    {
        std::uint32_t value;
        if (!reader.readBits(3, value))
            return false;
        type = value;

        if (kind_ == StreamClassic) {
            std::uint32_t a, b, c;
            if (!reader.readBits(8, a) || !reader.readBits(8, b) || !reader.readBits(8, c))
                return false;
            blockSize = (std::size_t(a) << 16) | (std::size_t(b) << 8) | c;
        } else {
            if (!reader.readBit(value))
                return false;
            if (value != 0) {
                blockSize = kFrameSize;
            } else {
                std::uint32_t a, b, c = 0;
                if (!reader.readBits(8, a) || !reader.readBits(8, b))
                    return false;
                blockSize = (std::size_t(a) << 8) | b;
                if (windowSize_ >= 65536) {
                    if (!reader.readBits(8, c))
                        return false;
                    blockSize = (blockSize << 8) | c;
                }
            }
        }

        if (type == BlockAligned) {
            alignedLengths_.assign(kAlignedSymbols, 0);
            for (unsigned i = 0; i < kAlignedSymbols; ++i) {
                if (!reader.readBits(3, value))
                    return false;
                alignedLengths_[i] = static_cast<std::uint8_t>(value);
            }
            if (!alignedTree_.build(alignedLengths_, 8) || alignedTree_.empty())
                return false;
        }

        if (type == BlockVerbatim || type == BlockAligned) {
            if (!readCodeLengths(reader, mainLengths_, 0, kLiteralCount) ||
                !readCodeLengths(reader, mainLengths_, kLiteralCount,
                                 mainLengths_.size() - kLiteralCount) ||
                !mainTree_.build(mainLengths_, 16) || mainTree_.empty())
                return false;
            if (!readCodeLengths(reader, lengthLengths_, 0, lengthLengths_.size()) ||
                !lengthTree_.build(lengthLengths_, 16))
                return false;
            return true;
        }

        if (type == BlockUncompressed) {
            if (!reader.prepareUncompressedHeader())
                return false;
            std::uint8_t queueBytes[12];
            if (!reader.readRaw(queueBytes, sizeof(queueBytes)))
                return false;
            recent_[0] = readLe32(queueBytes);
            recent_[1] = readLe32(queueBytes + 4);
            recent_[2] = readLe32(queueBytes + 8);
            return recent_[0] != 0 && recent_[1] != 0 && recent_[2] != 0;
        }
        return false;
    }

    bool decodeUncompressed(WordBitReader &reader, std::uint8_t *out,
                            std::size_t &outPos, std::size_t blockSize)
    {
        if (!reader.readRaw(out + outPos, blockSize))
            return false;
        outPos += blockSize;
        if ((blockSize & 1) != 0 && reader.bytesRemaining() != 0 && !reader.skipRaw(1))
            return false;
        return true;
    }

    bool decodeCompressed(WordBitReader &reader, unsigned type, std::uint8_t *out,
                          std::size_t &outPos, std::size_t blockSize,
                          std::size_t totalOutput)
    {
        const std::size_t blockEnd = outPos + blockSize;
        while (outPos < blockEnd) {
            unsigned symbol;
            if (!mainTree_.decode(reader, symbol))
                return false;
            if (symbol < kLiteralCount) {
                out[outPos++] = static_cast<std::uint8_t>(symbol);
            } else {
                symbol -= kLiteralCount;
                const unsigned lengthHeader = symbol & kPrimaryLengths;
                const unsigned slot = symbol >> 3;
                if (slot >= slotCount_)
                    return false;

                std::size_t matchLength = kMinimumMatch + lengthHeader;
                if (lengthHeader == kPrimaryLengths) {
                    unsigned extraLength;
                    if (lengthTree_.empty() || !lengthTree_.decode(reader, extraLength))
                        return false;
                    matchLength += extraLength;
                }
                if (matchLength > blockEnd - outPos)
                    return false;

                std::size_t matchOffset;
                if (slot <= 2) {
                    matchOffset = recent_[slot];
                    recent_[slot] = recent_[0];
                    recent_[0] = static_cast<std::uint32_t>(matchOffset);
                } else {
                    const unsigned extraBits = extraBitCount(slot);
                    std::uint32_t high = 0;
                    unsigned low = 0;
                    if (type == BlockAligned && extraBits >= 3) {
                        if (!reader.readBits(extraBits - 3, high) ||
                            !alignedTree_.decode(reader, low))
                            return false;
                        high <<= 3;
                    } else if (!reader.readBits(extraBits, high)) {
                        return false;
                    }
                    matchOffset = std::size_t(kPositionBase[slot]) + high + low - 2;
                    recent_[2] = recent_[1];
                    recent_[1] = recent_[0];
                    recent_[0] = static_cast<std::uint32_t>(matchOffset);
                }

                if (matchOffset == 0 || matchOffset > windowSize_ || matchOffset > outPos)
                    return false;
                for (std::size_t i = 0; i < matchLength; ++i)
                    out[outPos + i] = out[outPos + i - matchOffset];
                outPos += matchLength;
            }

            if (outPos < totalOutput && (outPos % kFrameSize) == 0)
                reader.alignToWord();
        }
        return true;
    }

    std::size_t windowSize_;
    StreamKind kind_;
    unsigned slotCount_;
    std::vector<std::uint8_t> mainLengths_;
    std::vector<std::uint8_t> lengthLengths_;
    std::vector<std::uint8_t> alignedLengths_;
    CanonicalTree mainTree_;
    CanonicalTree lengthTree_;
    CanonicalTree alignedTree_;
    std::uint32_t recent_[3];
    bool e8Used_;
    std::uint32_t classicE8FileSize_;
};

peare_lzx_status decodeWith(LzxDecoder &decoder, const void *compressedData,
                            std::size_t compressedSize, void *uncompressedData,
                            std::size_t uncompressedSize)
{
    try {
        return decoder.decode(compressedData, compressedSize, uncompressedData, uncompressedSize)
                   ? PEARE_LZX_OK
                   : PEARE_LZX_CORRUPT_STREAM;
    } catch (const std::bad_alloc &) {
        return PEARE_LZX_OUT_OF_MEMORY;
    } catch (...) {
        return PEARE_LZX_CORRUPT_STREAM;
    }
}

} // namespace

struct peare_lzx_wim_decoder {
    explicit peare_lzx_wim_decoder(std::size_t size) : engine(size, StreamWim) {}
    LzxDecoder engine;
};

struct peare_lzx_xex_decoder {
    peare_lzx_xex_decoder(std::size_t window, std::size_t expected)
        : engine(window, StreamClassic), expectedSize(expected) {}
    LzxDecoder engine;
    std::size_t expectedSize;
};

struct peare_lzx_cab_decoder {
    explicit peare_lzx_cab_decoder(std::size_t window) : engine(window, StreamClassic) {}
    LzxDecoder engine;
};

extern "C" peare_lzx_status peare_lzx_wim_create(
    std::size_t maxChunkSize, peare_lzx_wim_decoder **decoder)
{
    if (!decoder || !validWindowSize(maxChunkSize))
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = 0;
    try {
        *decoder = new peare_lzx_wim_decoder(maxChunkSize);
        return PEARE_LZX_OK;
    } catch (const std::bad_alloc &) {
        return PEARE_LZX_OUT_OF_MEMORY;
    }
}

extern "C" peare_lzx_status peare_lzx_wim_decompress(
    peare_lzx_wim_decoder *decoder, const void *compressedData,
    std::size_t compressedSize, void *uncompressedData,
    std::size_t uncompressedSize)
{
    if (!decoder || !compressedData || !uncompressedData || compressedSize == 0 ||
        uncompressedSize == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    return decodeWith(decoder->engine, compressedData, compressedSize,
                      uncompressedData, uncompressedSize);
}

extern "C" void peare_lzx_wim_destroy(peare_lzx_wim_decoder *decoder)
{
    delete decoder;
}

extern "C" peare_lzx_status peare_lzx_xex_create(
    std::size_t windowSize, std::size_t expectedOutputSize,
    peare_lzx_xex_decoder **decoder)
{
    if (!decoder || !validWindowSize(windowSize) || expectedOutputSize == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = 0;
    try {
        *decoder = new peare_lzx_xex_decoder(windowSize, expectedOutputSize);
        return PEARE_LZX_OK;
    } catch (const std::bad_alloc &) {
        return PEARE_LZX_OUT_OF_MEMORY;
    }
}

extern "C" peare_lzx_status peare_lzx_xex_decompress(
    peare_lzx_xex_decoder *decoder, const void *compressedStream,
    std::size_t compressedSize, void *uncompressedImage, std::size_t imageSize)
{
    if (!decoder || !compressedStream || !uncompressedImage || compressedSize == 0 ||
        imageSize == 0 || imageSize != decoder->expectedSize)
        return PEARE_LZX_INVALID_ARGUMENT;
    return decodeWith(decoder->engine, compressedStream, compressedSize,
                      uncompressedImage, imageSize);
}

extern "C" void peare_lzx_xex_destroy(peare_lzx_xex_decoder *decoder)
{
    delete decoder;
}

extern "C" peare_lzx_status peare_lzx_cab_create(
    std::size_t windowSize, peare_lzx_cab_decoder **decoder)
{
    if (!decoder || !validWindowSize(windowSize))
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = 0;
    try {
        *decoder = new peare_lzx_cab_decoder(windowSize);
        return PEARE_LZX_OK;
    } catch (const std::bad_alloc &) {
        return PEARE_LZX_OUT_OF_MEMORY;
    }
}

extern "C" peare_lzx_status peare_lzx_cab_decompress(
    peare_lzx_cab_decoder *decoder, const void *compressedStream,
    std::size_t compressedSize, void *uncompressedData,
    std::size_t uncompressedSize)
{
    if (!decoder || !compressedStream || !uncompressedData || compressedSize == 0 ||
        uncompressedSize == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    return decodeWith(decoder->engine, compressedStream, compressedSize,
                      uncompressedData, uncompressedSize);
}

extern "C" void peare_lzx_cab_destroy(peare_lzx_cab_decoder *decoder)
{
    delete decoder;
}
