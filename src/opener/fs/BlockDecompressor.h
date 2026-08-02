#pragma once

// Block decompressor abstraction, ported faithfully from DiscUtils'
// DiscUtils.Core/Compression/IBlockDecompressor.cs:
//
//   bool TryDecompress(ReadOnlySpan<byte> source, Span<byte> decompressed,
//                      out int decompressedSize)
//
// DiscUtils selects a codec per compressed resource (WIM: LZX or XPRESS) behind
// this single interface. We keep the interface (the DiscUtils shape) and port
// every codec from DiscUtils EXCEPT LZX: LZX is provided by our existing wimlib
// engine (peare_lzx_wim_*), wrapped as an IBlockDecompressor so the WIM reader
// selects it through the same interface DiscUtils uses.

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

// LZX decompressor for WIM chunks. This is the ONE codec we do not port from
// DiscUtils: it adapts our wimlib-based engine to the DiscUtils interface. WIM
// chunks are independent and their decompressed size is known (the chunk size),
// so it maps directly onto peare_lzx_wim_decompress.
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

// XPRESS (Huffman) decompressor, ported from DiscUtils.Core/Compression/
// XpressHuffman.cs (+ XpressBitStream / XpressLz77 / Huffman helpers). Declared
// here; implemented in XpressHuffman.cpp.
class XpressHuffmanDecompressor : public IBlockDecompressor {
public:
    XpressHuffmanDecompressor();
    bool tryDecompress(const std::uint8_t* src, std::size_t srcLen,
                       std::uint8_t* dst, std::size_t dstCap,
                       std::size_t& outSize) override;
};

}  // namespace fs
}  // namespace peare
