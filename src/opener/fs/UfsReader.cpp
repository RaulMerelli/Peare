#include "UfsReader.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace peare {
namespace fs {
namespace {

constexpr std::int64_t kSuperblockSize = 8192;
constexpr std::int64_t kUfs1SuperblockOffset = 8192;
constexpr std::int64_t kUfs2SuperblockOffset = 65536;
constexpr std::uint32_t kUfs1Magic = 0x00011954;
constexpr std::uint32_t kUfs2Magic = 0x19540119;
constexpr std::uint32_t kUfs1InodeSize = 128;
constexpr std::uint32_t kUfs2InodeSize = 256;
constexpr std::uint32_t kDirectCount = 12;
constexpr std::uint32_t kIndirectCount = 3;
constexpr std::uint32_t kDirectoryBlockSize = 512;

std::uint16_t load16(const std::uint8_t* p, bool bigEndian) {
    return bigEndian
        ? static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1])
        : static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}

std::uint32_t load32(const std::uint8_t* p, bool bigEndian) {
    if (bigEndian)
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
            (std::uint32_t(p[2]) << 8) | p[3];
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t load64(const std::uint8_t* p, bool bigEndian) {
    const std::uint64_t a = load32(p, bigEndian);
    const std::uint64_t b = load32(p + 4, bigEndian);
    return bigEndian ? (a << 32) | b : a | (b << 32);
}

bool plausibleSuperblock(const std::uint8_t* data, std::size_t size,
                         bool bigEndian, bool ufs2) {
    const std::uint32_t magic = ufs2 ? kUfs2Magic : kUfs1Magic;
    if (size < 1376 || load32(data + 1372, bigEndian) != magic)
        return false;
    const std::uint32_t blockSize = load32(data + 48, bigEndian);
    const std::uint32_t fragmentSize = load32(data + 52, bigEndian);
    const std::uint32_t fragmentCount = load32(data + 56, bigEndian);
    const std::uint32_t groups = load32(data + 44, bigEndian);
    const std::uint32_t inodesPerGroup = load32(data + 184, bigEndian);
    const std::uint32_t fragmentsPerGroup = load32(data + 188, bigEndian);
    return blockSize >= 4096 && blockSize <= 65536 &&
        (blockSize & (blockSize - 1)) == 0 && fragmentSize >= 512 &&
        fragmentSize <= blockSize && (fragmentSize & (fragmentSize - 1)) == 0 &&
        blockSize / fragmentSize == fragmentCount && fragmentCount <= 8 &&
        groups > 0 && groups <= 65536 && inodesPerGroup > 0 &&
        fragmentsPerGroup > 0;
}

bool hasUfsSuperblock(const IByteStore& disc) {
    const struct Candidate { std::int64_t offset; bool ufs2; } candidates[] = {
        {kUfs2SuperblockOffset, true}, {kUfs1SuperblockOffset, false},
        {0, false}, {262144, false}
    };
    for (const Candidate& candidate : candidates) {
        const std::int64_t offset = candidate.offset;
        if (offset < 0 || offset + 1376 > disc.capacity())
            continue;
        std::uint8_t header[1376];
        if (disc.read(offset, header, sizeof(header)) != sizeof(header))
            continue;
        if (plausibleSuperblock(header, sizeof(header), true, candidate.ufs2) ||
            plausibleSuperblock(header, sizeof(header), false, candidate.ufs2))
            return true;
    }
    return false;
}

void splitPath(const std::string& path, std::vector<std::string>* parts) {
    std::string current;
    for (const char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (!current.empty()) {
                parts->push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        parts->push_back(current);
}

class UfsFileStore final : public IByteStore {
public:
    UfsFileStore(ByteStorePtr disc, bool bigEndian, std::uint32_t blockSize,
                 std::uint32_t fragmentSize, std::uint32_t addressSize,
                 std::uint64_t size, std::vector<std::uint64_t> direct,
                 std::vector<std::uint64_t> indirect)
        : disc_(std::move(disc)), bigEndian_(bigEndian), blockSize_(blockSize),
          fragmentSize_(fragmentSize), addressSize_(addressSize), size_(size),
          direct_(std::move(direct)),
          indirect_(std::move(indirect)) {}

    std::int64_t capacity() const override {
        return size_ > static_cast<std::uint64_t>(INT64_MAX)
            ? INT64_MAX : static_cast<std::int64_t>(size_);
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!disc_ || !dst || pos < 0 || count <= 0 ||
            static_cast<std::uint64_t>(pos) >= size_)
            return 0;
        const std::uint64_t remaining = size_ - static_cast<std::uint64_t>(pos);
        const int wanted = static_cast<int>(std::min<std::uint64_t>(count, remaining));
        int produced = 0;
        const std::uint32_t pointersPerBlock = blockSize_ / addressSize_;
        while (produced < wanted) {
            const std::uint64_t offset = static_cast<std::uint64_t>(pos) + produced;
            const std::uint64_t logicalBlock = offset / blockSize_;
            const std::uint32_t blockOffset = static_cast<std::uint32_t>(offset % blockSize_);
            const int amount = static_cast<int>(std::min<std::uint64_t>(
                blockSize_ - blockOffset, wanted - produced));
            std::uint64_t fragment = 0;
            if (!blockPointer(logicalBlock, pointersPerBlock, &fragment))
                return produced;
            if (fragment == 0) {
                std::memset(dst + produced, 0, static_cast<std::size_t>(amount));
            } else {
                const std::uint64_t physical = std::uint64_t(fragment) * fragmentSize_ + blockOffset;
                if (physical > static_cast<std::uint64_t>(disc_->capacity()) ||
                    amount > disc_->capacity() - static_cast<std::int64_t>(physical))
                    return produced;
                const int got = disc_->read(static_cast<std::int64_t>(physical), dst + produced, amount);
                if (got != amount)
                    return produced + std::max(got, 0);
            }
            produced += amount;
        }
        return produced;
    }

private:
    bool readIndirect(std::uint64_t fragment, const std::uint8_t** bytes) const {
        if (fragment == 0) {
            *bytes = nullptr;
            return true;
        }
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto found = indirectCache_.find(fragment);
        if (found == indirectCache_.end()) {
            std::vector<std::uint8_t> block(blockSize_);
            if (fragment > UINT64_MAX / fragmentSize_)
                return false;
            const std::uint64_t offset = fragment * fragmentSize_;
            if (offset > static_cast<std::uint64_t>(disc_->capacity()) ||
                blockSize_ > disc_->capacity() - static_cast<std::int64_t>(offset) ||
                disc_->read(static_cast<std::int64_t>(offset), block.data(),
                    static_cast<int>(block.size())) != static_cast<int>(block.size()))
                return false;
            found = indirectCache_.emplace(fragment, std::move(block)).first;
        }
        *bytes = found->second.data();
        return true;
    }

    std::uint64_t pointerAt(std::uint64_t fragment, std::uint32_t index,
                            std::uint32_t pointersPerBlock, bool* ok) const {
        const std::uint8_t* bytes = nullptr;
        if (!readIndirect(fragment, &bytes)) {
            *ok = false;
            return 0;
        }
        if (!bytes)
            return 0;
        const std::uint8_t* value = bytes + std::size_t(index) * addressSize_;
        return addressSize_ == 8 ? load64(value, bigEndian_) : load32(value, bigEndian_);
    }

    bool blockPointer(std::uint64_t logicalBlock, std::uint32_t pointersPerBlock,
                      std::uint64_t* fragment) const {
        if (logicalBlock < direct_.size()) {
            *fragment = direct_[static_cast<std::size_t>(logicalBlock)];
            return true;
        }
        logicalBlock -= direct_.size();
        bool ok = true;
        if (logicalBlock < pointersPerBlock) {
            *fragment = pointerAt(indirect_[0], static_cast<std::uint32_t>(logicalBlock),
                                  pointersPerBlock, &ok);
            return ok;
        }
        logicalBlock -= pointersPerBlock;
        const std::uint64_t doubleLimit = std::uint64_t(pointersPerBlock) * pointersPerBlock;
        if (logicalBlock < doubleLimit) {
            const std::uint32_t first = static_cast<std::uint32_t>(logicalBlock / pointersPerBlock);
            const std::uint32_t second = static_cast<std::uint32_t>(logicalBlock % pointersPerBlock);
            const std::uint64_t leaf = pointerAt(indirect_[1], first, pointersPerBlock, &ok);
            *fragment = ok ? pointerAt(leaf, second, pointersPerBlock, &ok) : 0;
            return ok;
        }
        logicalBlock -= doubleLimit;
        const std::uint64_t tripleLimit = doubleLimit * pointersPerBlock;
        if (logicalBlock >= tripleLimit)
            return false;
        const std::uint32_t first = static_cast<std::uint32_t>(
            logicalBlock / doubleLimit);
        const std::uint32_t second = static_cast<std::uint32_t>(
            (logicalBlock / pointersPerBlock) % pointersPerBlock);
        const std::uint32_t third = static_cast<std::uint32_t>(logicalBlock % pointersPerBlock);
        const std::uint64_t middle = pointerAt(indirect_[2], first, pointersPerBlock, &ok);
        const std::uint64_t leaf = ok
            ? pointerAt(middle, second, pointersPerBlock, &ok) : 0;
        *fragment = ok ? pointerAt(leaf, third, pointersPerBlock, &ok) : 0;
        return ok;
    }

    ByteStorePtr disc_;
    bool bigEndian_;
    std::uint32_t blockSize_;
    std::uint32_t fragmentSize_;
    std::uint32_t addressSize_;
    std::uint64_t size_;
    std::vector<std::uint64_t> direct_;
    std::vector<std::uint64_t> indirect_;
    mutable std::mutex cacheMutex_;
    mutable std::map<std::uint64_t, std::vector<std::uint8_t>> indirectCache_;
};

}  // namespace

UfsReader::UfsReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try {
        valid_ = parse();
    } catch (...) {
        valid_ = false;
        error_ = "UFS parse error";
    }
}

