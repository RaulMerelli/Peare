#pragma once

// SquashFS v4 read-only reader, ported from DiscUtils.SquashFs for Peare's
// lazy DiscFileSystem surface. Supports zlib-compressed and uncompressed
// metadata/data blocks, regular files, directories and symlinks.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class SquashFsReader final : public IDiscFileSystem {
public:
    explicit SquashFsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return "SquashFS"; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct MetadataRef {
        std::int64_t block = 0;
        std::uint16_t offset = 0;
    };

    struct MetaBlock {
        std::int64_t next = 0;
        std::vector<std::uint8_t> data;
    };

    struct Inode {
        bool valid = false;
        std::uint16_t type = 0;
        std::uint64_t size = 0;
        std::uint32_t startBlock = 0;
        std::uint32_t fragmentKey = 0xffffffffU;
        std::uint32_t fragmentOffset = 0;
        std::uint16_t dirOffset = 0;
        MetadataRef ref;
        std::vector<std::uint32_t> blockSizes;

        bool isDirectory() const { return type == 1 || type == 8; }
        bool isRegular() const { return type == 2 || type == 9; }
        bool isSymlink() const { return type == 3 || type == 10; }
    };

    struct DirRec {
        std::string name;
        MetadataRef inodeRef;
        bool isDirectory = false;
        bool isSymlink = false;
    };

    void parse();
    MetadataRef metadataRef(std::int64_t value) const;
    MetaBlock readMetaBlock(std::int64_t absolutePos) const;
    std::vector<std::uint8_t> readMetaBytes(std::int64_t tableStart, MetadataRef ref,
                                            std::size_t count, MetadataRef* end = nullptr) const;
    Inode readInode(MetadataRef ref) const;
    std::vector<DirRec> readDirectory(const Inode& dir) const;
    std::vector<std::uint8_t> readDataBlock(std::int64_t pos, std::uint32_t diskLen,
                                            std::size_t expected) const;
    bool readFragment(std::uint32_t key, std::vector<std::uint8_t>* out) const;
    bool resolvePath(const std::string& path, Inode* out) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;

    std::uint32_t blockSize_ = 0;
    std::uint16_t compression_ = 0;
    std::uint16_t flags_ = 0;
    std::uint16_t major_ = 0;
    std::uint16_t minor_ = 0;
    std::uint32_t fragmentsCount_ = 0;
    MetadataRef root_;
    std::int64_t inodeTableStart_ = 0;
    std::int64_t directoryTableStart_ = 0;
    std::int64_t fragmentTableStart_ = -1;
    std::int64_t xattrsTableStart_ = -1;

    mutable std::map<std::int64_t, MetaBlock> metaCache_;
    mutable std::map<std::uint32_t, std::vector<std::uint8_t>> fragmentCache_;
};

}  // namespace fs
}  // namespace peare
