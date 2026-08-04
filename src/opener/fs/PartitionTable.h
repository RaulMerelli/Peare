#pragma once

// Master Boot Record (MBR) and GUID Partition Table (GPT) parsing over a disk
// byte store. Given the raw disk exposed by a virtual-disk container (VMDK, and
// later VHD/raw), it enumerates the partitions as byte ranges so each can be
// handed to the file-system openers as a nested container.

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
std::vector<PartitionInfo> readPartitionTable(const ByteStorePtr& disk);
bool hasApplePartitionMap(const ByteStorePtr& disk);
bool hasMbrOrGptPartitionTable(const ByteStorePtr& disk);

}  // namespace fs
}  // namespace peare
