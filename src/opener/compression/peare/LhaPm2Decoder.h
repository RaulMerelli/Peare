#ifndef PEARE_LHA_PM2_DECODER_H
#define PEARE_LHA_PM2_DECODER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decompressLhaPm2(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputSize, std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare

#endif
