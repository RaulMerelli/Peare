#pragma once

#include "DiscStore.h"

#include <string>
#include <vector>

namespace peare {
namespace fs {

// Parts must be complete and ordered by their one-based volume number.
ByteStorePtr openTibMultiVolume(const std::vector<ByteStorePtr>& parts,
                                std::string* error = nullptr);

} // namespace fs
} // namespace peare
