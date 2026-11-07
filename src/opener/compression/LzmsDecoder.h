#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace compression {

bool decompressLzmsBlock(const std::uint8_t* input, std::size_t inputSize,
                         std::size_t outputSize,
                         std::vector<std::uint8_t>* output,
                         std::string* error = nullptr);

}
}
