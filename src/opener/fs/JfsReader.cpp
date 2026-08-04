#include "JfsReader.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::int64_t kPrimarySuperOffset = 0x8000;
const std::int64_t kSecondarySuperOffset = 0xF000;
const std::int64_t kPrimaryAitOffset = 0xB000;
const std::uint32_t kFilesystemInode = 16;
const std::uint32_t kRootInode = 2;
const std::size_t kInodeSize = 512;
const std::size_t kPageSize = 4096;
const std::size_t kIagInoextOffset = 3072;
const int kInodesPerIag = 4096;
const int kInodesPerExtent = 32;
const int kExtentsPerIag = 128;
const std::uint8_t kBtRoot = 0x01;
const std::uint8_t kBtLeaf = 0x02;
const std::uint8_t kBtInternal = 0x04;
const std::uint8_t kXadNotRecorded = 0x08;
const std::uint32_t kJfsDirIndex = 0x00200000U;
const std::uint32_t kJfsOs2 = 0x40000000U;
const std::uint32_t kOs2Directory = 0x20000000U;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) |
           (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::uint32_t pxdLength(const std::uint8_t* p) {
    return le32(p) & 0x00FFFFFFU;
}

std::uint64_t pxdAddress(const std::uint8_t* p) {
    const std::uint64_t hi = static_cast<std::uint64_t>(le32(p) >> 24);
    return (hi << 32) | le32(p + 4);
}

std::uint64_t xadOffset(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(p[3]) << 32) | le32(p + 4);
}

void splitPath(const std::string& path, std::vector<std::string>* out) {
    std::string current;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                out->push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out->push_back(current);
}

bool asciiEqualIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ac = static_cast<unsigned char>(a[i]);
        unsigned char bc = static_cast<unsigned char>(b[i]);
        if (ac >= 'A' && ac <= 'Z') ac = static_cast<unsigned char>(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z') bc = static_cast<unsigned char>(bc + ('a' - 'A'));
        if (ac != bc) return false;
    }
    return true;
}

void appendUtf8(std::uint32_t cp, std::string* out) {
    if (cp <= 0x7F) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16ToUtf8(const std::vector<std::uint16_t>& chars) {
    std::string out;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        std::uint32_t cp = chars[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < chars.size()) {
            const std::uint32_t low = chars[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        appendUtf8(cp, &out);
    }
    return out;
}

class JfsExtentStore final : public IByteStore {
public:
    JfsExtentStore(ByteStorePtr parent, std::int64_t length,
                   std::vector<JfsReader::Extent> extents, std::uint32_t blockSize)
        : parent_(std::move(parent)), length_(length), extents_(std::move(extents)),
          blockSize_(blockSize) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!parent_ || !dst || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t remain = length_ - pos;
        int want = count < remain ? count : static_cast<int>(remain);
        int done = 0;
        while (done < want) {
            const std::uint64_t logical = static_cast<std::uint64_t>(pos + done) / blockSize_;
            const std::uint32_t inBlock = static_cast<std::uint32_t>((pos + done) % blockSize_);
            const int chunk = std::min<int>(want - done, static_cast<int>(blockSize_ - inBlock));
            const JfsReader::Extent* found = nullptr;
            for (const JfsReader::Extent& e : extents_) {
                if (logical >= e.logicalBlock && logical < e.logicalBlock + e.blockCount) {
                    found = &e;
                    break;
                }
            }
            if (!found || (found->flags & kXadNotRecorded)) {
                std::fill(dst + done, dst + done + chunk, std::uint8_t(0));
            } else {
                const std::uint64_t physical = found->physicalBlock + (logical - found->logicalBlock);
                const std::int64_t source = static_cast<std::int64_t>(physical * blockSize_ + inBlock);
                int got = parent_->read(source, dst + done, chunk);
                if (got < 0) got = 0;
                if (got < chunk)
                    std::fill(dst + done + got, dst + done + chunk, std::uint8_t(0));
            }
            done += chunk;
        }
        return done;
    }

private:
    ByteStorePtr parent_;
    std::int64_t length_;
    std::vector<JfsReader::Extent> extents_;
    std::uint32_t blockSize_;
};

}  // namespace

