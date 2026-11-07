#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decodeWavPackZip(const std::uint8_t* data, std::size_t size,
                      std::size_t expectedSize, std::vector<std::uint8_t>* output);

} // namespace compression
} // namespace peare
