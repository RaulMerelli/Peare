#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {

bool decompressLz4Block(const std::uint8_t* input, std::size_t inputSize,
                        std::vector<std::uint8_t>* output, std::size_t outputSize);
bool appendLz4Block(const std::uint8_t* input, std::size_t inputSize,
                    std::vector<std::uint8_t>* output, std::size_t outputSize);

}  // namespace peare