JfsReader::JfsReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

void JfsReader::parse() {
    if (!disc_ || disc_->capacity() < kPrimarySuperOffset + 184) {
        error_ = "Truncated JFS image";
        return;
    }

    std::vector<std::uint8_t> sb = disc_->readRange(kPrimarySuperOffset, kPageSize);
    if (!parseSuperblock(sb)) {
        sb = disc_->readRange(kSecondarySuperOffset, kPageSize);
        if (!parseSuperblock(sb)) {
            error_ = "JFS superblock not found";
            return;
        }
    }

    // Both aggregate inode tables are authoritative copies.  OS/2 systems can
    // leave the primary table readable but stale after a recovery; selecting it
    // merely because inode 16 has a valid header hides most of the fileset.  Try
    // both tables and keep the map that resolves the richest valid root tree.
    std::vector<Extent> bestMap;
    int bestScore = -1;
    for (int secondary = 0; secondary < 2; ++secondary) {
        if (secondary && secondaryAitBlock_ == 0) continue;
        const Inode imap = readAggregateInode(kFilesystemInode, secondary != 0);
        if (!imap.valid) continue;
        const std::vector<Extent> candidate = inodeExtents(imap);
        if (candidate.empty()) continue;
        inodeMapExtents_ = candidate;
        const Inode candidateRoot = readFilesetInode(kRootInode);
        if (!candidateRoot.valid || !inodeIsDirectory(candidateRoot)) continue;
        // A stale fileset map can still expose a plausible root and one or two
        // directories. Score several levels so the usable AIT copy wins.
        const int score = scoreFilesetTree(4, 4096);
        if (score > bestScore) {
            bestScore = score;
            bestMap = candidate;
        }
    }
    inodeMapExtents_ = bestMap;
    qualityScore_ = bestScore;
    if (inodeMapExtents_.empty()) {
        error_ = "JFS fileset inode map is invalid";
        return;
    }

    const Inode root = readFilesetInode(kRootInode);
    if (!root.valid || !inodeIsDirectory(root)) {
        error_ = "JFS root inode is invalid";
        return;
    }

    const std::size_t labelOffset = version_ == 1 ? 101 : 152;
    const std::size_t labelLength = version_ == 1 ? 11 : 16;
    const std::string label(
        reinterpret_cast<const char*>(sb.data() + labelOffset),
        std::min<std::size_t>(labelLength,
                              sb.size() > labelOffset ? sb.size() - labelOffset : 0));
    std::string cleanLabel = label.substr(0, label.find('\0'));
    friendly_ = "JFS";
    valid_ = true;
}

bool JfsReader::parseSuperblock(const std::vector<std::uint8_t>& sb) {
    if (sb.size() < 184 || std::memcmp(sb.data(), "JFS1", 4) != 0) return false;
    const std::uint32_t version = le32(sb.data() + 4);
    const std::uint32_t blockSize = le32(sb.data() + 16);
    const std::uint16_t shift = le16(sb.data() + 20);
    if ((version != 1 && version != 2) || blockSize < 512 || blockSize > 4096 ||
        (blockSize & (blockSize - 1)) != 0 || shift < 9 || shift > 12 ||
        (1U << shift) != blockSize)
        return false;

    version_ = version;
    blockSize_ = blockSize;
    blockShift_ = shift;
    const std::uint64_t aggregateUnits = le64(sb.data() + 8);
    const std::uint32_t physicalBlockSize = le32(sb.data() + 24);
    const std::uint16_t physicalBlockShift = le16(sb.data() + 28);
    std::uint32_t aggregateUnitSize = 512;
    if (physicalBlockSize >= 512 && physicalBlockSize <= 4096 &&
        (physicalBlockSize & (physicalBlockSize - 1)) == 0 &&
        physicalBlockShift >= 9 && physicalBlockShift <= 12 &&
        (1U << physicalBlockShift) == physicalBlockSize)
        aggregateUnitSize = physicalBlockSize;
    if (aggregateUnits == 0 ||
        aggregateUnits > static_cast<std::uint64_t>(INT64_MAX) / aggregateUnitSize)
        return false;
    aggregateBytes_ = aggregateUnits * aggregateUnitSize;

    flags_ = le32(sb.data() + 36);
    directoryIndex_ = (flags_ & kJfsDirIndex) != 0;
    caseInsensitive_ = (flags_ & kJfsOs2) != 0;
    secondaryAitBlock_ = pxdAddress(sb.data() + 48);
    return true;
}

