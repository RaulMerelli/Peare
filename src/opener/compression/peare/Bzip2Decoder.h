#ifndef PEARE_BZIP2_DECODER_H
#define PEARE_BZIP2_DECODER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decompressBzip2(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputLimit, bool allowConcatenated,
                     std::vector<std::uint8_t>* output);
bool decompressNsisBzip2(const std::uint8_t* input, std::size_t inputSize,
                         std::size_t outputLimit,
                         std::vector<std::uint8_t>* output);
bool decompressNsisV1Bzip2(const std::uint8_t* input, std::size_t inputSize,
                           std::size_t outputLimit,
                           std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare

#endif
