#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decodeLzmaRaw(const std::uint8_t* packed, std::size_t packedSize,
                   const std::uint8_t* properties, std::size_t propertiesSize,
                   std::size_t unpackedSize, std::vector<std::uint8_t>* output);

bool decodeLzma2Raw(const std::uint8_t* packed, std::size_t packedSize,
                    const std::uint8_t* properties, std::size_t propertiesSize,
                    std::size_t unpackedSize, std::vector<std::uint8_t>* output);

bool decodePpmd7Raw(const std::uint8_t* packed, std::size_t packedSize,
                    const std::uint8_t* properties, std::size_t propertiesSize,
                    std::size_t unpackedSize, std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare
