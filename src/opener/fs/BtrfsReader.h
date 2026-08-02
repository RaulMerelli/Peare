#pragma once

// Btrfs read-only reader, based on DiscUtils.Btrfs. Supports single-device
// chunk mapping, directory indexes, subvolume roots, inline/regular/sparse file
// extents and zlib-compressed extents.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class BtrfsReader final : public IDiscFileSystem {
public:
    struct Key {
        std::uint64_t objectId = 0;
        std::uint8_t type = 0;
        std::uint64_t offset = 0;
    };
    struct Chunk {
        std::uint64_t logical = 0;
        std::uint64_t size = 0;
        std::uint64_t physical = 0;
    };
    struct Extent {
        std::uint64_t fileOffset = 0;
        std::uint64_t decodedSize = 0;
        std::uint8_t compression = 0;
        std::uint8_t type = 0;
        std::vector<std::uint8_t> inlineData;
        std::uint64_t extentAddress = 0;
        std::uint64_t extentSize = 0;
        std::uint64_t extentOffset = 0;
        std::uint64_t logicalSize = 0;
    };

    explicit BtrfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct RawItem {
        Key key;
        std::vector<std::uint8_t> data;
        std::uint64_t physicalData = 0;
    };
    struct KeyPtr {
        Key key;
        std::uint64_t block = 0;
    };
    struct Node {
        std::uint64_t logical = 0;
        std::uint8_t level = 0;
        std::vector<RawItem> items;
        std::vector<KeyPtr> ptrs;
    };
    struct Inode {
        bool valid = false;
        std::uint64_t size = 0;
        std::uint32_t mode = 0;
        bool isDirectory() const { return ((mode & 0xf000U) >> 12) == 4; }
        bool isRegular() const { return ((mode & 0xf000U) >> 12) == 8; }
        bool isSymlink() const { return ((mode & 0xf000U) >> 12) == 10; }
    };
    struct Entry {
        std::string name;
        std::uint64_t treeId = 0;
        std::uint64_t objectId = 0;
        std::uint8_t childType = 0;
        Inode inode;
        bool subtree = false;
        bool isDirectory() const { return childType == 2 || subtree || inode.isDirectory(); }
    };

    void parse();
    void parseSuperblock(const std::vector<std::uint8_t>& sb);
    void parseChunkArray(const std::vector<std::uint8_t>& sb, std::size_t off, std::size_t len);
    Node readTree(std::uint64_t logical, std::uint8_t expectedLevel) const;
    void collectChunks(const Node& node, std::map<std::uint64_t, bool>* seen);
    std::uint64_t mapToPhysical(std::uint64_t logical) const;
    std::vector<RawItem> findItems(const Node& node, const Key& key) const;
    RawItem findFirst(const Node& node, const Key& key) const;
    Node fsTree(std::uint64_t treeId) const;
    Inode parseInode(const RawItem& item) const;
    Entry rootEntry() const;
    std::vector<Entry> readDirectory(const Entry& dir) const;
    bool resolvePath(const std::string& path, Entry* out) const;
    std::vector<Extent> readExtents(std::uint64_t treeId, std::uint64_t objectId) const;
    ByteStorePtr contentStore(const Entry& file) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Btrfs";

    std::uint32_t nodeSize_ = 0;
    std::uint32_t leafSize_ = 0;
    std::uint64_t rootLogical_ = 0;
    std::uint64_t chunkRootLogical_ = 0;
    std::uint64_t rootDirObjectId_ = 0;
    std::uint8_t rootLevel_ = 0;
    std::uint8_t chunkRootLevel_ = 0;
    std::vector<Chunk> chunks_;
    Node rootTree_;
    mutable std::map<std::uint64_t, Node> fsTrees_;
};

}  // namespace fs
}  // namespace peare
