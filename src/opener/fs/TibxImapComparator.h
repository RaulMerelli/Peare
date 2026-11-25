#pragma once

#include <cstdint>
#include <vector>

namespace peare {
namespace fs {

enum class TibxImapSort { Plain, Hierarchical };

// Returns false for an invalid key or malformed hierarchical name.
// On success, result is negative, zero, or positive.
bool compareTibxImapKeys(const std::vector<std::uint8_t>& left,
                         const std::vector<std::uint8_t>& right,
                         TibxImapSort mode, int* result);

}  // namespace fs
}  // namespace peare
