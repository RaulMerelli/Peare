#pragma once

// Read standalone fixed and dynamic VHDX disks as logical byte stores.
// Differencing parents and dirty-log replay are not supported.

#include <string>
#include <cstdint>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openVhdxDisk(const ByteStorePtr& file, std::string* error,
                          std::uint32_t* logicalSectorSize = nullptr);

}  // namespace fs
}  // namespace peare
