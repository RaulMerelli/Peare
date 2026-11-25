#pragma once

// Read NTFS resident, non-resident, sparse, LZNT1, and WOF streams.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

struct PartitionIdentity;
class IPartitionResolver;

class NtfsReader final : public IDiscFileSystem {
public:
    explicit NtfsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    bool listChecked(const std::string& dirPath, std::vector<DiscEntry>* entries) const;
    ByteStorePtr openFile(const std::string& path) const override;
    bool partitionContext(PartitionIdentity* identity,
                          std::shared_ptr<const IPartitionResolver>* resolver) const;

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
        std::uint16_t id = 0;
        std::uint64_t lowestVcn = 0;
        std::vector<std::uint8_t> residentData;
        std::vector<Run> runs;
        std::vector<Run> strictRuns;
        std::uint64_t realSize = 0;
        std::uint64_t allocatedSize = 0;
        std::uint64_t initializedSize = 0;
        std::uint16_t compressionUnit = 0;
        bool compressedRunsValid = false;
        bool strictRunsValid = false;
    };

    struct Record {
        bool valid = false;
        bool isDirectory = false;
        std::uint16_t sequence = 0;
        std::uint64_t baseReference = 0;
        std::vector<Attr> attrs;
    };

    struct DirEntry {
        std::string name;
        std::uint64_t mftRef = 0;
        bool isDirectory = false;
        std::uint64_t size = 0;
    };

    enum class WofLookup { Found, NotFound, Error };

    void parse();
    // Fix up a FILE/INDX record in place (undo the update-sequence protection).
    bool applyFixup(std::vector<std::uint8_t>& buf) const;
    std::vector<Run> parseRunlist(const std::uint8_t* p, std::size_t len) const;
    bool parseCompressedRunlist(const std::uint8_t* p, std::size_t len,
                                std::vector<Run>* runs) const;
    ByteStorePtr runsStore(const std::vector<Run>& runs, std::uint64_t realSize) const;
    ByteStorePtr openWofBacking(const std::string& path, WofLookup* status = nullptr) const;
    ByteStorePtr openWofWim(const Record& rec, const Attr& data) const;
    Record readRecordRaw(std::uint64_t index) const;
    Record readRecord(std::uint64_t index) const;
    Record parseRecord(std::vector<std::uint8_t>& buf) const;
    void mergeAttributeList(std::uint64_t baseIndex, Record* rec) const;
    void normalizeAttributes(Record* rec) const;
    const Attr* findAttr(const Record& rec, std::uint32_t type, const std::string& name) const;
    std::vector<std::uint8_t> attrBytes(const Attr& a) const;  // full content, materialised
    void parseIndexNode(const std::uint8_t* node, std::size_t nodeLen, std::size_t entriesOffset,
                        std::vector<DirEntry>& out, bool includeSystem = false) const;
    std::vector<DirEntry> readDirectory(const Record& rec, bool includeSystem = false,
                                        bool* complete = nullptr) const;
    bool resolvePath(const std::string& path, std::uint64_t* mftRef) const;
    bool resolveWofPath(const std::string& path, std::uint64_t* mftRef) const;
    bool readWofStrictRecord(std::uint64_t index, Record* record) const;
    bool readWofStrictRecordRaw(std::uint64_t index, Record* record) const;
    WofLookup resolveWofPathStrict(const std::string& path, std::uint64_t* mftRef) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Microsoft NTFS";

    std::uint32_t bytesPerSector_ = 512;
    std::uint32_t bytesPerCluster_ = 4096;
    std::uint32_t mftRecordSize_ = 1024;
    ByteStorePtr mftStore_;  // the whole $MFT $DATA, as a positioned store
    mutable std::mutex directoryMutex_;
    mutable std::map<std::uint64_t, std::map<std::string, std::uint64_t> > directoryCache_;
};

}  // namespace fs
}  // namespace peare
