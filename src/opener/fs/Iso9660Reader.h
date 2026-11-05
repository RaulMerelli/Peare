#pragma once

// Read ISO 9660 volumes with Joliet names and multi-extent files.
// Enumerate directory metadata up front and read file content on demand.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"
#include "DiscStore.h"

namespace peare {
namespace fs {

class Iso9660Reader : public IDiscFileSystem {
public:
    explicit Iso9660Reader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

    static bool detect(const IByteStore& disc);

private:
    struct Node {
        std::string name;
        bool isDir = false;
        std::uint32_t extent = 0;  // starting logical block
        std::uint32_t length = 0;  // content length in bytes
        std::vector<Node> children;
    };

    void parse();
    void readDirectory(Node& dir, bool joliet, int depth);
    const Node* find(const std::string& path) const;

    ByteStorePtr disc_;
    Node root_;
    std::string friendly_;
    std::string error_;
    bool valid_ = false;
};

}  // namespace fs
}  // namespace peare
