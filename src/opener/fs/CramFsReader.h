#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class CramFsReader final : public IDiscFileSystem {
public:
    explicit CramFsReader(ByteStorePtr disc);

    std::string friendlyName() const override { return "CramFS"; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

    static bool hasSignature(const ByteStorePtr& disc, std::int64_t* offset = nullptr);

private:
    struct Inode {
        std::uint16_t mode = 0;
        std::uint32_t size = 0;
        std::uint8_t nameLengthWords = 0;
        std::uint32_t dataOffset = 0;
        bool isDirectory() const { return (mode & 0170000) == 0040000; }
        bool isRegular() const { return (mode & 0170000) == 0100000; }
        bool isSymlink() const { return (mode & 0170000) == 0120000; }
        bool isCharacterDevice() const { return (mode & 0170000) == 0020000; }
        bool isBlockDevice() const { return (mode & 0170000) == 0060000; }
        bool isFifo() const { return (mode & 0170000) == 0010000; }
        bool isSocket() const { return (mode & 0170000) == 0140000; }
        bool isSpecial() const {
            return isCharacterDevice() || isBlockDevice() || isFifo() || isSocket();
        }
        std::string specialType() const {
            if (isCharacterDevice()) return "CRAMFS_CHARACTER_DEVICE";
            if (isBlockDevice()) return "CRAMFS_BLOCK_DEVICE";
            if (isFifo()) return "CRAMFS_FIFO";
            if (isSocket()) return "CRAMFS_SOCKET";
            return std::string();
        }
    };

    struct Entry {
        std::string name;
        Inode inode;
    };

    bool parse();
    bool readInode(std::int64_t offset, Inode* inode) const;
    bool readDirectory(const Inode& inode, std::vector<Entry>* entries) const;
    bool resolvePath(const std::string& path, Inode* inode) const;
    std::uint32_t read32(const std::uint8_t* data) const;

    ByteStorePtr disc_;
    bool valid_ = false;
    bool bigEndian_ = false;
    std::int64_t imageOffset_ = 0;
    std::uint32_t imageSize_ = 0;
    std::uint32_t flags_ = 0;
    Inode root_;
    std::string error_;
};

}  // namespace fs
}  // namespace peare
