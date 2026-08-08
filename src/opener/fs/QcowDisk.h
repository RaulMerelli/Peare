#pragma once

// Read-only QEMU Copy-On-Write disk reader. Supports standalone QCOW v1 and
// QCOW2 v2/v3 images, including sparse/zero clusters, zlib-compressed clusters
// and QCOW2 extended L2 entries. Backing files, external data files and
// encrypted images are intentionally not resolved by this layer.

#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openQcowDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
