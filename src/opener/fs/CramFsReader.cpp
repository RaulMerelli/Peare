#include "CramFsReader.h"

#include "../modules/DeflateDecoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kMagic = 0x28cd3d45U;
const std::uint32_t kFlagVersion2 = 0x00000001U;
const std::uint32_t kFlagSortedDirectories = 0x00000002U;
const std::uint32_t kFlagHoles = 0x00000100U;
const std::uint32_t kFlagShiftedRoot = 0x00000200U;
const std::uint32_t kFlagExtendedPointers = 0x00000400U;
const std::uint32_t kSupportedFlags = kFlagVersion2 | kFlagSortedDirectories | kFlagHoles |
                                      kFlagShiftedRoot | kFlagExtendedPointers;
const std::uint32_t kBlockSize = 4096;
const std::uint32_t kBlockFlagUncompressed = 1;
const std::uint32_t kBlockFlagDirect = 2;
const std::uint32_t kBlockFlagMask = 3;

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (!part.empty()) { parts.push_back(part); part.clear(); }
        } else {
            part.push_back(ch);
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}

class CramFileStore final : public IByteStore {
public:
    CramFileStore(ByteStorePtr disc, std::int64_t imageOffset, std::uint32_t imageSize,
                  std::uint32_t dataOffset, std::uint32_t size, bool bigEndian,
                  bool extendedPointers, bool holes)
        : disc_(std::move(disc)), imageOffset_(imageOffset), imageSize_(imageSize),
          dataOffset_(dataOffset), size_(size), bigEndian_(bigEndian),
          extendedPointers_(extendedPointers), holes_(holes) {}

    std::int64_t capacity() const override { return size_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!disc_ || !dst || pos < 0 || count <= 0 || pos >= size_) return 0;
        const int wanted = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
        int done = 0;
        while (done < wanted) {
            const std::uint32_t at = static_cast<std::uint32_t>(pos + done);
            const std::uint32_t blockIndex = at / kBlockSize;
            std::vector<std::uint8_t> block;
            if (!readBlock(blockIndex, &block)) break;
            const std::uint32_t inBlock = at % kBlockSize;
            const int take = std::min<int>(wanted - done,
                static_cast<int>(block.size() > inBlock ? block.size() - inBlock : 0));
            if (take <= 0) break;
            std::copy(block.begin() + inBlock, block.begin() + inBlock + take, dst + done);
            done += take;
        }
        return done;
    }

private:
    std::uint32_t read32At(std::int64_t offset) const {
        const std::vector<std::uint8_t> bytes = disc_->readRange(offset, 4);
        if (bytes.size() != 4) return 0;
        if (bigEndian_)
            return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                   (std::uint32_t(bytes[2]) << 8) | bytes[3];
        return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
               (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    }

    bool readBlock(std::uint32_t index, std::vector<std::uint8_t>* output) const {
        if (!output || index >= (size_ + kBlockSize - 1) / kBlockSize) return false;
        const std::uint32_t blockCount = (size_ + kBlockSize - 1) / kBlockSize;
        const std::int64_t table = imageOffset_ + dataOffset_;
        const std::int64_t tableEnd = table + std::int64_t(blockCount) * 4;
        if (dataOffset_ > imageSize_ || std::int64_t(blockCount) * 4 > imageSize_ - dataOffset_)
            return false;
        std::uint32_t pointer = read32At(table + std::int64_t(index) * 4);
        std::int64_t start = tableEnd - imageOffset_;
        if (index != 0) {
            const std::uint32_t previous = read32At(table + std::int64_t(index - 1) * 4);
            if (extendedPointers_ && (previous & kBlockFlagDirect)) {
                const std::uint32_t address = (previous & ~kBlockFlagMask) << 2;
                if (previous & kBlockFlagUncompressed) {
                    start = std::int64_t(address) + kBlockSize;
                } else {
                    const std::vector<std::uint8_t> prefix = disc_->readRange(imageOffset_ + address, 2);
                    if (prefix.size() != 2) return false;
                    const std::uint32_t length = bigEndian_
                        ? (std::uint32_t(prefix[0]) << 8) | prefix[1]
                        : std::uint32_t(prefix[0]) | (std::uint32_t(prefix[1]) << 8);
                    start = std::int64_t(address) + 2 + length;
                }
            } else {
                start = extendedPointers_ ? previous & ~kBlockFlagMask : previous;
            }
        }

        bool uncompressed = false;
        std::int64_t diskStart = 0;
        std::int64_t diskLength = 0;
        if (extendedPointers_ && (pointer & kBlockFlagDirect)) {
            uncompressed = (pointer & kBlockFlagUncompressed) != 0;
            diskStart = imageOffset_ + std::int64_t((pointer & ~kBlockFlagMask) << 2);
            if (uncompressed) {
                diskLength = std::min<std::uint32_t>(kBlockSize, size_ - index * kBlockSize);
            } else {
                const std::vector<std::uint8_t> prefix = disc_->readRange(diskStart, 2);
                if (prefix.size() != 2) return false;
                const std::uint32_t packed = bigEndian_
                    ? (std::uint32_t(prefix[0]) << 8) | prefix[1]
                    : std::uint32_t(prefix[0]) | (std::uint32_t(prefix[1]) << 8);
                diskStart += 2;
                diskLength = packed;
            }
        } else {
            const std::int64_t end = extendedPointers_ ? pointer & ~kBlockFlagMask : pointer;
            diskStart = imageOffset_ + start;
            diskLength = end - start;
        }

        if (diskLength == 0) {
            if (!holes_) return false;
            output->assign(kBlockSize, 0);
            return true;
        }
        if (diskLength < 0 || diskLength > 2 * kBlockSize ||
            (uncompressed && diskLength > kBlockSize) ||
            diskStart < imageOffset_ || diskStart + diskLength > imageOffset_ + imageSize_)
            return false;
        const std::vector<std::uint8_t> packed = disc_->readRange(diskStart, diskLength);
        if (packed.size() != static_cast<std::size_t>(diskLength)) return false;
        if (uncompressed) {
            *output = packed;
        } else if (!compression::inflateZlibExact(
                       packed, std::min<std::uint32_t>(kBlockSize, size_ - index * kBlockSize),
                       output)) {
            return false;
        }
        if (output->size() > kBlockSize) return false;
        output->resize(kBlockSize, 0);
        return true;
    }

    ByteStorePtr disc_;
    std::int64_t imageOffset_;
    std::uint32_t imageSize_;
    std::uint32_t dataOffset_;
    std::uint32_t size_;
    bool bigEndian_;
    bool extendedPointers_;
    bool holes_;
};

}  // namespace