std::int64_t JfsReader::blockOffset(std::uint64_t block) const {
    if (blockSize_ == 0 || block > static_cast<std::uint64_t>(INT64_MAX) / blockSize_)
        return -1;
    return static_cast<std::int64_t>(block * blockSize_);
}

JfsReader::Inode JfsReader::readAggregateInode(std::uint32_t number, bool secondary) const {
    Inode out;
    if (number >= kInodesPerExtent || blockSize_ == 0) return out;
    std::int64_t base = kPrimaryAitOffset;
    if (secondary) {
        base = blockOffset(secondaryAitBlock_);
        if (base < 0) return out;
    }
    const std::int64_t pos = base + static_cast<std::int64_t>(number) * kInodeSize;
    out.raw = disc_->readRange(pos, kInodeSize);
    if (out.raw.size() != kInodeSize) return Inode();
    out.number = le32(out.raw.data() + 8);
    out.size = le64(out.raw.data() + 24);
    out.blocks = le64(out.raw.data() + 32);
    out.mode = le32(out.raw.data() + 52);
    const std::uint32_t links = le32(out.raw.data() + 40);
    if (out.number != number || links == 0) return Inode();
    out.valid = true;
    return out;
}

bool JfsReader::mapLogicalBlock(const std::vector<Extent>& extents, std::uint64_t logical,
                                std::uint64_t* physical) const {
    for (const Extent& e : extents) {
        if (logical >= e.logicalBlock && logical < e.logicalBlock + e.blockCount &&
            !(e.flags & kXadNotRecorded)) {
            if (physical) *physical = e.physicalBlock + logical - e.logicalBlock;
            return true;
        }
    }
    return false;
}

bool JfsReader::readIag(std::uint32_t iagNumber, std::vector<std::uint8_t>* out) const {
    if (!out || blockSize_ == 0 || kPageSize % blockSize_ != 0) return false;
    const std::uint64_t blocksPerPage = kPageSize / blockSize_;
    const std::uint64_t firstLogical =
        (static_cast<std::uint64_t>(iagNumber) + 1) * blocksPerPage;
    out->assign(kPageSize, std::uint8_t(0));
    for (std::uint64_t i = 0; i < blocksPerPage; ++i) {
        std::uint64_t physical = 0;
        if (!mapLogicalBlock(inodeMapExtents_, firstLogical + i, &physical)) return false;
        const int got = disc_->read(blockOffset(physical),
                                    out->data() + static_cast<std::size_t>(i * blockSize_),
                                    static_cast<int>(blockSize_));
        if (got != static_cast<int>(blockSize_)) return false;
    }
    return true;
}

int JfsReader::scoreFilesetTree(int maxDepth, int maxNodes) const {
    if (maxDepth < 0 || maxNodes <= 0) return -1;
    struct Pending {
        std::uint32_t inode;
        int depth;
    };
    std::deque<Pending> pending;
    std::set<std::uint32_t> visited;
    pending.push_back(Pending{kRootInode, 0});
    int score = 0;
    int nodes = 0;

    while (!pending.empty() && nodes < maxNodes) {
        const Pending item = pending.front();
        pending.pop_front();
        if (!visited.insert(item.inode).second) continue;
        ++nodes;

        const Inode inode = readFilesetInode(item.inode);
        if (!inode.valid) {
            score -= 4;
            continue;
        }
        score += 8;
        if (inodeIsDirectory(inode)) {
            score += 8;
            const std::vector<DirRec> entries = readDirectory(inode);
            score += std::min<int>(static_cast<int>(entries.size()), 512);
            if (!entries.empty()) score += 4;
            if (item.depth < maxDepth) {
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (visited.find(entries[i].inode) == visited.end())
                        pending.push_back(Pending{entries[i].inode, item.depth + 1});
                }
            }
        } else if (inodeIsRegular(inode) || inodeIsSymlink(inode)) {
            score += 4;
            if (inode.size != 0) score += 1;
        } else {
            score -= 2;
        }
    }
    return score;
}