bool UfsReader::detect(const IByteStore& disc) {
    return hasUfsSuperblock(disc);
}

bool UfsReader::detect(const std::uint8_t* data, std::size_t size) {
    if (!data)
        return false;
    const struct Candidate { std::size_t offset; bool ufs2; } candidates[] = {
        {65536, true}, {8192, false}, {0, false}, {262144, false}
    };
    for (const Candidate& candidate : candidates) {
        const std::size_t offset = candidate.offset;
        if (offset <= size && size - offset >= 1376 &&
            (plausibleSuperblock(data + offset, size - offset, true, candidate.ufs2) ||
             plausibleSuperblock(data + offset, size - offset, false, candidate.ufs2)))
            return true;
    }
    return false;
}

std::uint16_t UfsReader::read16(const std::uint8_t* p) const {
    return load16(p, geometry_.bigEndian);
}

std::uint32_t UfsReader::read32(const std::uint8_t* p) const {
    return load32(p, geometry_.bigEndian);
}

std::uint64_t UfsReader::read64(const std::uint8_t* p) const {
    return load64(p, geometry_.bigEndian);
}

bool UfsReader::parse() {
    if (!disc_) {
        error_ = "No UFS image";
        return false;
    }
    const struct Candidate { std::int64_t offset; bool ufs2; } candidates[] = {
        {kUfs2SuperblockOffset, true}, {kUfs1SuperblockOffset, false},
        {0, false}, {262144, false}
    };
    std::vector<std::uint8_t> superblock;
    for (const Candidate& candidate : candidates) {
        const std::int64_t offset = candidate.offset;
        if (offset < 0 || offset + kSuperblockSize > disc_->capacity())
            continue;
        std::vector<std::uint8_t> block = disc_->readRange(offset, kSuperblockSize);
        if (block.size() < 1376)
            continue;
        if (plausibleSuperblock(block.data(), block.size(), true, candidate.ufs2)) {
            geometry_.bigEndian = true;
            geometry_.ufs2 = candidate.ufs2;
            superblock = std::move(block);
        } else if (plausibleSuperblock(block.data(), block.size(), false, candidate.ufs2)) {
            geometry_.bigEndian = false;
            geometry_.ufs2 = candidate.ufs2;
            superblock = std::move(block);
        }
        if (!superblock.empty()) {
            geometry_.superOffset = offset;
            break;
        }
    }
    if (superblock.empty()) {
        error_ = "UFS superblock not found";
        return false;
    }
    const auto* p = superblock.data();
    geometry_.superBlockFragments = read32(p + 8);
    geometry_.cylinderGroupOffset = static_cast<std::int32_t>(read32(p + 24));
    geometry_.cylinderGroupMask = static_cast<std::int32_t>(read32(p + 28));
    geometry_.cylinderGroups = read32(p + 44);
    geometry_.blockSize = read32(p + 48);
    geometry_.fragmentSize = read32(p + 52);
    geometry_.fragmentsPerBlock = read32(p + 56);
    geometry_.inodesPerBlock = read32(p + 120);
    geometry_.inodeTableFragment = read32(p + 16);
    geometry_.inodesPerGroup = read32(p + 184);
    geometry_.fragmentsPerGroup = read32(p + 188);
    geometry_.inodeSize = geometry_.ufs2 ? kUfs2InodeSize : kUfs1InodeSize;
    geometry_.maxSymlinkLength = geometry_.ufs2 ? read32(p + 1320) : 60;
    const std::uint32_t expectedInodesPerBlock = geometry_.blockSize / geometry_.inodeSize;
    const std::uint64_t totalFragments =
        std::uint64_t(geometry_.cylinderGroups) * geometry_.fragmentsPerGroup;
    const std::uint64_t availableFragments = static_cast<std::uint64_t>(disc_->capacity()) /
        geometry_.fragmentSize;
    if (geometry_.inodesPerBlock != expectedInodesPerBlock ||
        geometry_.fragmentsPerBlock != geometry_.blockSize / geometry_.fragmentSize ||
        geometry_.cylinderGroups == 0 || geometry_.inodesPerGroup == 0 ||
        geometry_.fragmentsPerGroup == 0 ||
        totalFragments > availableFragments + geometry_.fragmentsPerGroup ||
        geometry_.inodeTableFragment >= geometry_.fragmentsPerGroup ||
        geometry_.maxSymlinkLength > (kDirectCount + kIndirectCount) *
            (geometry_.ufs2 ? 8U : 4U)) {
        error_ = geometry_.ufs2 ? "Invalid UFS2 geometry" : "Invalid UFS1 geometry";
        return false;
    }
    friendly_ = geometry_.ufs2 ? "UFS2" : "UFS1";
    return true;
}

