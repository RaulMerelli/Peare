#include "LzmsDecoder.h"

#include <limits>

extern "C" int peare_lzms_decompress_buffer(const void* input,
                                             std::size_t inputSize,
                                             void* output,
                                             std::size_t outputSize);

namespace peare {
namespace compression {

bool decompressLzmsBlock(const std::uint8_t* input, std::size_t inputSize,
                         std::size_t outputSize,
                         std::vector<std::uint8_t>* output,
                         std::string* error)
{
    if (!output || (!input && inputSize != 0) || inputSize < 4 || (inputSize & 1)) {
        if (error) *error = "Invalid LZMS block";
        return false;
    }
    if (outputSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        if (error) *error = "LZMS output exceeds the supported buffer size";
        return false;
    }

    output->assign(outputSize, 0);
    const int result = peare_lzms_decompress_buffer(
        input, inputSize, output->data(), output->size());
    if (result != 0) {
        output->clear();
        if (error) *error = "Invalid LZMS data";
        return false;
    }
    return true;
}

}
}