CramFsReader::CramFsReader(ByteStorePtr disc) : disc_(std::move(disc)) { valid_ = parse(); }

std::uint32_t CramFsReader::read32(const std::uint8_t* data) const {
    if (bigEndian_)
        return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) |
               (std::uint32_t(data[2]) << 8) | data[3];
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

bool CramFsReader::hasSignature(const ByteStorePtr& disc, std::int64_t* offset) {
    if (!disc) return false;
    const std::int64_t starts[] = {0, 64, 512};
    for (std::int64_t start : starts) {
        const std::vector<std::uint8_t> bytes = disc->readRange(start, 32);
        if (bytes.size() < 32) continue;
        const bool le = bytes[0] == 0x45 && bytes[1] == 0x3d && bytes[2] == 0xcd && bytes[3] == 0x28;
        const bool be = bytes[0] == 0x28 && bytes[1] == 0xcd && bytes[2] == 0x3d && bytes[3] == 0x45;
        if ((le || be) && std::memcmp(bytes.data() + 16, "Compressed ROMFS", 16) == 0) {
            if (offset) *offset = start;
            return true;
        }
    }
    return false;
}

bool CramFsReader::parse() {
    if (!disc_ || !hasSignature(disc_, &imageOffset_)) {
        error_ = "Not a supported CramFS image";
        return false;
    }
    const std::vector<std::uint8_t> header = disc_->readRange(imageOffset_, 76);
    if (header.size() < 76) { error_ = "Truncated CramFS superblock"; return false; }
    bigEndian_ = header[0] == 0x28;
    const std::uint32_t magic = read32(header.data());
    imageSize_ = read32(header.data() + 4);
    flags_ = read32(header.data() + 8);
    if (!(flags_ & kFlagVersion2))
        imageSize_ = static_cast<std::uint32_t>(std::min<std::int64_t>(
            disc_->capacity() - imageOffset_, 1LL << 28));
    if (magic != kMagic || imageSize_ < 76 || imageSize_ > disc_->capacity() - imageOffset_ ||
        (flags_ & ~kSupportedFlags) != 0) {
        error_ = "Invalid or unsupported CramFS superblock";
        return false;
    }
    if (!readInode(imageOffset_ + 64, &root_) || !root_.isDirectory()) {
        error_ = "Invalid CramFS root inode";
        return false;
    }
    const std::uint64_t rootLocation = std::uint64_t(root_.dataOffset) * 4;
    if (rootLocation > imageSize_ ||
        (!(flags_ & kFlagShiftedRoot) && rootLocation != 0 && rootLocation != 76 && rootLocation != 588)) {
        error_ = "Invalid CramFS root directory offset";
        return false;
    }
    std::vector<Entry> entries;
    if (!readDirectory(root_, &entries)) {
        error_ = "Invalid CramFS root directory";
        return false;
    }
    return true;
}

