#include "XfsReader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kXfsMagic = 0x58465342U;
const std::uint32_t kBmapMagic = 0x424D4150U;
const std::uint32_t kBmapMagicV5 = 0x424D4133U;
const std::uint32_t kBlockDirMagic = 0x58443242U;
const std::uint32_t kBlockDirMagicV5 = 0x58444233U;
const std::uint32_t kLeafDirMagic = 0x58443244U;
const std::uint32_t kLeafDirMagicV5 = 0x58444433U;
const std::uint64_t kLeafOffset = 1ULL << 35;

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t be64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

std::uint32_t maskBits(int bits) {
    if (bits <= 0) return 0;
    if (bits >= 32) return 0xffffffffU;
    return (1U << bits) - 1U;
}

void splitPath(const std::string& path, std::vector<std::string>* out) {
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out->push_back(cur);
}

XfsReader::Extent parseExtent(const std::uint8_t* p) {
    XfsReader::Extent e;
    const std::uint64_t lower = be64(p + 8);
    const std::uint64_t middle = be64(p + 6);
    const std::uint64_t upper = be64(p);
    e.blockCount = static_cast<std::uint32_t>(lower & 0x001FFFFFULL);
    e.startBlock = (middle >> 5) & 0x000FFFFFFFFFFFFFULL;
    e.startOffset = (upper >> 9) & 0x003FFFFFFFFFFFFFULL;
    return e;
}

class XfsExtentStore final : public IByteStore {
public:
    XfsExtentStore(ByteStorePtr parent, std::int64_t length,
                   std::vector<XfsReader::Extent> extents, std::uint32_t blockSize,
                   std::uint8_t agBlocksLog2, std::uint32_t agBlocks)
        : parent_(std::move(parent)), length_(length), extents_(std::move(extents)),
          blockSize_(blockSize), agBlocksLog2_(agBlocksLog2), agBlocks_(agBlocks) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!parent_ || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t avail = length_ - pos;
        int want = count < avail ? count : static_cast<int>(avail);
        int done = 0;
        while (want > 0) {
            const std::uint64_t logicalBlock = static_cast<std::uint64_t>(pos + done) / blockSize_;
            const std::uint32_t inBlock = static_cast<std::uint32_t>((pos + done) % blockSize_);
            const XfsReader::Extent* found = nullptr;
            for (const XfsReader::Extent& e : extents_) {
                if (logicalBlock >= e.startOffset && logicalBlock < e.startOffset + e.blockCount) {
                    found = &e;
                    break;
                }
            }
            const int chunk = std::min<int>(want, static_cast<int>(blockSize_ - inBlock));
            if (!found) {
                std::fill(dst + done, dst + done + chunk, std::uint8_t(0));
            } else {
                const std::uint64_t physBlock =
                    found->startBlock + (logicalBlock - found->startOffset);
                parent_->readExactly(fsBlockOffset(physBlock) + inBlock, dst + done, chunk);
            }
            done += chunk;
            want -= chunk;
        }
        return done;
    }

private:
    std::int64_t fsBlockOffset(std::uint64_t fsBlock) const {
        const std::uint64_t ag = fsBlock >> agBlocksLog2_;
        const std::uint64_t rel = fsBlock & maskBits(agBlocksLog2_);
        return static_cast<std::int64_t>((ag * agBlocks_ + rel) * blockSize_);
    }

    ByteStorePtr parent_;
    std::int64_t length_;
    std::vector<XfsReader::Extent> extents_;
    std::uint32_t blockSize_;
    std::uint8_t agBlocksLog2_;
    std::uint32_t agBlocks_;
};

}  // namespace

XfsReader::XfsReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

