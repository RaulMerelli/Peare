#pragma once

// OSTA Universal Disk Format (UDF) read-only file system, ported from
// DiscUtils.Udf (UdfReader/UdfContext/UdfUtilities, the volume/partition
// descriptors, the File/FileEntry/ExtendedFileEntry ICB model, FileContentBuffer
// and the Directory/FileIdentifier tree).
//
// Scope: the common on-disc case — Type 1 (physical) partitions with short,
// long or embedded allocation descriptors, FileEntry (261) and ExtendedFileEntry
// (266) ICBs. Metadata/Sparable/Virtual partition maps are reported as an error
// rather than silently mis-read.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class UdfReader final : public IDiscFileSystem {
public:
    explicit UdfReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    // A logical partition (Type 1): a window over the disc plus the block size.
    struct LogicalPart {
        ByteStorePtr content;          // SubStore over the physical partition
        std::uint32_t blockSize = 2048;
    };

    // A long_ad / short_ad reference resolved enough to locate an ICB or extent.
    struct Icb {
        std::uint32_t logicalBlock = 0;
        std::uint16_t partition = 0;
        std::uint32_t length = 0;      // extent byte length
    };

    // A parsed File/ExtendedFileEntry.
    struct Node {
        bool valid = false;
        bool isDirectory = false;
        std::uint16_t partition = 0;   // partition the entry (and short ADs) live in
        std::uint64_t informationLength = 0;
        int allocationType = 0;        // 0 short, 1 long, 2 extended, 3 embedded
        std::vector<std::uint8_t> allocationDescriptors;
    };

    void parse();
    bool probeSectorSize(std::uint32_t size) const;
    std::vector<std::uint8_t> readExtent(const Icb& icb) const;
    Node readNode(const Icb& icb) const;
    ByteStorePtr nodeContent(const Node& node) const;
    std::vector<DiscEntry> readDirectory(const Node& node,
                                         std::vector<Icb>* childIcbs) const;
    // Resolve a '/'-separated path to its ICB. Returns false if not found.
    bool resolve(const std::string& path, Icb* out, bool* isDir) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "OSTA Universal Disk Format";

    std::uint32_t sectorSize_ = 2048;
    std::vector<LogicalPart> partitions_;   // indexed by logical partition number
    Icb rootIcb_;
};

}  // namespace fs
}  // namespace peare
