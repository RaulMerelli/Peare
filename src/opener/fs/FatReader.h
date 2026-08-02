#pragma once

// FAT12/16/32 read-only file system, ported from DiscUtils.Fat (FatFileSystem,
// FatBuffer, ClusterReader, Directory, DirectoryEntry/FatFileName). Parses the
// BPB, follows cluster chains through the FAT, and exposes files whose content is
// a zero-copy view over the cluster runs (no decompression — FAT is plain).

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class FatReader final : public IDiscFileSystem {
public:
    explicit FatReader(ByteStorePtr disc);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    enum class FatType { Fat12, Fat16, Fat32 };

    struct DirEntry {
        std::string name;
        bool isDirectory = false;
        std::uint32_t firstCluster = 0;
        std::uint32_t size = 0;
    };

    void parse();
    std::uint32_t nextCluster(std::uint32_t cluster) const;
    bool isEndOfChain(std::uint32_t v) const;
    std::vector<std::uint32_t> chain(std::uint32_t firstCluster) const;
    std::int64_t clusterPos(std::uint32_t cluster) const;
    ByteStorePtr clusterContent(std::uint32_t firstCluster, std::uint32_t size) const;
    std::vector<std::uint8_t> readClusterChain(std::uint32_t firstCluster) const;
    std::vector<DirEntry> parseDirectory(const std::vector<std::uint8_t>& buf) const;
    // Directory entries at a path ("" = root).
    std::vector<DirEntry> directory(const std::string& dirPath) const;
    const DirEntry* find(const std::string& path, DirEntry* store) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "FAT";

    FatType type_ = FatType::Fat16;
    std::uint16_t bytesPerSec_ = 512;
    std::uint8_t secPerClus_ = 1;
    std::uint16_t rsvdSec_ = 0;
    std::uint8_t fatCount_ = 2;
    std::uint16_t rootEntCnt_ = 0;
    std::uint32_t fatSz_ = 0;
    std::uint32_t rootClus_ = 0;
    std::int64_t firstDataSector_ = 0;
    std::int64_t rootDirStart_ = 0;   // FAT12/16 fixed root area byte offset
    std::int64_t rootDirBytes_ = 0;

    std::vector<std::uint8_t> fat_;   // the (active) FAT table, materialised
};

}  // namespace fs
}  // namespace peare