void XfsReader::parse() {
    std::vector<std::uint8_t> sb = disc_ ? disc_->readRange(0, 512) : std::vector<std::uint8_t>();
    if (sb.size() < 264) { error_ = "Truncated XFS superblock"; return; }
    if (be32(sb.data()) != kXfsMagic) { error_ = "Not an XFS volume"; return; }

    blockSize_ = be32(sb.data() + 0x04);
    dataBlocks_ = be64(sb.data() + 0x08);
    rootInode_ = be64(sb.data() + 0x38);
    agBlocks_ = be32(sb.data() + 0x54);
    agCount_ = be32(sb.data() + 0x58);
    sbVersion_ = static_cast<std::uint16_t>(be16(sb.data() + 0x64) & 0x000F);
    inodeSize_ = be16(sb.data() + 0x68);
    blockSizeLog2_ = sb[0x78];
    inodeSizeLog2_ = sb[0x7A];
    inodesPerBlockLog2_ = sb[0x7B];
    agBlocksLog2_ = sb[0x7C];
    dirBlockLog2_ = sb[0xC0];
    const std::uint16_t versionFlags = be16(sb.data() + 0x64);
    const std::uint32_t features2 = be32(sb.data() + 0xC8);
    const std::uint32_t incompat = sbVersion_ >= 5 ? be32(sb.data() + 0xD8) : 0;
    hasFType_ = (sbVersion_ == 5 && (incompat & 0x00000001U) != 0) ||
                ((versionFlags & 0x0080U) != 0 && (features2 & 0x00000200U) != 0);

    if (blockSize_ < 512 || inodeSize_ < 128 || agBlocks_ == 0 || agCount_ == 0 ||
        blockSizeLog2_ == 0 || inodeSizeLog2_ == 0) {
        error_ = "Invalid XFS geometry";
        return;
    }
    const int agOffset = agBlocksLog2_ + inodesPerBlockLog2_;
    relativeInodeMask_ = maskBits(agOffset);
    agInodeMask_ = ~relativeInodeMask_;
    dirBlockSize_ = blockSize_ << dirBlockLog2_;

    const char* name = reinterpret_cast<const char*>(sb.data() + 0x6C);
    std::size_t n = 0;
    while (n < 12 && name[n] != '\0') ++n;
    if (n) friendly_ = std::string("XFS (") + std::string(name, n) + ")";
    valid_ = true;
}

std::int64_t XfsReader::fsBlockOffset(std::uint64_t fsBlock) const {
    const std::uint64_t ag = fsBlock >> agBlocksLog2_;
    const std::uint64_t rel = fsBlock & maskBits(agBlocksLog2_);
    return static_cast<std::int64_t>((ag * agBlocks_ + rel) * blockSize_);
}

std::uint64_t XfsReader::inodeOffset(std::uint64_t inodeNumber) const {
    const std::uint32_t ag =
        static_cast<std::uint32_t>((inodeNumber & agInodeMask_) >>
                                   (agBlocksLog2_ + inodesPerBlockLog2_));
    const std::uint32_t agBlock =
        static_cast<std::uint32_t>((inodeNumber >> inodesPerBlockLog2_) & maskBits(agBlocksLog2_));
    const std::uint32_t blockOffset =
        static_cast<std::uint32_t>(inodeNumber & maskBits(inodesPerBlockLog2_));
    return static_cast<std::uint64_t>(ag) * agBlocks_ * blockSize_ +
           static_cast<std::uint64_t>(agBlock) * blockSize_ +
           static_cast<std::uint64_t>(blockOffset) * inodeSize_;
}

XfsReader::Inode XfsReader::readInode(std::uint64_t inodeNumber) const {
    Inode in;
    std::vector<std::uint8_t> data = disc_->readRange(static_cast<std::int64_t>(inodeOffset(inodeNumber)),
                                                      inodeSize_);
    if (data.size() < 128 || be16(data.data()) != 0x494E) return in;
    in.mode = be16(data.data() + 0x02);
    in.version = data[0x04];
    in.format = data[0x05];
    in.length = be64(data.data() + 0x38);
    in.extentCount = be32(data.data() + 0x4C);
    const std::size_t dfOffset = in.version < 3 ? 0x64 : 0xB0;
    const std::uint8_t forkoff = data[0x52];
    std::size_t dfLength = forkoff == 0 ? data.size() - dfOffset : forkoff * 8;
    if (dfOffset > data.size()) return in;
    dfLength = std::min<std::size_t>(dfLength, data.size() - dfOffset);
    in.dataFork.assign(data.begin() + static_cast<std::ptrdiff_t>(dfOffset),
                       data.begin() + static_cast<std::ptrdiff_t>(dfOffset + dfLength));
    in.valid = true;
    return in;
}

