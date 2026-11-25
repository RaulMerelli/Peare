#pragma once

#include <cstdint>
#include <vector>

namespace grasshopper {

// Decrypt CBC with a 32-byte key, 16-byte IV, and PKCS#7 padding.
// On invalid input or padding, return false and leave plaintext empty.
// Input and output may refer to the same vector. CBC does not authenticate data.
bool decryptCbcPkcs7(const std::vector<std::uint8_t>& key,
                     const std::vector<std::uint8_t>& iv,
                     const std::vector<std::uint8_t>& ciphertext,
                     std::vector<std::uint8_t>& plaintext);

} // namespace grasshopper
