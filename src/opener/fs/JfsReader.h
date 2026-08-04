#pragma once

// IBM Journaled File System (JFS) read-only reader.
//
// The implementation follows the Linux JFS on-disk structures: the primary
// superblock at 0x8000, the fixed aggregate inode table, the fileset inode-map
// inode, IAG inode allocation pages, directory dtrees and file xtrees.  It is
// intentionally read-only and exposes regular files and symlinks through the
// lazy DiscFileSystem byte-store interface.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class JfsReader final : public IDiscFileSystem {
public:
    struct Extent {
        std::uint64_t logicalBlock = 0;
        std::uint64_t physicalBlock = 0;
        std::uint32_t blockCount = 0;
        std::uint8_t flags = 0;
    };

    explicit JfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

    // Probe metrics used by the partition layer when choosing between a raw
    // OS/2 LVM segment and an assembled logical volume.
    int qualityScore() const { return qualityScore_; }
    std::uint64_t declaredAggregateBytes() const { return aggregateBytes_; }

private:
    struct Inode {
        bool valid = false;
        std::uint32_t number = 0;
        std::uint32_t mode = 0;
        std::uint64_t size = 0;
        std::uint64_t blocks = 0;
        std::vector<std::uint8_t> raw;

        bool isPosixDirectory() const { return (mode & 0xF000U) == 0x4000U; }
        bool isPosixRegular() const { return (mode & 0xF000U) == 0x8000U; }
        bool isPosixSymlink() const { return (mode & 0xF000U) == 0xA000U; }
    };

    struct DirRec {
        std::string name;
        std::uint32_t inode = 0;
    };

    void parse();
    bool parseSuperblock(const std::vector<std::uint8_t>& sb);
    Inode readAggregateInode(std::uint32_t number, bool secondary) const;
    Inode readFilesetInode(std::uint32_t number) const;
    bool readIag(std::uint32_t iagNumber, std::vector<std::uint8_t>* out) const;
    int scoreFilesetTree(int maxDepth, int maxNodes) const;

    std::vector<Extent> inodeExtents(const Inode& inode) const;
    void parseXtreeNode(const std::vector<std::uint8_t>& node, bool root,
                        std::vector<Extent>* out, std::set<std::uint64_t>* visited,
                        int depth) const;
    ByteStorePtr inodeContent(const Inode& inode) const;

    std::vector<DirRec> readDirectory(const Inode& inode) const;
    bool parseDtreeLeaf(const std::vector<std::uint8_t>& node, bool root,
                        std::vector<DirRec>* out) const;
    bool descendToLeftmostLeaf(const std::vector<std::uint8_t>& root,
                               std::vector<std::uint8_t>* leaf,
                               std::size_t* leafBytes) const;
    bool collectDtreeNode(const std::vector<std::uint8_t>& node, bool root,
                          std::vector<DirRec>* out,
                          std::set<std::uint64_t>* visited, int depth) const;
    bool decodeLeafEntry(const std::vector<std::uint8_t>& node, int slot,
                         int slotCount, DirRec* out) const;
    bool decodeLeafEntryWithCapacity(const std::vector<std::uint8_t>& node, int slot,
                                     int slotCount, int firstCapacity,
                                     DirRec* out) const;
    bool decodeInternalChild(const std::vector<std::uint8_t>& node, int slot,
                             int slotCount, std::uint64_t* childBlock,
                             std::uint32_t* childBlocks) const;
    bool readDtreePage(std::uint64_t block, std::uint32_t hintedBlocks,
                       std::vector<std::uint8_t>* out) const;

    bool inodeIsDirectory(const Inode& inode) const;
    bool inodeIsRegular(const Inode& inode) const;
    bool inodeIsSymlink(const Inode& inode) const;
    bool resolvePath(const std::string& path, Inode* out) const;
    std::int64_t blockOffset(std::uint64_t block) const;
    bool mapLogicalBlock(const std::vector<Extent>& extents, std::uint64_t logical,
                         std::uint64_t* physical) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "JFS";

    std::uint32_t version_ = 0;
    std::uint32_t blockSize_ = 0;
    std::uint16_t blockShift_ = 0;
    std::uint32_t flags_ = 0;
    bool directoryIndex_ = false;
    bool caseInsensitive_ = false;
    std::uint64_t secondaryAitBlock_ = 0;
    std::uint64_t aggregateBytes_ = 0;
    int qualityScore_ = -1;
    std::vector<Extent> inodeMapExtents_;
};

}  // namespace fs
}  // namespace peare