std::int64_t UfsReader::cylinderGroupStart(std::uint32_t group) const {
    const std::int64_t base = std::int64_t(group) * geometry_.fragmentsPerGroup;
    if (geometry_.ufs2)
        return base;
    const std::uint32_t mask = static_cast<std::uint32_t>(geometry_.cylinderGroupMask);
    const std::int64_t skipped = std::int64_t(geometry_.cylinderGroupOffset) *
        (group & ~mask);
    return base + skipped;
}

std::int64_t UfsReader::inodeOffset(std::uint32_t inode) const {
    const std::uint32_t group = inode / geometry_.inodesPerGroup;
    const std::uint32_t local = inode % geometry_.inodesPerGroup;
    if (group >= geometry_.cylinderGroups)
        return -1;
    const std::int64_t inodeBlock = (local / geometry_.inodesPerBlock) *
        geometry_.fragmentsPerBlock;
    const std::int64_t fragment = cylinderGroupStart(group) +
        geometry_.inodeTableFragment + inodeBlock;
    if (fragment < 0 || fragment > INT64_MAX / geometry_.fragmentSize)
        return -1;
    const std::int64_t blockOffset = fragment * geometry_.fragmentSize;
    const std::int64_t inodeOffsetInBlock =
        std::int64_t(local % geometry_.inodesPerBlock) * geometry_.inodeSize;
    if (blockOffset > INT64_MAX - inodeOffsetInBlock)
        return -1;
    return blockOffset + inodeOffsetInBlock;
}

