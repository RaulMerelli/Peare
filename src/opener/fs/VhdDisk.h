#pragma once

// Read fixed and dynamic VHD disks as logical byte stores.
// Differencing parents are not supported; absent dynamic blocks read as zero.

#include <cstdint>
#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openVhdDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
