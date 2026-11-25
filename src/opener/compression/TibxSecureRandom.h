#pragma once

#include <cstddef>
#include <cstdint>

namespace peare {
namespace fs {

bool tibxSecureRandom(std::uint8_t* data, std::size_t size);

} // namespace fs
} // namespace peare
