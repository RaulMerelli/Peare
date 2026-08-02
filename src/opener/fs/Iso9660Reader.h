#pragma once

// ISO 9660 (+ Joliet) read-only reader, ported from DiscUtils.Iso9660
// (VfsCDReader / CommonVolumeDescriptor / DirectoryRecord / ExtentStream).
//
// Faithful to the DiscUtils read path:
//   * volume descriptors are scanned from sector 16 until the set terminator;
//   * a Joliet supplementary descriptor (UTF-16BE names) is preferred when
//     present, otherwise the primary descriptor is used;
//   * the directory tree (metadata only) is walked up front — it is tiny — while
//     file CONTENT stays lazy: openFile returns a SubStore window over the disc
//     that reads on demand (the ExtentStream role, which for a contiguous file
//     is exactly a window).

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
