#pragma once

// Block decompressor abstraction, ported faithfully from DiscUtils'
// DiscUtils.Core/Compression/IBlockDecompressor.cs:
//
//   bool TryDecompress(ReadOnlySpan<byte> source, Span<byte> decompressed,
//                      out int decompressedSize)
//
// DiscUtils selects a codec per compressed resource (WIM: LZX or XPRESS) behind
// this single interface. We keep the interface (the DiscUtils shape) and port
// every codec behind the same interface. LZX is implemented by Peare's internal
// decoder and exposed through the peare_lzx_wim_* frontend.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "peare/lzx_frontends.h"

namespace peare {
namespace fs {

// == DiscUtils.Compression.IBlockDecompressor (decompress side).
class IBlockDecompressor {
public:
    virtual ~IBlockDecompressor() {}

    // Decompress `src` (srcLen bytes) into `dst` (capacity dstCap, which is the
    // expected decompressed size for fixed-block formats such as WIM chunks).
    // Returns true on success and sets outSize to the produced byte count.
    virtual bool tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCap,
                               std::size_t& outSize) = 0;
};

typedef std::shared_ptr<IBlockDecompressor> BlockDecompressorPtr;

// LZX decompressor for independent WIM chunks. The destination capacity is the
// expected chunk size, so the frontend can validate the exact result.
class LzxWimDecompressor : public IBlockDecompressor {
public:
    explicit LzxWimDecompressor(std::size_t maxChunkSize) {
        if (peare_lzx_wim_create(maxChunkSize, &decoder_) != PEARE_LZX_OK)
            decoder_ = nullptr;
    }
    ~LzxWimDecompressor() override {
        if (decoder_) peare_lzx_wim_destroy(decoder_);
    }

    bool tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                       std::uint8_t* dst, std::size_t dstCap,
                       std::size_t& outSize) override {
        if (!decoder_) return false;
        const peare_lzx_status s =
            peare_lzx_wim_decompress(decoder_, src, srcLen, dst, dstCap);
        if (s != PEARE_LZX_OK) return false;
        outSize = dstCap;  // WIM chunk decompressed length equals the chunk size
        return true;
    }

private:
    peare_lzx_wim_decoder* decoder_ = nullptr;

    LzxWimDecompressor(const LzxWimDecompressor&);
    LzxWimDecompressor& operator=(const LzxWimDecompressor&);
};

// XPRESS LZ77+Huffman decoder used by compressed WIM resources.
// The implementation is maintained locally in XpressDecoder.cpp.
class XpressHuffmanDecompressor : public IBlockDecompressor {
public:
    XpressHuffmanDecompressor();
    bool tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                       std::uint8_t* dst, std::size_t dstCap,
                       std::size_t& outSize) override;
};

}  // namespace fs
}  // namespace peare
