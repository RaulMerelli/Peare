#pragma once

// NTFS read-only file system, ported from DiscUtils.Ntfs (BiosParameterBlock,
// the MFT / FILE record model with update-sequence fixups, resident and
// non-resident attributes, data-run runlists, and the $I30 directory index of
// $INDEX_ROOT + $INDEX_ALLOCATION with $FILE_NAME keys).
//
// Scope: enumeration and extraction of uncompressed files (resident and
// non-resident, incl. sparse runs). Compressed ($DATA with the compressed flag,
// LZNT1) and $ATTRIBUTE_LIST-split attributes are not yet handled and are
// reported per-file rather than mis-read.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class NtfsReader final : public IDiscFileSystem {
public:
    explicit NtfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct Run {
        std::int64_t lcn = 0;      // -1 == sparse
        std::int64_t length = 0;   // in clusters
    };

    struct Attr {
        std::uint32_t type = 0;
        std::uint16_t flags = 0;   // 0x0001 compressed, 0x8000 sparse
        std::string name;          // attribute name (usually empty, or "$I30")
        bool nonResident = false;
        std::vector<std::uint8_t> residentData;
        std::vector<Run> runs;
        std::uint64_t realSize = 0;
    };

    struct Record {
        bool valid = false;
        bool isDirectory = false;
        std::vector<Attr> attrs;
    };

    struct DirEntry {
        std::string name;
        std::uint64_t mftRef = 0;
        bool isDirectory = false;
        std::uint64_t size = 0;
    };

    void parse();
    // Fix up a FILE/INDX record in place (undo the update-sequence protection).
    static void applyFixup(std::vector<std::uint8_t>& buf);
    std::vector<Run> parseRunlist(const std::uint8_t* p, std::size_t len) const;
    ByteStorePtr runsStore(const std::vector<Run>& runs, std::uint64_t realSize) const;
    Record readRecord(std::uint64_t index) const;
    Record parseRecord(std::vector<std::uint8_t>& buf) const;
    const Attr* findAttr(const Record& rec, std::uint32_t type, const std::string& name) const;
    std::vector<std::uint8_t> attrBytes(const Attr& a) const;  // full content, materialised
    void parseIndexNode(const std::uint8_t* node, std::size_t nodeLen, std::size_t entriesOffset,
                        std::vector<DirEntry>& out) const;
    std::vector<DirEntry> readDirectory(const Record& rec) const;
    bool resolvePath(const std::string& path, std::uint64_t* mftRef) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Microsoft NTFS";

    std::uint32_t bytesPerCluster_ = 4096;
    std::uint32_t mftRecordSize_ = 1024;
    ByteStorePtr mftStore_;  // the whole $MFT $DATA, as a positioned store
};

}  // namespace fs
}  // namespace peare