JfsReader::Inode JfsReader::readFilesetInode(std::uint32_t number) const {
    Inode out;
    const std::uint32_t iagNumber = number / kInodesPerIag;
    const std::uint32_t relative = number % kInodesPerIag;
    const std::uint32_t extentNo = relative / kInodesPerExtent;
    const std::uint32_t inExtent = relative % kInodesPerExtent;
    if (extentNo >= kExtentsPerIag) return out;

    std::vector<std::uint8_t> iag;
    if (!readIag(iagNumber, &iag)) return out;
    const std::size_t pxd = kIagInoextOffset + static_cast<std::size_t>(extentNo) * 8;
    if (pxd + 8 > iag.size()) return out;

    const std::uint32_t extentBlocks = pxdLength(iag.data() + pxd);
    const std::uint64_t extentBlock = pxdAddress(iag.data() + pxd);
    const std::uint32_t neededBlocks = static_cast<std::uint32_t>(
        (kInodesPerExtent * kInodeSize + blockSize_ - 1) / blockSize_);
    if (extentBlock == 0 || extentBlocks < neededBlocks) return out;

    // OS/2 did not always align 16 KiB inode extents to a 4 KiB page.
    // The inode number is relative to the start of the extent, not to a
    // rounded metadata-page boundary.  Computing the byte position directly
    // preserves every page transition, including extents that begin halfway
    // through a page.
    const std::int64_t extentOffset = blockOffset(extentBlock);
    if (extentOffset < 0) return out;
    const std::uint64_t inodeByte =
        static_cast<std::uint64_t>(inExtent) * kInodeSize;
    if (inodeByte > static_cast<std::uint64_t>(extentBlocks) * blockSize_ -
                        kInodeSize)
        return out;
    const std::int64_t pos = extentOffset +
        static_cast<std::int64_t>(inodeByte);
    out.raw = disc_->readRange(pos, kInodeSize);
    if (out.raw.size() != kInodeSize) return Inode();
    out.number = le32(out.raw.data() + 8);
    out.size = le64(out.raw.data() + 24);
    out.blocks = le64(out.raw.data() + 32);
    out.mode = le32(out.raw.data() + 52);
    const std::uint32_t links = le32(out.raw.data() + 40);
    if (out.number != number || links == 0) return Inode();
    out.valid = true;
    return out;
}

std::vector<JfsReader::Extent> JfsReader::inodeExtents(const Inode& inode) const {
    std::vector<Extent> out;
    if (!inode.valid || inode.raw.size() < kInodeSize) return out;
    const std::vector<std::uint8_t> root(inode.raw.begin() + 224, inode.raw.end());
    std::set<std::uint64_t> visited;
    parseXtreeNode(root, true, &out, &visited, 0);
    std::sort(out.begin(), out.end(), [](const Extent& a, const Extent& b) {
        return a.logicalBlock < b.logicalBlock;
    });
    return out;
}

