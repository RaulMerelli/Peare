#pragma once

// Microsoft VHD reader, ported from DiscUtils.Vhd (Footer, DynamicHeader and
// DynamicStream read path). It exposes a fixed or dynamic VHD as one positioned
// byte store over the logical disk. Differencing parents are not resolved yet;
// absent dynamic blocks read as zero, matching standalone dynamic disks.

#include <cstdint>
#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openVhdDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