bool CramFsReader::readInode(std::int64_t offset, Inode* inode) const {
    if (!inode || offset < imageOffset_ || offset + 12 > imageOffset_ + imageSize_) return false;
    const std::vector<std::uint8_t> bytes = disc_->readRange(offset, 12);
    if (bytes.size() != 12) return false;
    const std::uint32_t modeUid = read32(bytes.data());
    const std::uint32_t sizeGid = read32(bytes.data() + 4);
    const std::uint32_t nameOffset = read32(bytes.data() + 8);
    if (bigEndian_) {
        inode->mode = static_cast<std::uint16_t>(modeUid >> 16);
        inode->size = (sizeGid >> 8) & 0x00ffffffU;
        inode->nameLengthWords = static_cast<std::uint8_t>(nameOffset >> 26);
        inode->dataOffset = nameOffset & 0x03ffffffU;
    } else {
        inode->mode = static_cast<std::uint16_t>(modeUid & 0xffffU);
        inode->size = sizeGid & 0x00ffffffU;
        inode->nameLengthWords = static_cast<std::uint8_t>(nameOffset & 0x3fU);
        inode->dataOffset = nameOffset >> 6;
    }
    return inode->mode != 0 && inode->nameLengthWords <= 63;
}

bool CramFsReader::readDirectory(const Inode& inode, std::vector<Entry>* entries) const {
    if (!entries || !inode.isDirectory()) return false;
    const std::uint64_t start = std::uint64_t(inode.dataOffset) * 4;
    if (start + inode.size > imageSize_) return false;
    std::uint64_t pos = start;
    const std::uint64_t end = start + inode.size;
    while (pos < end) {
        if (end - pos < 12) return false;
        Inode child;
        if (!readInode(imageOffset_ + static_cast<std::int64_t>(pos), &child)) return false;
        const std::uint32_t nameBytes = std::uint32_t(child.nameLengthWords) * 4;
        if (nameBytes == 0 || pos + 12 + nameBytes > end || pos + 12 + nameBytes > imageSize_)
            return false;
        const std::vector<std::uint8_t> rawName = disc_->readRange(
            imageOffset_ + static_cast<std::int64_t>(pos + 12), nameBytes);
        if (rawName.size() != nameBytes) return false;
        std::size_t length = 0;
        while (length < rawName.size() && rawName[length] != 0) ++length;
        if (length == 0) return false;
        const std::string name(reinterpret_cast<const char*>(rawName.data()), length);
        if (name == "." || name == ".." || name.find('/') != std::string::npos) return false;
        Entry entry;
        entry.name = name;
        entry.inode = child;
        entries->push_back(std::move(entry));
        pos += 12 + nameBytes;
        if (entries->size() > 1000000) return false;
    }
    return pos == end;
}

bool CramFsReader::resolvePath(const std::string& path, Inode* inode) const {
    if (!inode) return false;
    *inode = root_;
    for (const std::string& part : splitPath(path)) {
        std::vector<Entry> children;
        if (!readDirectory(*inode, &children)) return false;
        const auto found = std::find_if(children.begin(), children.end(),
            [&part](const Entry& entry) { return entry.name == part; });
        if (found == children.end()) return false;
        *inode = found->inode;
    }
    return true;
}

std::vector<DiscEntry> CramFsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> result;
    Inode dir;
    if (!valid_ || !resolvePath(dirPath, &dir) || !dir.isDirectory()) return result;
    std::vector<Entry> entries;
    if (!readDirectory(dir, &entries)) return result;
    for (const Entry& entry : entries) {
        if (entry.inode.isSpecial()) {
            DiscEntry special(entry.name, false, 0);
            special.type = entry.inode.specialType();
            result.push_back(std::move(special));
            continue;
        }
        if (!entry.inode.isDirectory() && !entry.inode.isRegular() && !entry.inode.isSymlink())
            continue;
        result.push_back(DiscEntry(entry.name, entry.inode.isDirectory(), entry.inode.size));
    }
    return result;
}

ByteStorePtr CramFsReader::openFile(const std::string& path) const {
    Inode inode;
    if (!valid_ || !resolvePath(path, &inode) || (!inode.isRegular() && !inode.isSymlink()))
        return nullptr;
    return std::make_shared<CramFileStore>(disc_, imageOffset_, imageSize_, inode.dataOffset * 4,
        inode.size, bigEndian_, (flags_ & kFlagExtendedPointers) != 0,
        (flags_ & kFlagHoles) != 0);
}

}  // namespace fs
}  // namespace peare
