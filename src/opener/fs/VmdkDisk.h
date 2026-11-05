#pragma once

// Read VMDK sparse, flat, split, and stream-optimized disks.
// Expose each image as a logical byte store.

#include <cstdint>
#include <functional>
#include <string>

#include "DiscStore.h"

namespace peare {
namespace fs {

// Opens ONE KDMV sparse extent and returns a store of its capacity.
ByteStorePtr openVmdkExtent(const ByteStorePtr& file, std::string* error);

// Builds the logical disk. If `file` is a KDMV extent it is used directly; if it
// is a text disk descriptor, its extent list is parsed and each named extent is
// resolved through resolveSibling (by file name) and concatenated in order.
ByteStorePtr openVmdkDisk(const ByteStorePtr& file,
                          const std::function<ByteStorePtr(const std::string&)>& resolveSibling,
                          std::string* error);

// Convenience for the nested/store path (KDMV single extent, no siblings).
ByteStorePtr openVmdkDisk(const ByteStorePtr& file, std::string* error);

}  // namespace fs
}  // namespace peare