void JfsReader::parseXtreeNode(const std::vector<std::uint8_t>& node, bool root,
                               std::vector<Extent>* out, std::set<std::uint64_t>* visited,
                               int depth) const {
    if (!out || !visited || node.size() < 32 || depth > 8) return;
    const std::uint8_t flag = node[16];
    const std::uint16_t nextIndex = le16(node.data() + 18);
    const std::uint16_t maxEntry = le16(node.data() + 20);
    const std::size_t slotCount = node.size() / 16;
    if ((flag & (kBtLeaf | kBtInternal)) == 0 || (root && !(flag & kBtRoot)) ||
        nextIndex < 2 || nextIndex > maxEntry || nextIndex > slotCount)
        return;

    for (std::uint16_t slot = 2; slot < nextIndex; ++slot) {
        const std::size_t off = static_cast<std::size_t>(slot) * 16;
        if (off + 16 > node.size()) break;
        const std::uint8_t* xad = node.data() + off;
        const std::uint64_t address = pxdAddress(xad + 8);
        const std::uint32_t length = pxdLength(xad + 8);
        if (address == 0 || length == 0) continue;
        if (flag & kBtLeaf) {
            Extent e;
            e.logicalBlock = xadOffset(xad);
            e.physicalBlock = address;
            e.blockCount = length;
            e.flags = xad[0];
            out->push_back(e);
        } else {
            if (!visited->insert(address).second) continue;
            const std::vector<std::uint8_t> child = disc_->readRange(blockOffset(address), kPageSize);
            if (child.size() == kPageSize)
                parseXtreeNode(child, false, out, visited, depth + 1);
        }
    }
}

ByteStorePtr JfsReader::inodeContent(const Inode& inode) const {
    if (!inode.valid || inode.size > static_cast<std::uint64_t>(INT64_MAX)) return nullptr;
    if (inodeIsSymlink(inode) && inode.size <= 256 && inode.blocks == 0 &&
        inode.raw.size() >= 256 + inode.size) {
        return std::make_shared<MemoryStore>(inode.raw.data() + 256,
                                             static_cast<std::size_t>(inode.size));
    }
    const std::vector<Extent> extents = inodeExtents(inode);
    // XAD_COMPRESSED is an extent mapping flag, not a hole marker.  Keep the
    // physical mapping available instead of turning a non-empty inode into an
    // empty payload.
    return std::make_shared<JfsExtentStore>(disc_, static_cast<std::int64_t>(inode.size),
                                            extents, blockSize_);
}

bool JfsReader::decodeLeafEntryWithCapacity(const std::vector<std::uint8_t>& node,
                                            int slot, int slotCount,
                                            int firstCapacity,
                                            DirRec* out) const {
    if (!out || slot <= 0 || slot >= slotCount ||
        (firstCapacity != 11 && firstCapacity != 13))
        return false;
    const std::size_t off = static_cast<std::size_t>(slot) * 32;
    if (off + 32 > node.size()) return false;
    const std::uint8_t* entry = node.data() + off;
    const std::uint32_t inode = le32(entry);
    int next = static_cast<std::int8_t>(entry[4]);
    const int nameLength = entry[5];
    if (inode == 0 || nameLength <= 0 || nameLength > 255) return false;

    std::vector<std::uint16_t> chars;
    chars.reserve(static_cast<std::size_t>(nameLength));
    const int firstCount = std::min(nameLength, firstCapacity);
    for (int i = 0; i < firstCount; ++i)
        chars.push_back(le16(entry + 6 + i * 2));

    // Used continuation slots are a linked list.  JFS readers use the head
    // entry's namlen as the authoritative length; the continuation cnt byte is
    // not part of the name-length calculation.
    std::set<int> visited;
    visited.insert(slot);
    while (static_cast<int>(chars.size()) < nameLength) {
        if (next < 0 || next >= slotCount || !visited.insert(next).second)
            return false;
        const std::size_t continuationOff = static_cast<std::size_t>(next) * 32;
        if (continuationOff + 32 > node.size()) return false;
        const std::uint8_t* continuation = node.data() + continuationOff;
        next = static_cast<std::int8_t>(continuation[0]);
        const int count = std::min<int>(nameLength - static_cast<int>(chars.size()), 15);
        for (int i = 0; i < count; ++i)
            chars.push_back(le16(continuation + 2 + i * 2));
    }

    out->inode = inode;
    out->name = utf16ToUtf8(chars);
    return !out->name.empty() && out->name != "." && out->name != "..";
}

