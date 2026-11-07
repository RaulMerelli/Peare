#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decodeWinZipJpeg(const std::uint8_t* packed, std::size_t packedSize,
                      std::size_t unpackedSize, std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare
