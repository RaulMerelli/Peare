#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class UfsReader final : public IDiscFileSystem {
public:
    explicit UfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

    static bool detect(const IByteStore& disc);
    static bool detect(const std::uint8_t* data, std::size_t size);

private:
    struct Geometry {
        bool bigEndian = true;
        std::uint32_t blockSize = 0;
        std::uint32_t fragmentSize = 0;
        std::uint32_t fragmentsPerBlock = 0;
        std::uint32_t cylinderGroups = 0;
        std::uint32_t inodesPerGroup = 0;
        std::uint32_t fragmentsPerGroup = 0;
        std::uint32_t inodesPerBlock = 0;
        std::uint32_t superBlockFragments = 0;
        std::uint32_t inodeTableFragment = 0;
        std::int32_t cylinderGroupOffset = 0;
        std::int32_t cylinderGroupMask = 0;
        std::uint32_t inodeSize = 128;
        std::uint32_t maxSymlinkLength = 60;
        bool ufs2 = false;
        std::int64_t superOffset = 0;
    };

    struct Node {
        std::uint32_t inode = 0;
        std::uint16_t mode = 0;
        std::uint64_t size = 0;
        std::vector<std::uint64_t> direct;
        std::vector<std::uint64_t> indirect;
        std::vector<std::uint8_t> inlineData;
        bool isDirectory = false;
        bool isSymlink = false;
        bool inlineSymlink = false;
        bool special = false;
    };

    bool parse();
    std::uint16_t read16(const std::uint8_t* p) const;
    std::uint32_t read32(const std::uint8_t* p) const;
    std::uint64_t read64(const std::uint8_t* p) const;
    bool readNode(std::uint32_t inode, Node* out) const;
    std::int64_t inodeOffset(std::uint32_t inode) const;
    std::int64_t cylinderGroupStart(std::uint32_t group) const;
    std::vector<DiscEntry> readDirectory(const Node& dir,
                                         std::map<std::string, std::uint32_t>* inodes = nullptr) const;
    bool resolve(const std::string& path, Node* out) const;

    ByteStorePtr disc_;
    Geometry geometry_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "UFS1";
};

}  // namespace fs
}  // namespace peare
