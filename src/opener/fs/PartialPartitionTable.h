#pragma once

#include "DiscStore.h"

#include <array>
#include <cstdint>
#include <vector>

namespace peare {
namespace fs {

enum class PartialTableStatus { Valid, Absent, Incomplete, Invalid, IoError };

struct PartialPartitionEntry {
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;
    std::uint8_t mbrType = 0;
    std::array<std::uint8_t, 16> typeGuid = {};
    std::array<std::uint8_t, 16> partitionGuid = {};
};

struct PartialMbrTable {
    PartialTableStatus status = PartialTableStatus::Incomplete;
    std::uint32_t diskSignature = 0;
    bool protective = false;
    bool hasExtended = false;
    std::vector<PartialPartitionEntry> entries;
};

struct PartialGptTable {
    PartialTableStatus status = PartialTableStatus::Incomplete;
    std::array<std::uint8_t, 16> diskGuid = {};
    std::vector<PartialPartitionEntry> entries;
};

// Both observations are independent: neither one selects the active scheme.
// A valid GPT result requires a complete primary header and table with CRCs.
// MBR includes logical entries only if every referenced EBR is readable;
// hasExtended records the presence of extended containers.
void readPartialPartitionTables(const ByteStorePtr& disk, std::uint32_t sectorSize,
                                PartialMbrTable* mbr, PartialGptTable* gpt);

} // namespace fs
} // namespace peare
