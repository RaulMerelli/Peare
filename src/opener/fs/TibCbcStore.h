#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

// Offsets and capacity match source; reads in the initial IV fail.
ByteStorePtr openTibCbcStore(const ByteStorePtr& source,
                              const std::vector<std::uint8_t>& key,
                              std::string* error = nullptr);

} // namespace fs
} // namespace peare