bool UfsReader::readNode(std::uint32_t inode, Node* out) const {
    if (!out)
        return false;
    const std::int64_t offset = inodeOffset(inode);
    if (offset < 0 || offset > disc_->capacity() - geometry_.inodeSize)
        return false;
    std::uint8_t raw[kUfs2InodeSize];
    if (disc_->read(offset, raw, static_cast<int>(geometry_.inodeSize)) !=
        static_cast<int>(geometry_.inodeSize))
        return false;
    Node node;
    node.inode = inode;
    node.mode = read16(raw);
    node.size = read64(raw + (geometry_.ufs2 ? 16 : 8));
    const std::uint16_t type = node.mode & 0170000;
    node.isDirectory = type == 0040000;
    node.isSymlink = type == 0120000;
    node.special = type != 0040000 && type != 0100000 && !node.isSymlink;
    if (node.mode == 0)
        return false;
    const std::uint32_t pointerSize = geometry_.ufs2 ? 8 : 4;
    const std::uint32_t pointerOffset = geometry_.ufs2 ? 112 : 40;
    node.direct.reserve(kDirectCount);
    for (std::uint32_t i = 0; i < kDirectCount; ++i)
        node.direct.push_back(pointerSize == 8
            ? read64(raw + pointerOffset + i * pointerSize)
            : read32(raw + pointerOffset + i * pointerSize));
    node.indirect.reserve(kIndirectCount);
    for (std::uint32_t i = 0; i < kIndirectCount; ++i)
        node.indirect.push_back(pointerSize == 8
            ? read64(raw + pointerOffset + kDirectCount * pointerSize + i * pointerSize)
            : read32(raw + pointerOffset + kDirectCount * pointerSize + i * pointerSize));
    node.inlineSymlink = node.isSymlink && node.size <= geometry_.maxSymlinkLength;
    if (node.inlineSymlink)
        node.inlineData.assign(raw + pointerOffset,
            raw + pointerOffset + static_cast<std::size_t>(node.size));
    *out = std::move(node);
    return true;
}

