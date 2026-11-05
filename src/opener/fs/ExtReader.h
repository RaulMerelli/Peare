#pragma once

// Read Ext2, Ext3, and Ext4 volumes and expose file data as lazy block views.
// Holes and unwritten extents read as zero.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class ExtReader final : public IDiscFileSystem {
public:
    explicit ExtReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct Inode {
        bool valid = false;
        std::uint16_t mode = 0;
        std::uint32_t inodeNumber = 0;
        std::uint32_t flags = 0;
        std::uint64_t fileSize = 0;
        bool usesExtents = false;
        bool fastSymlink = false;            // target path stored inline in i_block
        std::vector<std::uint8_t> blockMap;  // the 60-byte i_block area (raw)
    };

    struct DirRec {
        std::string name;
        std::uint32_t inode = 0;
        std::uint8_t fileType = 0;
    };

    void parse();
    Inode readInode(std::uint32_t inodeNum) const;
    std::uint64_t inodeTableBlock(std::uint32_t group) const;
    // Resolve logical block L of an inode to a physical block (0 == hole) via the
    // extent tree or the indirect block map.
    void resolveRun(const Inode& in, std::uint64_t logical, std::uint64_t totalBlocks,
                    std::uint64_t* physStart, std::uint64_t* runLen, bool* hole) const;
    ByteStorePtr buildContent(const Inode& in) const;
    bool verifyVerity(const Inode& in) const;
    bool readInodeBytes(const Inode& in, std::uint64_t offset,
                        std::uint8_t* dst, std::size_t size) const;
    bool lastExtentEnd(const std::vector<std::uint8_t>& node,
                       std::uint64_t* logicalEnd, unsigned depth) const;
    std::vector<DirRec> readDirectory(const Inode& dir) const;
    bool resolvePath(const std::string& path, Inode* out) const;

    // Extent-tree helper: find the extent covering logical block L, starting from
    // a node (root i_block or a loaded leaf). Returns false if not mapped.
    bool findExtent(const std::vector<std::uint8_t>& node, std::uint64_t logical,
                    std::uint64_t* firstLogical, std::uint64_t* physBlock,
                    std::uint64_t* numBlocks, bool* unwritten) const;
    std::uint32_t indirectLookup(const Inode& in, std::uint64_t logical) const;
    std::uint32_t readU32(std::int64_t bytePos) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "EXT-family";

    std::uint32_t blockSize_ = 1024;
    std::uint32_t inodesPerGroup_ = 0;
    std::uint32_t blocksPerGroup_ = 0;
    std::uint16_t inodeSize_ = 128;
    std::uint64_t firstDataBlock_ = 1;
    std::uint64_t blocksCount_ = 0;
    std::uint32_t descriptorSize_ = 32;
    bool has64Bit_ = false;
    std::int64_t bgDescStart_ = 0;
};

}  // namespace fs
}  // namespace peare
