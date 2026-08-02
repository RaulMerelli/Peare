#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

struct DynamicVolumeInfo {
    std::string name;
    std::uint64_t sizeSectors = 0;
    ByteStorePtr content;
};

bool hasDynamicDiskMetadata(const ByteStorePtr& disk);
std::vector<DynamicVolumeInfo> readDynamicDiskVolumes(const ByteStorePtr& disk, std::string* error);

}  // namespace fs
}  // namespace peare
