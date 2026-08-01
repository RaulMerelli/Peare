// XPRESS Huffman block decompressor, ported faithfully from
// DiscUtils.Core/Compression/XpressHuffman.cs (canonical-Huffman + LZ77 with the
// XPRESS symbol scheme). Behaviour — including the 16-bit little-endian bit
// buffer and the raw length-extension reads — mirrors the C# decoder exactly.

#include "BlockDecompressor.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace peare {
namespace fs {
namespace {

const int kSymbolCount = 512;
const int kMaxCodeLength = 15;
const int kFastBits = 10;

// 16-bit-word bit reader, MSB-first within the running buffer.
struct XpressBitReader {
    const std::uint8_t* src;
    std::size_t len;
    std::size_t rawPos;
    std::uint32_t bitBuffer;
    int bitsAvailable;

    XpressBitReader(const std::uint8_t* s, std::size_t l, std::size_t start)
        : src(s), len(l), rawPos(start), bitBuffer(0), bitsAvailable(0) {}

    bool ensureFilled() {
        if (bitsAvailable < 16) {
            if (rawPos + 1 >= len) return false;
            const std::uint16_t word =
                static_cast<std::uint16_t>(src[rawPos]) |
                (static_cast<std::uint16_t>(src[rawPos + 1]) << 8);
            rawPos += 2;
            bitBuffer = (bitBuffer << 16) | word;
            bitsAvailable += 16;
        }
        return true;
    }

    bool tryPeek(int count, std::uint32_t& value) {
        value = 0;
        if (count > 16) return false;
        if (!ensureFilled()) return false;
        if (bitsAvailable < count) return false;
        if (count == 0) return true;
        const std::uint32_t mask = (1u << count) - 1u;
        value = (bitBuffer >> (bitsAvailable - count)) & mask;
        return true;
    }

    bool tryConsume(int count) {
        if (count > 16) return false;
        if (!ensureFilled()) return false;
        if (bitsAvailable < count) return false;
        bitsAvailable -= count;
        return true;
    }

    bool tryReadBits(int count, std::uint32_t& value) {
        value = 0;
        if (count > 16) return false;
        if (!ensureFilled() || bitsAvailable < count) return false;
        bitsAvailable -= count;
        if (count == 0) return true;
        const std::uint32_t mask = (1u << count) - 1u;
        value = (bitBuffer >> bitsAvailable) & mask;
        return true;
    }

    // Length-extension bytes are read straight from the raw stream position,
    // matching DiscUtils' decoder.
    bool tryReadRawByte(std::uint8_t& value) {
        if (rawPos >= len) { value = 0; return false; }
        value = src[rawPos++];
        return true;
    }
    bool tryReadRawUInt16(std::uint16_t& value) {
        if (rawPos + 1 >= len) { value = 0; return false; }
        value = static_cast<std::uint16_t>(src[rawPos]) |
                (static_cast<std::uint16_t>(src[rawPos + 1]) << 8);
        rawPos += 2;
        return true;
    }
};

bool buildDecoder(const std::uint8_t* codeLengths,
                  std::uint16_t* sortedSymbols,
                  std::uint16_t* lengthCounts,
                  int* firstCode, int* firstSymbol, int* nextSymbol,
                  std::uint16_t* fastSymbol, std::uint8_t* fastLength) {
    std::memset(lengthCounts, 0, (kMaxCodeLength + 1) * sizeof(std::uint16_t));
    std::memset(firstCode, 0, (kMaxCodeLength + 1) * sizeof(int));
    std::memset(firstSymbol, 0, (kMaxCodeLength + 1) * sizeof(int));
    std::memset(nextSymbol, 0, (kMaxCodeLength + 1) * sizeof(int));
    std::memset(fastSymbol, 0, (1 << kFastBits) * sizeof(std::uint16_t));
    std::memset(fastLength, 0, (1 << kFastBits) * sizeof(std::uint8_t));

    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        const int l = codeLengths[symbol];
        if (static_cast<unsigned>(l) > static_cast<unsigned>(kMaxCodeLength)) return false;
        if (l != 0) ++lengthCounts[l];
    }

    int runningSymbolIndex = 0;
    int code = 0;
    for (int l = 1; l <= kMaxCodeLength; ++l) {
        code <<= 1;
        firstCode[l] = code;
        firstSymbol[l] = runningSymbolIndex;
        nextSymbol[l] = runningSymbolIndex;
        runningSymbolIndex += lengthCounts[l];
        code += lengthCounts[l];
        if (code > (1 << l)) return false;  // oversubscribed tree
    }

    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        const int l = codeLengths[symbol];
        if (l == 0) continue;
        sortedSymbols[nextSymbol[l]++] = static_cast<std::uint16_t>(symbol);
    }

