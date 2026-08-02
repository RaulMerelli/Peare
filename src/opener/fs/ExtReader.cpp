#include "ExtReader.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace peare {
namespace fs {
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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

const std::uint32_t kExtentsFlag = 0x80000;
const std::uint16_t kExtentMagic = 0xF30A;

}  // namespace

ExtReader::ExtReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

std::uint32_t ExtReader::readU32(std::int64_t bytePos) const {
    std::uint8_t b[4];
    if (disc_->read(bytePos, b, 4) != 4) return 0;
    return le32(b);
}

void ExtReader::parse() {
    std::vector<std::uint8_t> sb = disc_->readRange(1024, 1024);
    if (sb.size() < 1024) { error_ = "Truncated ext superblock"; return; }
    if (le16(sb.data() + 56) != 0xEF53) { error_ = "Not an ext2/3/4 volume"; return; }

    const std::uint32_t logBlockSize = le32(sb.data() + 24);
    blockSize_ = 1024u << logBlockSize;
    blocksCount_ = le32(sb.data() + 4);
    firstDataBlock_ = le32(sb.data() + 20);
    blocksPerGroup_ = le32(sb.data() + 32);
    inodesPerGroup_ = le32(sb.data() + 40);
    inodeSize_ = le16(sb.data() + 88);
    if (inodeSize_ < 128) inodeSize_ = 128;
    const std::uint32_t incompat = le32(sb.data() + 96);
    descriptorSize_ = le16(sb.data() + 254);
    has64Bit_ = (incompat & 0x80) != 0 && descriptorSize_ >= 64;
    if (!has64Bit_) descriptorSize_ = 32;

    if (blockSize_ == 0 || inodesPerGroup_ == 0 || blocksPerGroup_ == 0) {
        error_ = "Invalid ext geometry";
        return;
    }
    bgDescStart_ = static_cast<std::int64_t>(firstDataBlock_ + 1) * blockSize_;

    const char* v = reinterpret_cast<const char*>(sb.data() + 120);
    std::size_t vlen = 0;
    while (vlen < 16 && v[vlen] != '\0') ++vlen;
    if (vlen) friendly_ = std::string("ext (") + std::string(v, vlen) + ")";
    valid_ = true;
}

std::uint64_t ExtReader::inodeTableBlock(std::uint32_t group) const {
    const std::int64_t pos = bgDescStart_ + static_cast<std::int64_t>(group) * descriptorSize_;
    std::uint64_t block = readU32(pos + 8);
    if (has64Bit_) block |= static_cast<std::uint64_t>(readU32(pos + 0x28)) << 32;
    return block;
}

ExtReader::Inode ExtReader::readInode(std::uint32_t inodeNum) const {
    Inode in;
    if (inodeNum == 0) return in;
    const std::uint32_t index = inodeNum - 1;
    const std::uint32_t group = index / inodesPerGroup_;
    const std::uint32_t off = index % inodesPerGroup_;
    const std::int64_t pos =
        static_cast<std::int64_t>(inodeTableBlock(group)) * blockSize_ +
        static_cast<std::int64_t>(off) * inodeSize_;
    std::vector<std::uint8_t> data = disc_->readRange(pos, inodeSize_);
    if (static_cast<int>(data.size()) < 128) return in;

    in.mode = le16(data.data());
    std::uint64_t size = le32(data.data() + 4);
    const std::uint32_t blocksCount = le32(data.data() + 28);
    const std::uint32_t flags = le32(data.data() + 32);
    const std::uint16_t ft = static_cast<std::uint16_t>((in.mode >> 12) & 0xF);
    // Fast symlink: target path (< 60 bytes) is stored inline in i_block.
    in.fastSymlink = (ft == 0xA) && blocksCount == 0;
    in.usesExtents = !in.fastSymlink && (flags & kExtentsFlag) != 0;
    // 60-byte i_block area holds either the extent tree root or the block map.
    in.blockMap.assign(data.begin() + 40, data.begin() + 100);
    // For regular files the high 32 bits of the size live in i_size_high (@108).
    if (ft == 0x8) size |= static_cast<std::uint64_t>(le32(data.data() + 108)) << 32;
    in.fileSize = size;
    in.valid = true;
    return in;
}

