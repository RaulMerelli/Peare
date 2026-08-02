#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <string>

namespace peare {
namespace fs {

struct LinuxRaidSuperblock {
    bool valid = false;
    std::uint32_t majorVersion = 0;
    std::uint32_t minorVersion = 0;
    std::uint32_t raidLevel = 0;
    std::uint64_t dataOffsetSectors = 0;
    std::uint64_t arraySizeSectors = 0;
    std::uint32_t totalDisks = 0;
    std::string arrayName;
};

bool readLinuxRaidSuperblock(const ByteStorePtr& store, LinuxRaidSuperblock* out);
bool hasLinuxRaidSuperblock(const ByteStorePtr& store);

}  // namespace fs
}  // namespace peare
