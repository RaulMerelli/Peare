#pragma once

#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decodePackMp3(const std::uint8_t* input, std::size_t inputSize,
                   std::size_t outputLimit, std::vector<std::uint8_t>* output);

}
}