std::vector<XfsReader::Extent> XfsReader::inodeExtents(const Inode& inode) const {
    if (inode.format == 2) {
        std::vector<Extent> out;
        for (std::uint32_t i = 0; i < inode.extentCount && i * 16 + 16 <= inode.dataFork.size(); ++i)
            out.push_back(parseExtent(inode.dataFork.data() + i * 16));
        return out;
    }
    if (inode.format == 3)
        return btreeRootExtents(inode.dataFork);
    return {};
}

std::vector<XfsReader::Extent> XfsReader::btreeRootExtents(const std::vector<std::uint8_t>& root) const {
    std::vector<Extent> out;
    if (root.size() < 4) return out;
    const std::uint16_t level = be16(root.data());
    const std::uint16_t records = be16(root.data() + 2);
    const std::size_t keyOffset = 4;
    const std::size_t ptrOffset = keyOffset + ((root.size() - keyOffset) / 16) * 8;
    for (std::uint16_t i = 0; i < records && ptrOffset + (i + 1) * 8 <= root.size(); ++i) {
        const std::uint64_t ptr = be64(root.data() + ptrOffset + i * 8);
        if (level == 0) break;
        readBtreeBlock(ptr, &out, level - 1);
    }
    std::sort(out.begin(), out.end(), [](const Extent& a, const Extent& b) {
        return a.startOffset < b.startOffset;
    });
    return out;
}

void XfsReader::readBtreeBlock(std::uint64_t fsBlock, std::vector<Extent>* out, int depth) const {
    if (depth < 0 || depth > 16) return;
    std::vector<std::uint8_t> block = disc_->readRange(fsBlockOffset(fsBlock), blockSize_);
    if (block.size() < 24) return;
    const std::uint32_t magic = be32(block.data());
    const bool v5 = magic == kBmapMagicV5;
    if (magic != kBmapMagic && !v5) return;
    const std::uint16_t level = be16(block.data() + 4);
    const std::uint16_t records = be16(block.data() + 6);
    const std::size_t header = v5 ? 72 : 24;
    if (level == 0) {
        for (std::uint16_t i = 0; i < records && header + (i + 1) * 16 <= block.size(); ++i)
            out->push_back(parseExtent(block.data() + header + i * 16));
        return;
    }
    const std::size_t ptrOffset = header + ((block.size() - header) / 16) * 8;
    for (std::uint16_t i = 0; i < records && ptrOffset + (i + 1) * 8 <= block.size(); ++i)
        readBtreeBlock(be64(block.data() + ptrOffset + i * 8), out, depth - 1);
}

ByteStorePtr XfsReader::inodeContent(const Inode& inode) const {
    if (inode.format == 1)
        return std::make_shared<MemoryStore>(inode.dataFork.data(), std::min<std::size_t>(inode.dataFork.size(), static_cast<std::size_t>(inode.length)));
    return std::make_shared<XfsExtentStore>(disc_, static_cast<std::int64_t>(inode.length),
                                            inodeExtents(inode), blockSize_, agBlocksLog2_, agBlocks_);
}

