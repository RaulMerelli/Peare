#include "Lz4Block.h"

#include <limits>

namespace peare {

bool appendLz4Block(const std::uint8_t* input, std::size_t inputSize,
                    std::vector<std::uint8_t>* output, std::size_t outputSize) {
    if (!output || (inputSize && !input)) return false;
    if (outputSize > std::numeric_limits<std::size_t>::max() - output->size()) return false;
    const std::size_t end = output->size() + outputSize;
    output->reserve(end);
    std::size_t in = 0;
    while (in < inputSize) {
        const std::uint8_t token = input[in++];
        std::size_t literalLength = token >> 4;
        if (literalLength == 15) {
            std::uint8_t extension = 255;
            while (extension == 255) {
                if (in >= inputSize) return false;
                extension = input[in++];
                if (literalLength > outputSize || extension > outputSize - literalLength)
                    return false;
                literalLength += extension;
            }
        }
        if (literalLength > inputSize - in || literalLength > end - output->size())
            return false;
        output->insert(output->end(), input + in, input + in + literalLength);
        in += literalLength;
        if (in == inputSize) break;
        if (inputSize - in < 2) return false;
        const std::size_t distance = std::size_t(input[in]) |
                                     (std::size_t(input[in + 1]) << 8);
        in += 2;
        if (!distance || distance > output->size()) return false;
        std::size_t matchLength = (token & 0x0fu) + 4u;
        if ((token & 0x0fu) == 15u) {
            std::uint8_t extension = 255;
            while (extension == 255) {
                if (in >= inputSize) return false;
                extension = input[in++];
                if (matchLength > outputSize || extension > outputSize - matchLength)
                    return false;
                matchLength += extension;
            }
        }
        if (matchLength > end - output->size()) return false;
        for (std::size_t i = 0; i < matchLength; ++i)
            output->push_back((*output)[output->size() - distance]);
    }
    return output->size() == end;
}

bool decompressLz4Block(const std::uint8_t* input, std::size_t inputSize,
                        std::vector<std::uint8_t>* output, std::size_t outputSize) {
    if (!output || (inputSize && !input)) return false;
    output->clear();
    return appendLz4Block(input, inputSize, output, outputSize);
}

}  // namespace peare
