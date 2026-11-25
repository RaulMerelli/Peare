#pragma once

#include "DiscStore.h"

#include <string>

namespace peare {
namespace fs {

// A null or empty bitmap disables filtering. Bit 0 is the least significant bit.
// Reconstruction is performed only by calling this function.
ByteStorePtr reconstructAcronisFat(const ByteStorePtr& source,
                                   const ByteStorePtr& bitmap,
                                   std::string* error = nullptr);

} // namespace fs
} // namespace peare