void XfsReader::readBlockDirectory(const std::vector<std::uint8_t>& data, std::vector<DirRec>* out) const {
    if (data.size() < 64) return;
    const std::uint32_t magic = be32(data.data());
    const bool block = magic == kBlockDirMagic || magic == kBlockDirMagicV5;
    const bool leaf = magic == kLeafDirMagic || magic == kLeafDirMagicV5;
    if (!block && !leaf) return;
    std::size_t offset = (magic == kBlockDirMagicV5 || magic == kLeafDirMagicV5) ? 0x30 : 0x04;
    offset += 3 * 4;
    if (magic == kBlockDirMagicV5 || magic == kLeafDirMagicV5) offset += 4;
    std::size_t eof = data.size();
    if (block && data.size() >= 8) {
        const std::uint32_t leafCount = be32(data.data() + data.size() - 8);
        eof = data.size() - 8 - static_cast<std::size_t>(leafCount) * 8;
    }
    while (offset + 10 <= eof) {
        if (data[offset] == 0xFF && data[offset + 1] == 0xFF) {
            const std::uint16_t len = be16(data.data() + offset + 2);
            if (len == 0) break;
            offset += len;
            continue;
        }
        const std::uint64_t ino = be64(data.data() + offset);
        const std::uint8_t nameLen = data[offset + 8];
        if (nameLen == 0 || offset + 9 + nameLen > eof) break;
        std::string name(reinterpret_cast<const char*>(data.data() + offset + 9), nameLen);
        if (name != "." && name != "..")
            out->push_back({name, ino});
        std::size_t size = 0x0B + nameLen + (hasFType_ ? 1 : 0);
        const std::size_t rem = size & 7;
        if (rem) size += 8 - rem;
        if (size == 0) break;
        offset += size;
    }
}

std::vector<XfsReader::DirRec> XfsReader::readDirectory(const Inode& dir) const {
    std::vector<DirRec> out;
    if (!dir.valid || !dir.isDirectory()) return out;
    if (dir.format == 1) {
        if (dir.dataFork.size() < 6) return out;
        const std::uint8_t count = dir.dataFork[0];
        const bool shortInode = dir.dataFork[1] == 0;
        std::size_t off = shortInode ? 6 : 10;
        for (std::uint8_t i = 0; i < count && off + 3 <= dir.dataFork.size(); ++i) {
            const std::uint8_t nameLen = dir.dataFork[off];
            off += 3;
            if (off + nameLen > dir.dataFork.size()) break;
            std::string name(reinterpret_cast<const char*>(dir.dataFork.data() + off), nameLen);
            off += nameLen;
            if (hasFType_) ++off;
            if (off + (shortInode ? 4 : 8) > dir.dataFork.size()) break;
            const std::uint64_t ino = shortInode ? be32(dir.dataFork.data() + off) : be64(dir.dataFork.data() + off);
            off += shortInode ? 4 : 8;
            if (name != "." && name != "..") out.push_back({name, ino});
        }
        return out;
    }

    const std::vector<Extent> extents = inodeExtents(dir);
    const std::uint64_t leafStart = kLeafOffset / blockSize_;
    for (const Extent& e : extents) {
        if (e.startOffset >= leafStart) continue;
        for (std::uint32_t i = 0; i < e.blockCount; ++i) {
            const std::int64_t pos = fsBlockOffset(e.startBlock + i);
            const std::vector<std::uint8_t> block = disc_->readRange(pos, dirBlockSize_);
            readBlockDirectory(block, &out);
        }
    }
    return out;
}

bool XfsReader::resolvePath(const std::string& path, Inode* out) const {
    if (!valid_) return false;
    Inode cur = readInode(rootInode_);
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& part : parts) {
        if (!cur.valid || !cur.isDirectory()) return false;
        bool found = false;
        for (const DirRec& rec : readDirectory(cur)) {
            if (rec.name == part) {
                cur = readInode(rec.inode);
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (out) *out = cur;
    return cur.valid;
}

std::vector<DiscEntry> XfsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    Inode dir;
    if (!resolvePath(dirPath, &dir) || !dir.isDirectory()) return out;
    for (const DirRec& rec : readDirectory(dir)) {
        const Inode child = readInode(rec.inode);
        if (!child.valid) continue;
        DiscEntry e;
        e.name = rec.name;
        e.isDirectory = child.isDirectory();
        e.length = child.isDirectory() ? 0 : static_cast<std::int64_t>(child.length);
        out.push_back(e);
    }
    return out;
}

ByteStorePtr XfsReader::openFile(const std::string& path) const {
    Inode in;
    if (!resolvePath(path, &in) || (!in.isRegular() && !in.isSymlink())) return nullptr;
    return inodeContent(in);
}

}  // namespace fs
}  // namespace peare