    for (int l = 1; l <= kFastBits; ++l) {
        const int count = lengthCounts[l];
        if (count == 0) continue;
        const int baseCode = firstCode[l];
        const int symbolBase = firstSymbol[l];
        const int fill = 1 << (kFastBits - l);
        for (int i = 0; i < count; ++i) {
            const int codeValue = baseCode + i;
            const int start = codeValue << (kFastBits - l);
            const std::uint16_t symbol = sortedSymbols[symbolBase + i];
            for (int j = 0; j < fill; ++j) {
                fastSymbol[start + j] = symbol;
                fastLength[start + j] = static_cast<std::uint8_t>(l);
            }
        }
    }
    return true;
}

int decodeSymbol(XpressBitReader& reader,
                 const std::uint16_t* sortedSymbols, const std::uint16_t* lengthCounts,
                 const int* firstCode, const int* firstSymbol,
                 const std::uint16_t* fastSymbol, const std::uint8_t* fastLength) {
    std::uint32_t dummy;
    if (!reader.tryPeek(1, dummy)) return -1;

    std::uint32_t fastPeek;
    if (reader.tryPeek(kFastBits, fastPeek)) {
        const std::uint8_t l = fastLength[fastPeek];
        if (l != 0) {
            if (!reader.tryConsume(l)) return -1;
            return fastSymbol[fastPeek];
        }
    }

    for (int l = 1; l <= kMaxCodeLength; ++l) {
        std::uint32_t peek;
        if (!reader.tryPeek(l, peek)) return -1;
        const int delta = static_cast<int>(peek) - firstCode[l];
        const int count = lengthCounts[l];
        if (static_cast<unsigned>(delta) < static_cast<unsigned>(count)) {
            if (!reader.tryConsume(l)) return -1;
            return sortedSymbols[firstSymbol[l] + delta];
        }
    }
    return -1;
}

bool copyMatch(std::uint8_t* dst, std::size_t dstCap, std::size_t& dstPos,
               int distance, int length) {
    if (distance <= 0 || static_cast<std::size_t>(distance) > dstPos) return false;
    const std::size_t remaining = dstCap - dstPos;
    if (static_cast<std::size_t>(length) > remaining)
        length = static_cast<int>(remaining);
    std::size_t srcPos = dstPos - distance;
    for (int i = 0; i < length; ++i) dst[dstPos++] = dst[srcPos++];
    return true;
}

}  // namespace

XpressHuffmanDecompressor::XpressHuffmanDecompressor() {}

bool XpressHuffmanDecompressor::tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                                              std::uint8_t* dst, std::size_t dstCap,
                                              std::size_t& outSize) {
    outSize = 0;
    if (srcLen < 256) return false;

    std::uint8_t codeLengths[kSymbolCount];
    std::uint16_t sortedSymbols[kSymbolCount];
    std::uint16_t lengthCounts[kMaxCodeLength + 1];
    int firstCode[kMaxCodeLength + 1];
    int firstSymbol[kMaxCodeLength + 1];
    int nextSymbol[kMaxCodeLength + 1];
    std::uint16_t fastSymbol[1 << kFastBits];
    std::uint8_t fastLength[1 << kFastBits];

    std::size_t srcPos = 0;
    // First 256 bytes: two 4-bit code lengths each.
    for (int i = 0; i < kSymbolCount; i += 2) {
        const std::uint8_t b = src[srcPos++];
        codeLengths[i] = static_cast<std::uint8_t>(b & 0x0F);
        codeLengths[i + 1] = static_cast<std::uint8_t>(b >> 4);
    }

    if (!buildDecoder(codeLengths, sortedSymbols, lengthCounts, firstCode,
                      firstSymbol, nextSymbol, fastSymbol, fastLength))
        return false;

    XpressBitReader reader(src, srcLen, srcPos);
    std::size_t dstPos = 0;

    while (dstPos < dstCap) {
        const int symbol = decodeSymbol(reader, sortedSymbols, lengthCounts,
                                        firstCode, firstSymbol, fastSymbol, fastLength);
        if (symbol < 0) { outSize = dstPos; return false; }

        if (symbol < 256) {
            dst[dstPos++] = static_cast<std::uint8_t>(symbol);
            continue;
        }

        const int slot = symbol - 256;
        const int offsetBits = slot >> 4;
        int len = slot & 0x0F;

        std::uint32_t extraBits;
        if (!reader.tryReadBits(offsetBits, extraBits)) { outSize = dstPos; return false; }
        const int offset = ((1 << offsetBits) - 1) + static_cast<int>(extraBits);

        if (len == 15) {
            std::uint8_t b;
            if (!reader.tryReadRawByte(b)) { outSize = dstPos; return false; }
            if (b == 0xFF) {
                std::uint16_t extraLen;
                if (!reader.tryReadRawUInt16(extraLen)) { outSize = dstPos; return false; }
                len = extraLen;
            } else {
                len += b;
            }
        }
        len += 3;

        if (!copyMatch(dst, dstCap, dstPos, offset + 1, len)) { outSize = dstPos; return false; }
    }

    outSize = dstPos;
    return true;
}

}  // namespace fs
}  // namespace peare
