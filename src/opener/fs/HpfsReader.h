#pragma once

// IBM/Microsoft High Performance File System (HPFS) read-only reader.
//
// The reader follows the on-disk structures used by OS/2 HPFS: boot block,
// super/spare blocks, directory dnode B-trees, fnodes/anodes and allocation
// B+trees.  It is deliberately lazy and read-only; only actual directory
// entries are exposed through IDiscFileSystem.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class HpfsReader final : public IDiscFileSystem {
public:
    struct Extent {
        std::uint32_t logicalSector = 0;
        std::uint32_t physicalSector = 0;
        std::uint32_t sectorCount = 0;
    };

    explicit HpfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct FnodeInfo {
        bool valid = false;
        bool directory = false;
        std::uint32_t sector = 0;
        std::uint32_t fileSize = 0;
        std::vector<std::uint8_t> raw;
    };

    struct EntryInfo {
        std::string name;
        std::uint32_t fnodeSector = 0;
        std::uint32_t fileSize = 0;
        std::uint8_t attributes = 0;
        bool directory = false;
    };

    void parse();
    bool loadHotfixMap(const std::vector<std::uint8_t>& spare);
    void loadCodePages(const std::vector<std::uint8_t>& spare);

    int readMapped(std::int64_t pos, std::uint8_t* dst, int count) const;
    std::vector<std::uint8_t> readSectors(std::uint32_t sector,
                                          std::uint32_t count) const;
    std::uint32_t remapSector(std::uint32_t sector) const;

    FnodeInfo readFnode(std::uint32_t sector) const;
    std::vector<Extent> fnodeExtents(const FnodeInfo& fnode) const;
    void parseBplusNode(const std::vector<std::uint8_t>& raw,
                        std::size_t headerOffset, std::size_t entriesOffset,
                        std::size_t leafCapacity, std::size_t internalCapacity,
                        std::vector<Extent>* extents,
                        std::set<std::uint32_t>* visitedAnodes,
                        int depth) const;
    ByteStorePtr fileContent(const FnodeInfo& fnode, std::uint32_t length) const;
    std::uint32_t directoryRootDnode(const FnodeInfo& fnode, bool knownDirectory) const;

    std::vector<EntryInfo> readDirectory(std::uint32_t fnodeSector, bool knownDirectory) const;
    bool walkDnode(std::uint32_t dnodeSector, std::vector<EntryInfo>* entries,
                   std::set<std::uint32_t>* visited, int depth) const;

    bool resolvePath(const std::string& path, EntryInfo* result,
                     FnodeInfo* fnode) const;
    std::string decodeName(const std::uint8_t* bytes, std::size_t length,
                           std::uint8_t codePageIndex) const;
    std::uint16_t codePageForIndex(std::uint8_t index) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "HPFS";
    std::uint32_t rootFnode_ = 0;
    std::uint32_t sectorCount_ = 0;
    std::map<std::uint32_t, std::uint32_t> hotfixes_;
    std::map<std::uint8_t, std::uint16_t> codePages_;
    std::uint16_t defaultCodePage_ = 850;
};

}  // namespace fs
}  // namespace peare
