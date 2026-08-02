#pragma once

// VirtualBox VDI reader, ported from DiscUtils.Vdi (PreHeaderRecord,
// HeaderRecord and DiskStream read path). It exposes fixed and dynamic images
// as one logical disk byte store. Differencing/undo parents are not resolved.

#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openVdiDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
