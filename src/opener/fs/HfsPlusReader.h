#pragma once

// HFS+ read-only reader for Peare's DiscFileSystem surface. It parses the
// volume header and catalog B-tree leaf records, exposing regular files lazily
// through their data fork extents.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class HfsPlusReader final : public IDiscFileSystem {
public:
    struct Extent {
        std::uint32_t startBlock = 0;
        std::uint32_t blockCount = 0;
    };

    explicit HfsPlusReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct Fork {
        std::uint64_t logicalSize = 0;
        std::uint32_t totalBlocks = 0;
        std::vector<Extent> extents;
    };

    struct NodeDesc {
        std::uint32_t forward = 0;
        std::int8_t kind = 0;
        std::uint16_t records = 0;
    };

    struct Entry {
        std::string name;
        std::uint32_t id = 0;
        std::uint32_t parent = 0;
        bool directory = false;
        std::uint64_t length = 0;
        Fork dataFork;
    };

    void parse();
    Fork readFork(std::int64_t pos) const;
    bool readNode(std::uint32_t nodeNumber, std::vector<std::uint8_t>* out,
                  NodeDesc* desc) const;
    void parseLeafNode(const std::vector<std::uint8_t>& node);
    bool resolvePath(const std::string& path, Entry* out) const;
    ByteStorePtr forkStore(const Fork& fork) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "HFS+";

    std::uint32_t blockSize_ = 0;
    Fork catalogFork_;
    std::uint16_t nodeSize_ = 0;
    std::uint32_t firstLeaf_ = 0;

    std::map<std::uint32_t, std::vector<Entry> > children_;
};

}  // namespace fs
}  // namespace peare
