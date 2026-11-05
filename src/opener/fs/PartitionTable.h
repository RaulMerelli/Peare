#pragma once

// Parse MBR and GPT tables over a disk byte store.
// Expose partitions as byte ranges for nested file-system openers.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscStore.h"

namespace peare {
namespace fs {

struct PartitionInfo {
    std::string typeName;      // human-readable partition type/label
    std::int64_t offset = 0;   // byte offset of the partition within the disk
    std::int64_t length = 0;   // byte length
    std::uint8_t mbrType = 0;  // original MBR type when applicable
    ByteStorePtr content;       // optional assembled logical-volume view
};

// Reads the partition table (GPT preferred, else MBR incl. extended chain).
// Returns an empty list when no valid table is present.
std::vector<PartitionInfo> readPartitionTable(const ByteStorePtr& disk,
                                              std::int64_t logicalSectorSize = 512);
bool hasApplePartitionMap(const ByteStorePtr& disk);
bool hasMbrOrGptPartitionTable(const ByteStorePtr& disk);

}  // namespace fs
}  // namespace peare
