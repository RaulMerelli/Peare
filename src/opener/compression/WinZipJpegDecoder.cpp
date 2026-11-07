#include "WinZipJpegDecoder.h"

extern "C" {
#include "third_party/winzipjpeg/Decompressor.h"
}

#include <algorithm>
#include <cstring>

namespace peare {
namespace compression {
namespace {

struct MemoryInput {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset;
};

std::size_t readInput(void* context, std::uint8_t* buffer, std::size_t count)
{
    auto* input = static_cast<MemoryInput*>(context);
    const std::size_t available = input->size - input->offset;
    const std::size_t actual = std::min(count, available);
    if (actual) {
        std::memcpy(buffer, input->data + input->offset, actual);
        input->offset += actual;
    }
    return actual;
}

bool appendOutput(std::vector<std::uint8_t>* output, const std::uint8_t* data,
                  std::size_t size, std::size_t limit)
{
    if (!size) return true;
    if (size > limit - output->size()) return false;
    try {
        output->insert(output->end(), data, data + size);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

bool decodeWinZipJpeg(const std::uint8_t* packed, std::size_t packedSize,
                      std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize))
        return false;

    MemoryInput input{packed, packedSize, 0};
    WinZipJPEGDecompressor* decoder = AllocWinZipJPEGDecompressor(readInput, &input);
    if (!decoder) return false;
    decoder->maxcompressedsize = packedSize;
    decoder->maxmetadatasize = std::min<std::size_t>(unpackedSize, 16u * 1024u * 1024u);

    std::vector<std::uint8_t> decoded;
    bool valid = ReadWinZipJPEGHeader(decoder) == WinZipJPEGNoError;
    std::uint8_t buffer[16 * 1024];
    while (valid) {
        const int error = ReadNextWinZipJPEGBundle(decoder);
        if (error != WinZipJPEGNoError ||
            !appendOutput(&decoded, WinZipJPEGBundleMetadataBytes(decoder),
                          WinZipJPEGBundleMetadataLength(decoder), unpackedSize)) {
            valid = false;
            break;
        }
        if (IsFinalWinZipJPEGBundle(decoder)) break;

        while (valid && AreMoreWinZipJPEGSlicesAvailable(decoder)) {
            if (ReadNextWinZipJPEGSlice(decoder) != WinZipJPEGNoError) {
                valid = false;
                break;
            }
            while (AreMoreWinZipJPEGBytesAvailable(decoder)) {
                const std::size_t count = EncodeWinZipJPEGBlocksToBuffer(
                    decoder, buffer, sizeof(buffer));
                if (!count || !appendOutput(&decoded, buffer, count, unpackedSize)) {
                    valid = false;
                    break;
                }
            }
        }
    }

    FreeWinZipJPEGDecompressor(decoder);
    if (!valid || input.offset != packedSize || decoded.size() != unpackedSize)
        return false;
    output->swap(decoded);
    return true;
}

}  // namespace compression
}  // namespace peare
