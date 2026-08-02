#pragma once

// Microsoft exFAT read-only file system, ported from DiscUtils.ExFat
// (ExFatBootSector, ExFatPartition cluster/FAT handling, the File/Stream/FileName
// directory entry set, DataDescriptor contiguous-vs-FAT-chain allocation).
//
// exFAT stores each file's allocation as a DataDescriptor: a first cluster plus a
// NoFatChain flag. When NoFatChain is set the clusters are contiguous (no FAT walk
// needed); otherwise they are followed through the 32-bit FAT. Content is exposed
// as a zero-copy view over the cluster runs, like the FAT reader.

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class ExFatReader final : public IDiscFileSystem {
public:
    explicit ExFatReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    // A file/directory data stream: first cluster, whether contiguous, and the
    // allocated byte length (0 == unknown, follow the FAT chain to its end).
    struct DataDesc {
        std::uint32_t firstCluster = 0;
        bool contiguous = false;
        std::uint64_t length = 0;
    };

    struct DirRec {
        std::string name;
        bool isDirectory = false;
        DataDesc data;             // allocated stream
        std::uint64_t logicalLen = 0;  // valid data length (real file size)
    };

    void parse();
    std::uint32_t nextCluster(std::uint32_t cluster) const;
    std::vector<std::uint32_t> clusterChain(const DataDesc& desc) const;
    std::int64_t clusterOffset(std::uint32_t cluster) const;
    ByteStorePtr buildContent(const DataDesc& desc, std::uint64_t logicalLen) const;
    std::vector<DirRec> parseDirectory(const DataDesc& dir) const;
    // Directory records at a '/'-separated path ("" == root).
    std::vector<DirRec> directory(const std::string& path) const;
    bool find(const std::string& path, DirRec* out) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Microsoft exFAT";

    std::uint32_t bytesPerSector_ = 512;
    std::uint32_t sectorsPerCluster_ = 1;
    std::uint32_t bytesPerCluster_ = 512;
    std::uint32_t fatOffsetSector_ = 0;
    std::uint32_t clusterOffsetSector_ = 0;
    std::uint32_t clusterCount_ = 0;
    std::uint32_t rootDirCluster_ = 0;
    std::int64_t fatByteOffset_ = 0;
};

}  // namespace fs
}  // namespace peare
