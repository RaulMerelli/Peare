#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace writer {

bool encodeWinZipJpeg(const std::uint8_t* input, std::size_t inputSize,
                      std::vector<std::uint8_t>* output);

}  // namespace writer
}  // namespace peare