bool ExtReader::findExtent(const std::vector<std::uint8_t>& node, std::uint64_t logical,
                           std::uint64_t* firstLogical, std::uint64_t* physBlock,
                           std::uint64_t* numBlocks) const {
    if (node.size() < 12 || le16(node.data()) != kExtentMagic) return false;
    const std::uint16_t entries = le16(node.data() + 2);
    const std::uint16_t depth = le16(node.data() + 6);

    if (depth == 0) {
        if (entries == 0) return false;
        int sel = -1;
        for (int i = 0; i < entries; ++i) {
            const std::uint32_t fl = le32(node.data() + 12 + i * 12);
            if (i == 0 && fl >= logical) { sel = 0; break; }
            if (fl > logical) { sel = i - 1; break; }
            sel = i;
        }
        if (sel < 0) sel = 0;
        const std::uint8_t* e = node.data() + 12 + sel * 12;
        *firstLogical = le32(e);
        *numBlocks = le16(e + 4);
        *physBlock = static_cast<std::uint64_t>(le32(e + 8)) |
                     (static_cast<std::uint64_t>(le16(e + 6)) << 32);
        return true;
    }

    // Internal node: pick the index entry then load and recurse into the leaf.
    if (entries == 0) return false;
    int sel = -1;
    for (int i = 0; i < entries; ++i) {
        const std::uint32_t fl = le32(node.data() + 12 + i * 12);
        if (i == 0 && fl >= logical) { sel = 0; break; }
        if (fl > logical) { sel = i - 1; break; }
        sel = i;
    }
    if (sel < 0) sel = 0;
    const std::uint8_t* ix = node.data() + 12 + sel * 12;
    const std::uint64_t leaf = static_cast<std::uint64_t>(le32(ix + 4)) |
                               (static_cast<std::uint64_t>(le16(ix + 8)) << 32);
    std::vector<std::uint8_t> child =
        disc_->readRange(static_cast<std::int64_t>(leaf) * blockSize_, blockSize_);
    return findExtent(child, logical, firstLogical, physBlock, numBlocks);
}

std::uint32_t ExtReader::indirectLookup(const Inode& in, std::uint64_t logical) const {
    const std::uint32_t perInd = blockSize_ / 4;
    if (logical < 12)
        return le32(in.blockMap.data() + logical * 4);

    std::uint64_t l = logical - 12;
    const std::uint32_t indirect = le32(in.blockMap.data() + 48);
    const std::uint32_t dindirect = le32(in.blockMap.data() + 52);
    const std::uint32_t tindirect = le32(in.blockMap.data() + 56);

    if (l < perInd) {
        if (indirect == 0) return 0;
        return readU32(static_cast<std::int64_t>(indirect) * blockSize_ + l * 4);
    }
    l -= perInd;
    if (l < static_cast<std::uint64_t>(perInd) * perInd) {
        if (dindirect == 0) return 0;
        const std::uint32_t mid =
            readU32(static_cast<std::int64_t>(dindirect) * blockSize_ + (l / perInd) * 4);
        if (mid == 0) return 0;
        return readU32(static_cast<std::int64_t>(mid) * blockSize_ + (l % perInd) * 4);
    }
    l -= static_cast<std::uint64_t>(perInd) * perInd;
    if (tindirect == 0) return 0;
    const std::uint32_t a =
        readU32(static_cast<std::int64_t>(tindirect) * blockSize_ + (l / (static_cast<std::uint64_t>(perInd) * perInd)) * 4);
    if (a == 0) return 0;
    const std::uint32_t b =
        readU32(static_cast<std::int64_t>(a) * blockSize_ + ((l / perInd) % perInd) * 4);
    if (b == 0) return 0;
    return readU32(static_cast<std::int64_t>(b) * blockSize_ + (l % perInd) * 4);
}

void ExtReader::resolveRun(const Inode& in, std::uint64_t logical, std::uint64_t totalBlocks,
                           std::uint64_t* physStart, std::uint64_t* runLen, bool* hole) const {
    if (in.usesExtents) {
        std::uint64_t fl = 0, phys = 0, num = 0;
        if (!findExtent(in.blockMap, logical, &fl, &phys, &num)) {
            *hole = true; *physStart = 0; *runLen = totalBlocks - logical;
            return;
        }
        if (fl > logical) {
            *hole = true; *physStart = 0;
            *runLen = std::min<std::uint64_t>(fl - logical, totalBlocks - logical);
            return;
        }
        const std::uint64_t inExtent = logical - fl;
        if (inExtent >= num) {  // past this extent's coverage: treat as hole
            *hole = true; *physStart = 0; *runLen = 1;
            return;
        }
        *hole = false;
        *physStart = phys + inExtent;
        *runLen = std::min<std::uint64_t>(num - inExtent, totalBlocks - logical);
        return;
    }

    // Indirect block map: resolve a single block, callers coalesce runs.
    const std::uint32_t p = indirectLookup(in, logical);
    *hole = (p == 0);
    *physStart = p;
    *runLen = 1;
}

