#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace fs {

bool tibLegacyKdf(const std::vector<std::uint8_t>& password,
                  const std::vector<std::uint8_t>& salt,
                  std::array<std::uint8_t, 32>* output);

enum class TibKeyResult {
    Success,
    InvalidArgument,
    Truncated,
    InvalidRecord,
    Unsupported,
    IterationLimit,
    PasswordMismatch
};

// records starts at the first subrecord; consumed includes FF on success.
// maxIterations is the total PBKDF2 budget for all candidates.
TibKeyResult tibRecoverKey(const std::vector<std::uint8_t>& password,
                           const std::vector<std::uint8_t>& records,
                           std::uint64_t maxIterations,
                           std::vector<std::uint8_t>* key,
                           std::size_t* consumed);

} // namespace fs
} // namespace peare
