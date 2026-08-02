#pragma once

// VMware Virtual Disk (VMDK) reader, ported from DiscUtils.Vmdk
// (HostedSparseExtentHeader + CommonSparseExtentStream grain lookup, plus the
// text disk descriptor that ties multi-extent disks together). It turns a .vmdk
// into a positioned byte store over the *logical* disk.
//
// Supported: monolithicSparse (single KDMV extent), split sparse/flat disks
// (twoGbMaxExtentSparse/Flat) described by a text descriptor referencing sibling
// extent files, and streamOptimized compressed extents.

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
