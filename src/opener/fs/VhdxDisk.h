#pragma once

// Microsoft VHDX reader, ported from DiscUtils.Vhdx (file/header/region/
// metadata/BAT/content read path). It exposes standalone fixed/dynamic images as
// one logical disk byte store. Differencing parents and dirty-log replay are not
// resolved yet.

#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openVhdxDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