bool JfsReader::decodeLeafEntry(const std::vector<std::uint8_t>& node, int slot,
                                int slotCount, DirRec* out) const {
    // JFS_DIR_INDEX changes the leaf-head layout on every platform: indexed
    // entries have 11 UTF-16 units followed by a persistent 32-bit index;
    // only legacy, non-indexed filesystems have 13 units.
    const int firstCapacity = directoryIndex_ ? 11 : 13;
    return decodeLeafEntryWithCapacity(node, slot, slotCount, firstCapacity, out);
}

bool JfsReader::decodeInternalChild(const std::vector<std::uint8_t>& node, int slot,
                                    int slotCount, std::uint64_t* childBlock,
                                    std::uint32_t* childBlocks) const {
    if (!childBlock || !childBlocks || slot <= 0 || slot >= slotCount) return false;
    const std::size_t off = static_cast<std::size_t>(slot) * 32;
    if (off + 32 > node.size()) return false;
    const std::uint64_t address = pxdAddress(node.data() + off);
    const std::uint32_t length = pxdLength(node.data() + off);
    if (address == 0 || length == 0) return false;
    *childBlock = address;
    *childBlocks = length;
    return true;
}

bool JfsReader::readDtreePage(std::uint64_t block, std::uint32_t hintedBlocks,
                              std::vector<std::uint8_t>* out) const {
    if (!out || blockSize_ == 0 || block == 0) return false;
    const std::int64_t offset = blockOffset(block);
    if (offset < 0) return false;

    // Read the header first. The PXD length is a useful hint, but damaged or
    // old OS/2 trees can retain a conservative length while maxslot records
    // the actual 512/1024/2048/4096-byte node size.
    std::vector<std::uint8_t> header = disc_->readRange(offset, 32);
    if (header.size() != 32) return false;
    const int maxslot = static_cast<int>(header[20]);
    std::size_t declared = 0;
    if (maxslot == 16 || maxslot == 32 || maxslot == 64 || maxslot == 128)
        declared = static_cast<std::size_t>(maxslot) * 32;

    const std::uint64_t hinted64 = static_cast<std::uint64_t>(hintedBlocks) * blockSize_;
    std::size_t hinted = hinted64 <= kPageSize ? static_cast<std::size_t>(hinted64) : 0;
    std::size_t bytes = declared != 0 ? declared : hinted;
    if (bytes < 512 || bytes > kPageSize) return false;
    *out = disc_->readRange(offset, static_cast<std::int64_t>(bytes));
    return out->size() == bytes;
}

bool JfsReader::parseDtreeLeaf(const std::vector<std::uint8_t>& node, bool root,
                               std::vector<DirRec>* out) const {
    if (!out || node.size() < 32) return false;
    const std::uint8_t flag = node[16];
    if (!(flag & kBtLeaf) || (root && !(flag & kBtRoot))) return false;

    const int physicalSlots = static_cast<int>(node.size() / 32);
    int slotCount = root ? 9 : static_cast<int>(node[20]);
    if (slotCount <= 0 || slotCount > physicalSlots) slotCount = physicalSlots;
    const int entryCount = static_cast<int>(static_cast<std::int8_t>(node[17]));
    if (entryCount < 0 || entryCount > slotCount) return false;

    std::size_t stblOffset = 24;
    if (!root) {
        const int stblIndex = static_cast<int>(static_cast<std::int8_t>(node[21]));
        if (stblIndex <= 0 || stblIndex >= slotCount) return false;
        stblOffset = static_cast<std::size_t>(stblIndex) * 32;
    }
    if (stblOffset + static_cast<std::size_t>(entryCount) > node.size()) return false;

    bool parsed = false;
    for (int i = 0; i < entryCount; ++i) {
        const int slot = static_cast<int>(node[stblOffset + static_cast<std::size_t>(i)]);
        if (slot <= 0 || slot >= slotCount) continue;
        DirRec rec;
        if (decodeLeafEntry(node, slot, slotCount, &rec)) {
            out->push_back(rec);
            parsed = true;
        }
    }
    return parsed || entryCount == 0;
}

