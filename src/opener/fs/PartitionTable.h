#pragma once

// Parse MBR and GPT tables over a disk byte store.
// Expose partitions as byte ranges for nested file-system openers.

#include <cstdint>
#include <array>
#include <memory>
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

enum class PartitionScheme { Unknown, Mbr, Gpt };

struct PartitionIdentity {
    PartitionScheme scheme = PartitionScheme::Unknown;
    std::uint32_t mbrDiskSignature = 0;
    std::uint64_t startOffset = 0;
    // GPT identifiers retain their 16-byte on-disk representation.
    std::array<std::uint8_t, 16> diskGuid = {};
    std::array<std::uint8_t, 16> partitionGuid = {};
};

enum class PartitionResolveResult { Found, Unknown, NotFound, Ambiguous, Unavailable };

class IPartitionResolver {
public:
    virtual ~IPartitionResolver() {}
    virtual PartitionResolveResult open(const PartitionIdentity& identity,
                                        ByteStorePtr* result) const = 0;
};

// Only a verified physical partition carries a usable identity. The returned
// stores preserve the identity and sibling resolver through nested sources.
// A present resolver is returned even when its partition identity is unknown.
std::vector<ByteStorePtr> makePartitionContextStores(
    const ByteStorePtr& disk, const std::vector<PartitionInfo>& partitions,
    std::int64_t logicalSectorSize = 512);
bool partitionContext(const ByteStorePtr& store, PartitionIdentity* identity,
                      std::shared_ptr<const IPartitionResolver>* resolver);

// Reads the partition table (GPT preferred, else MBR incl. extended chain).
// Returns an empty list when no valid table is present.
std::vector<PartitionInfo> readPartitionTable(const ByteStorePtr& disk,
                                              std::int64_t logicalSectorSize = 512);
bool hasApplePartitionMap(const ByteStorePtr& disk);
bool hasMbrOrGptPartitionTable(const ByteStorePtr& disk);

}  // namespace fs
}  // namespace peare
