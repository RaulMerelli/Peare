#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

bool tibxUnwrapKey(const std::string& password, const std::vector<std::uint8_t>& wrapped,
                   std::vector<std::uint8_t>* key, std::uint8_t* algorithm);
bool tibxDecrypt(const std::vector<std::uint8_t>& key, std::uint8_t algorithm,
                 const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>* output);

} // namespace fs
} // namespace peare