ByteStorePtr ExtReader::buildContent(const Inode& in) const {
    if (in.fastSymlink) {
        // The symlink target is stored inline in i_block; content is those bytes.
        auto mem = std::make_shared<MemoryStore>(in.blockMap);
        const std::int64_t n =
            std::min<std::int64_t>(mem->capacity(), static_cast<std::int64_t>(in.fileSize));
        return std::make_shared<SubStore>(mem, 0, n);
    }
    const std::uint64_t total = (in.fileSize + blockSize_ - 1) / blockSize_;
    std::vector<ByteStorePtr> parts;

    std::uint64_t L = 0;
    while (L < total) {
        std::uint64_t physStart = 0, runLen = 0;
        bool hole = false;
        resolveRun(in, L, total, &physStart, &runLen, &hole);
        if (runLen == 0) runLen = 1;

        if (!in.usesExtents && !hole) {
            // Coalesce consecutive physical blocks from the indirect map.
            std::uint64_t next = L + 1;
            std::uint64_t expect = physStart + 1;
            while (next < total) {
                std::uint64_t ps = 0, rl = 0; bool h = false;
                resolveRun(in, next, total, &ps, &rl, &h);
                if (h || ps != expect) break;
                ++next; ++expect; ++runLen;
            }
        }

        const std::int64_t bytes = static_cast<std::int64_t>(runLen) * blockSize_;
        if (hole)
            parts.push_back(std::make_shared<ZeroStore>(bytes));
        else
            parts.push_back(std::make_shared<SubStore>(
                disc_, static_cast<std::int64_t>(physStart) * blockSize_, bytes));
        L += runLen;
    }

    ByteStorePtr concat = std::make_shared<ConcatStore>(std::move(parts));
    const std::int64_t n =
        std::min<std::int64_t>(concat->capacity(), static_cast<std::int64_t>(in.fileSize));
    return std::make_shared<SubStore>(concat, 0, n);
}

std::vector<ExtReader::DirRec> ExtReader::readDirectory(const Inode& dir) const {
    std::vector<DirRec> out;
    std::vector<std::uint8_t> content = buildContent(dir)->readAll();
    std::size_t pos = 0;
    while (pos + 8 <= content.size()) {
        const std::uint8_t* e = content.data() + pos;
        const std::uint32_t inode = le32(e);
        const std::uint16_t recLen = le16(e + 4);
        const std::uint8_t nameLen = e[6];
        const std::uint8_t fileType = e[7];
        if (recLen < 8) break;
        if (inode != 0 && pos + 8 + nameLen <= content.size()) {
            std::string name(reinterpret_cast<const char*>(e + 8), nameLen);
            if (name != "." && name != "..") {
                DirRec r;
                r.name = name;
                r.inode = inode;
                r.fileType = fileType;
                out.push_back(r);
            }
        }
        pos += recLen;
    }
    return out;
}

bool ExtReader::resolvePath(const std::string& path, Inode* out) const {
    Inode cur = readInode(2);  // root
    if (!cur.valid) return false;
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& comp : parts) {
        const std::vector<DirRec> recs = readDirectory(cur);
        const std::string want = toLower(comp);
        std::uint32_t next = 0;
        for (const DirRec& r : recs) {
            if (toLower(r.name) == want) { next = r.inode; break; }
        }
        if (next == 0) return false;
        cur = readInode(next);
        if (!cur.valid) return false;
    }
    *out = cur;
    return true;
}

std::vector<DiscEntry> ExtReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    Inode dir;
    if (!resolvePath(dirPath, &dir)) return out;
    if (((dir.mode >> 12) & 0xF) != 0x4) return out;  // not a directory
    for (const DirRec& r : readDirectory(dir)) {
        DiscEntry e;
        e.name = r.name;
        bool isDir = (r.fileType == 2);
        std::uint64_t size = 0;
        if (r.fileType == 0 || !isDir) {
            const Inode ci = readInode(r.inode);
            if (r.fileType == 0) isDir = (((ci.mode >> 12) & 0xF) == 0x4);
            size = ci.fileSize;
        }
        e.isDirectory = isDir;
        e.length = isDir ? 0 : static_cast<std::int64_t>(size);
        out.push_back(e);
    }
    return out;
}

ByteStorePtr ExtReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    Inode in;
    if (!resolvePath(path, &in)) return nullptr;
    if (((in.mode >> 12) & 0xF) == 0x4) return nullptr;  // directory
    return buildContent(in);
}

}  // namespace fs
}  // namespace peare
