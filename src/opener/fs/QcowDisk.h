#pragma once

// Read standalone QCOW v1 and QCOW2 v2/v3 images.
// Backing files, external data files, and encryption are not supported.

#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

ByteStorePtr openQcowDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