bool JfsReader::descendToLeftmostLeaf(const std::vector<std::uint8_t>& root,
                                      std::vector<std::uint8_t>* leaf,
                                      std::size_t* leafBytes) const {
    if (!leaf || !leafBytes || root.size() < 32) return false;
    std::vector<std::uint8_t> node = root;
    bool isRoot = true;
    std::set<std::uint64_t> visited;

    for (int depth = 0; depth <= 8; ++depth) {
        const std::uint8_t flag = node[16];
        if (isRoot && !(flag & kBtRoot)) return false;
        if (flag & kBtLeaf) {
            *leaf = std::move(node);
            *leafBytes = leaf->size();
            return true;
        }
        if (!(flag & kBtInternal)) return false;

        const int physicalSlots = static_cast<int>(node.size() / 32);
        int slotCount = isRoot ? 9 : static_cast<int>(node[20]);
        if (slotCount <= 0 || slotCount > physicalSlots) slotCount = physicalSlots;
        const int entryCount = static_cast<int>(static_cast<std::int8_t>(node[17]));
        if (entryCount <= 0) return false;
        std::size_t stblOffset = 24;
        if (!isRoot) {
            const int stblIndex = static_cast<int>(static_cast<std::int8_t>(node[21]));
            if (stblIndex <= 0 || stblIndex >= slotCount) return false;
            stblOffset = static_cast<std::size_t>(stblIndex) * 32;
        }
        if (stblOffset >= node.size()) return false;
        const int slot = static_cast<int>(node[stblOffset]);
        std::uint64_t childBlock = 0;
        std::uint32_t childBlocks = 0;
        if (!decodeInternalChild(node, slot, slotCount, &childBlock, &childBlocks) ||
            !visited.insert(childBlock).second)
            return false;

        if (!readDtreePage(childBlock, childBlocks, &node)) return false;
        isRoot = false;
    }
    return false;
}

bool JfsReader::collectDtreeNode(const std::vector<std::uint8_t>& node, bool root,
                                  std::vector<DirRec>* out,
                                  std::set<std::uint64_t>* visited,
                                  int depth) const {
    if (!out || !visited || node.size() < 32 || depth > 16) return false;
    const std::uint8_t flag = node[16];
    if (root && !(flag & kBtRoot)) return false;
    if (flag & kBtLeaf) return parseDtreeLeaf(node, root, out);
    if (!(flag & kBtInternal)) return false;

    const int physicalSlots = static_cast<int>(node.size() / 32);
    int slotCount = root ? 9 : static_cast<int>(node[20]);
    if (slotCount <= 0 || slotCount > physicalSlots) slotCount = physicalSlots;
    const int entryCount = static_cast<int>(static_cast<std::int8_t>(node[17]));
    if (entryCount <= 0 || entryCount > slotCount) return false;

    std::size_t stblOffset = 24;
    if (!root) {
        const int stblIndex = static_cast<int>(static_cast<std::int8_t>(node[21]));
        if (stblIndex <= 0 || stblIndex >= slotCount) return false;
        stblOffset = static_cast<std::size_t>(stblIndex) * 32;
    }
    if (stblOffset + static_cast<std::size_t>(entryCount) > node.size()) return false;

    bool any = false;
    for (int i = 0; i < entryCount; ++i) {
        const int slot = static_cast<int>(node[stblOffset + static_cast<std::size_t>(i)]);
        std::uint64_t childBlock = 0;
        std::uint32_t childBlocks = 0;
        if (!decodeInternalChild(node, slot, slotCount, &childBlock, &childBlocks) ||
            !visited->insert(childBlock).second)
            continue;
        std::vector<std::uint8_t> child;
        if (!readDtreePage(childBlock, childBlocks, &child)) continue;
        if (collectDtreeNode(child, false, out, visited, depth + 1)) any = true;
    }
    return any;
}