std::vector<DiscEntry> UfsReader::readDirectory(
    const Node& dir, std::map<std::string, std::uint32_t>* inodes) const {
    std::vector<DiscEntry> entries;
    if (!dir.isDirectory || dir.size > static_cast<std::uint64_t>(INT64_MAX))
        return entries;
    UfsFileStore content(disc_, geometry_.bigEndian, geometry_.blockSize,
                         geometry_.fragmentSize, geometry_.ufs2 ? 8 : 4,
                         dir.size, dir.direct, dir.indirect);
    const std::uint64_t chunkSize = 64 * 1024;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(chunkSize));
    std::uint64_t base = 0;
    while (base < dir.size) {
        const std::uint64_t request = std::min(chunkSize, dir.size - base);
        const int got = content.read(static_cast<std::int64_t>(base), bytes.data(),
                                     static_cast<int>(request));
        if (got <= 0)
            break;
        const std::size_t limit = static_cast<std::size_t>(got);
        std::size_t pos = 0;
        while (pos + 8 <= limit) {
            const std::uint32_t inode = read32(bytes.data() + pos);
            const std::uint16_t recordLength = read16(bytes.data() + pos + 4);
            const std::uint16_t nameLength = geometry_.ufs2
                ? bytes[pos + 7] : read16(bytes.data() + pos + 6);
            const std::size_t blockOffset = static_cast<std::size_t>((base + pos) % kDirectoryBlockSize);
            if (recordLength < 8 || (recordLength & 3) != 0 ||
                recordLength > kDirectoryBlockSize - blockOffset ||
                pos + recordLength > limit) {
                pos += kDirectoryBlockSize - blockOffset;
                continue;
            }
            if (inode != 0 && nameLength > 0 && nameLength <= 255 &&
                nameLength <= recordLength - 8) {
                const char* nameData = reinterpret_cast<const char*>(bytes.data() + pos + 8);
                const std::string name(nameData, nameLength);
                if (name != "." && name != "..") {
                    Node node;
                    if (readNode(inode, &node)) {
                        if (node.special)
                            node.size = 0;
                        entries.emplace_back(name, node.isDirectory,
                            node.isDirectory ? 0 : static_cast<std::int64_t>(
                                std::min<std::uint64_t>(node.size, INT64_MAX)));
                        if (inodes)
                            (*inodes)[name] = inode;
                    }
                }
            }
            pos += recordLength;
        }
        base += static_cast<std::uint64_t>(got);
        if (got < static_cast<int>(request))
            break;
    }
    return entries;
}

bool UfsReader::resolve(const std::string& path, Node* out) const {
    Node current;
    if (!readNode(2, &current) || !current.isDirectory)
        return false;
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& part : parts) {
        if (part == ".")
            continue;
        if (part == "..")
            return false;
        std::map<std::string, std::uint32_t> inodes;
        readDirectory(current, &inodes);
        const auto found = inodes.find(part);
        if (found == inodes.end() || !readNode(found->second, &current))
            return false;
    }
    if (out)
        *out = current;
    return true;
}

std::vector<DiscEntry> UfsReader::list(const std::string& dirPath) const {
    Node dir;
    if (!valid_ || !resolve(dirPath, &dir) || !dir.isDirectory)
        return {};
    return readDirectory(dir);
}

ByteStorePtr UfsReader::openFile(const std::string& path) const {
    Node node;
    if (!valid_ || !resolve(path, &node) || node.isDirectory)
        return ByteStorePtr();
    if (node.inlineSymlink)
        return std::make_shared<MemoryStore>(std::move(node.inlineData));
    if (node.special)
        return std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
    return std::make_shared<UfsFileStore>(disc_, geometry_.bigEndian, geometry_.blockSize,
        geometry_.fragmentSize, geometry_.ufs2 ? 8 : 4, node.size,
        std::move(node.direct), std::move(node.indirect));
}

}  // namespace fs
}  // namespace peare
