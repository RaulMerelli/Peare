#pragma once

// XFS read-only file system, ported from DiscUtils.Xfs for the opener's lazy
// DiscFileSystem surface. Supports v4/v5 superblocks, local/extent/btree inodes,
// shortform, block and leaf directories, and sparse file content by extents.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class XfsReader final : public IDiscFileSystem {
public:
    struct Extent {
        std::uint64_t startOffset = 0;
        std::uint64_t startBlock = 0;
        std::uint32_t blockCount = 0;
    };

    explicit XfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct Inode {
        bool valid = false;
        std::uint16_t mode = 0;
        std::uint8_t version = 0;
        std::uint8_t format = 0;
        std::uint64_t length = 0;
        std::uint32_t extentCount = 0;
        std::vector<std::uint8_t> dataFork;

        bool isDirectory() const { return ((mode >> 12) & 0xF) == 4; }
        bool isRegular() const { return ((mode >> 12) & 0xF) == 8; }
        bool isSymlink() const { return ((mode >> 12) & 0xF) == 10; }
    };

    struct DirRec {
        std::string name;
        std::uint64_t inode = 0;
    };

    void parse();
    std::uint64_t inodeOffset(std::uint64_t inodeNumber) const;
    Inode readInode(std::uint64_t inodeNumber) const;
    std::vector<Extent> inodeExtents(const Inode& inode) const;
    std::vector<Extent> btreeRootExtents(const std::vector<std::uint8_t>& root) const;
    void readBtreeBlock(std::uint64_t fsBlock, std::vector<Extent>* out, int depth) const;
    std::int64_t fsBlockOffset(std::uint64_t fsBlock) const;
    ByteStorePtr inodeContent(const Inode& inode) const;
    std::vector<DirRec> readDirectory(const Inode& dir) const;
    void readBlockDirectory(const std::vector<std::uint8_t>& data, std::vector<DirRec>* out) const;
    bool resolvePath(const std::string& path, Inode* out) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "XFS";

    std::uint32_t blockSize_ = 0;
    std::uint64_t dataBlocks_ = 0;
    std::uint64_t rootInode_ = 0;
    std::uint32_t agBlocks_ = 0;
    std::uint32_t agCount_ = 0;
    std::uint16_t inodeSize_ = 0;
    std::uint8_t blockSizeLog2_ = 0;
    std::uint8_t inodeSizeLog2_ = 0;
    std::uint8_t inodesPerBlockLog2_ = 0;
    std::uint8_t agBlocksLog2_ = 0;
    std::uint8_t dirBlockLog2_ = 0;
    std::uint16_t sbVersion_ = 0;
    bool hasFType_ = false;
    std::uint32_t relativeInodeMask_ = 0;
    std::uint32_t agInodeMask_ = 0;
    std::uint32_t dirBlockSize_ = 0;
};

}  // namespace fs
}  // namespace peare
