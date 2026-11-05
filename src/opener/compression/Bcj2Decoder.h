#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decodeBcj2(const std::array<const std::uint8_t*, 4>& streams,
                const std::array<std::size_t, 4>& sizes,
                std::size_t unpackedSize, std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare
