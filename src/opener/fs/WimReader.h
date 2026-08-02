#pragma once

// WIM (Windows Imaging) read-only file system, ported from DiscUtils.Wim
// (WimFile + WimFileSystem + FileResourceStream). Parses the 512-byte header,
// the resource lookup table (SHA1 -> resource), the first image's metadata
// (security block + directory tree), and exposes files whose content is a
// chunked LZX/XPRESS resource decoded on demand.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class WimReader final : public IDiscFileSystem {
public:
    explicit WimReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    // A pointer to a resource within the WIM (from a ShortResourceHeader).
    struct ResHeader {
        std::uint8_t flags = 0;       // ResourceFlags (Compressed=0x04, MetaData=0x02)
        std::int64_t compressedSize = 0;
        std::int64_t fileOffset = 0;
        std::int64_t originalSize = 0;
    };

    // A parsed directory entry (subset needed for read-only navigation).
    struct DirEntry {
        std::string name;
        bool isDirectory = false;
        std::int64_t length = 0;
        std::int64_t subdirOffset = 0;      // metadata-stream offset of children
        std::vector<std::uint8_t> hash;     // 20-byte SHA1 of the content resource
    };

    static ResHeader readResHeader(const std::vector<std::uint8_t>& buf, std::size_t off);
    ByteStorePtr makeResourceStore(const ResHeader& hdr) const;
    std::vector<std::uint8_t> materialize(const ResHeader& hdr) const;

    void parse();
    // Directory list at a metadata-stream offset (0 == root), cached.
    const std::vector<DirEntry>& directoryAt(std::int64_t offset) const;
    const DirEntry* entryForPath(const std::string& path) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Microsoft WIM";

    std::uint32_t fileFlags_ = 0;
    std::int32_t chunkSize_ = 0;
    ResHeader offsetTable_;
    ResHeader metadata_;

    // SHA1 hash -> resource header (the lookup table).
    std::map<std::vector<std::uint8_t>, ResHeader> resources_;

    // The image metadata stream, fully materialised for random access.
    std::vector<std::uint8_t> meta_;
    std::int64_t rootDirPos_ = 0;
    mutable std::map<std::int64_t, std::vector<DirEntry>> dirCache_;
};

}  // namespace fs
}  // namespace peare