std::vector<JfsReader::DirRec> JfsReader::readDirectory(const Inode& inode) const {
    std::vector<DirRec> out;
    if (!inode.valid || !inodeIsDirectory(inode) || inode.raw.size() < kInodeSize)
        return out;

    const std::vector<std::uint8_t> root(inode.raw.begin() + 224, inode.raw.end());
    if (root.size() < 32) return out;

    // Walk every router child, then also follow the legacy leaf sibling chain.
    // OS/2 volumes exist in both forms: some have complete router trees, while
    // others rely on siblings for entries not referenced by the root router.
    std::set<std::uint64_t> visited;
    collectDtreeNode(root, true, &out, &visited, 0);

    if (root[16] & kBtLeaf) {
        parseDtreeLeaf(root, true, &out);
    } else {
        std::vector<std::uint8_t> leaf;
        std::size_t leafBytes = 0;
        if (descendToLeftmostLeaf(root, &leaf, &leafBytes)) {
            std::set<std::uint64_t> visitedLeaves;
            for (int pages = 0; pages < 100000; ++pages) {
                parseDtreeLeaf(leaf, false, &out);
                const std::uint64_t next = le64(leaf.data());
                if (next == 0 || !visitedLeaves.insert(next).second) break;
                std::vector<std::uint8_t> nextLeaf;
                if (!readDtreePage(next, 0, &nextLeaf)) break;
                leaf.swap(nextLeaf);
            }
        }
    }

    std::vector<DirRec> unique;
    std::set<std::pair<std::uint32_t, std::string> > seen;
    for (const DirRec& rec : out) {
        const std::pair<std::uint32_t, std::string> key(rec.inode, rec.name);
        if (seen.insert(key).second) unique.push_back(rec);
    }
    return unique;
}

bool JfsReader::inodeIsDirectory(const Inode& inode) const {
    return inode.isPosixDirectory() ||
           (caseInsensitive_ && (inode.mode & kOs2Directory) != 0);
}

bool JfsReader::inodeIsSymlink(const Inode& inode) const {
    return inode.isPosixSymlink();
}

bool JfsReader::inodeIsRegular(const Inode& inode) const {
    return inode.isPosixRegular();
}

bool JfsReader::resolvePath(const std::string& path, Inode* out) const {
    if (inodeMapExtents_.empty()) return false;
    Inode current = readFilesetInode(kRootInode);
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& part : parts) {
        if (!current.valid || !inodeIsDirectory(current)) return false;
        bool found = false;
        const std::vector<DirRec> entries = readDirectory(current);
        for (const DirRec& rec : entries) {
            const bool match = caseInsensitive_ ? asciiEqualIgnoreCase(rec.name, part) : rec.name == part;
            if (match) {
                current = readFilesetInode(rec.inode);
                found = current.valid;
                break;
            }
        }
        if (!found) return false;
    }
    if (out) *out = current;
    return current.valid;
}

std::vector<DiscEntry> JfsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    Inode directory;
    if (!valid_ || !resolvePath(dirPath, &directory) || !inodeIsDirectory(directory))
        return out;
    const std::vector<DirRec> entries = readDirectory(directory);
    for (const DirRec& rec : entries) {
        const Inode child = readFilesetInode(rec.inode);
        const bool isDir = inodeIsDirectory(child);
        const bool isFile = inodeIsRegular(child) || inodeIsSymlink(child);
        if (!child.valid || (!isDir && !isFile)) continue;
        if (!isDir && child.size > static_cast<std::uint64_t>(INT64_MAX)) continue;
        out.push_back(DiscEntry(rec.name, isDir,
                                isDir ? 0 : static_cast<std::int64_t>(child.size)));
    }
    std::sort(out.begin(), out.end(), [](const DiscEntry& a, const DiscEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
    return out;
}

ByteStorePtr JfsReader::openFile(const std::string& path) const {
    Inode inode;
    if (!valid_ || !resolvePath(path, &inode) ||
        (!inodeIsRegular(inode) && !inodeIsSymlink(inode)))
        return nullptr;
    return inodeContent(inode);
}

}  // namespace fs
}  // namespace peare
