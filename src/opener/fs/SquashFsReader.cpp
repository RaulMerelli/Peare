#include "SquashFsReader.h"

#include "../modules/DeflateDecoder.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kMagic = 0x73717368U;
const std::uint32_t kInvalidFragment = 0xffffffffU;
const std::size_t kMetadataSize = 8192;
const std::uint16_t kMetaUncompressedBit = 0x8000;
const std::uint32_t kDataUncompressedBit = 0x01000000U;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t sle32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(le32(p));
}

std::uint64_t le64u(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) |
           (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::int64_t le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(le64u(p));
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

std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& data,
                                      std::size_t limit) {
    std::vector<std::uint8_t> out;
    if (!compression::inflateZlib(data, limit, &out)) out.clear();
    return out;
}

class SquashFileStore final : public IByteStore {
public:
    SquashFileStore(ByteStorePtr disc, std::int64_t length, std::uint32_t blockSize,
                    std::uint32_t startBlock, std::vector<std::uint32_t> blockSizes,
                    std::vector<std::uint8_t> fragment, std::uint32_t fragmentOffset)
        : disc_(std::move(disc)), length_(length), blockSize_(blockSize), startBlock_(startBlock),
          blockSizes_(std::move(blockSizes)), fragment_(std::move(fragment)),
          fragmentOffset_(fragmentOffset) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!disc_ || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t avail = length_ - pos;
        int want = count < avail ? count : static_cast<int>(avail);
        int done = 0;
        while (want > 0) {
            const std::uint64_t logicalBlock = static_cast<std::uint64_t>(pos + done) / blockSize_;
            const std::uint32_t inBlock = static_cast<std::uint32_t>((pos + done) % blockSize_);
            const std::int64_t fragmentStart = static_cast<std::int64_t>(blockSizes_.size()) * blockSize_;
            if (pos + done >= fragmentStart) {
                const std::size_t fragPos = fragmentOffset_ + static_cast<std::size_t>((pos + done) - fragmentStart);
                const int n = std::min<int>(want, static_cast<int>(fragment_.size() > fragPos ? fragment_.size() - fragPos : 0));
                if (n <= 0) break;
                std::copy(fragment_.begin() + static_cast<std::ptrdiff_t>(fragPos),
                          fragment_.begin() + static_cast<std::ptrdiff_t>(fragPos + n), dst + done);
                done += n;
                want -= n;
                continue;
            }

            if (logicalBlock >= blockSizes_.size()) break;
            std::int64_t diskPos = startBlock_;
            for (std::size_t i = 0; i < logicalBlock; ++i)
                diskPos += blockSizes_[i] & 0x00FFFFFFU;
            const std::uint32_t diskLen = blockSizes_[static_cast<std::size_t>(logicalBlock)];
            const std::size_t packedLen = diskLen & 0x00FFFFFFU;
            std::vector<std::uint8_t> block = disc_->readRange(diskPos, packedLen);
            if ((diskLen & kDataUncompressedBit) == 0)
                block = inflateZlib(block, blockSize_);
            if (block.size() > blockSize_) block.resize(blockSize_);
            const int n = std::min<int>(want, static_cast<int>(block.size() > inBlock ? block.size() - inBlock : 0));
            if (n <= 0) break;
            std::copy(block.begin() + inBlock, block.begin() + inBlock + n, dst + done);
            done += n;
            want -= n;
        }
        return done;
    }

private:
    ByteStorePtr disc_;
    std::int64_t length_;
    std::uint32_t blockSize_;
    std::uint32_t startBlock_;
    std::vector<std::uint32_t> blockSizes_;
    std::vector<std::uint8_t> fragment_;
    std::uint32_t fragmentOffset_;
};

}  // namespace

SquashFsReader::SquashFsReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

SquashFsReader::MetadataRef SquashFsReader::metadataRef(std::int64_t value) const {
    MetadataRef r;
    r.block = (value >> 16) & 0xFFFFFFFFFFFFLL;
    r.offset = static_cast<std::uint16_t>(value & 0xFFFF);
    return r;
}

void SquashFsReader::parse() {
    std::vector<std::uint8_t> sb = disc_ ? disc_->readRange(0, 96) : std::vector<std::uint8_t>();
    if (sb.size() < 96) { error_ = "Truncated SquashFS superblock"; return; }
    if (le32(sb.data()) != kMagic) { error_ = "Not a SquashFS image"; return; }

    blockSize_ = le32(sb.data() + 12);
    fragmentsCount_ = le32(sb.data() + 16);
    compression_ = le16(sb.data() + 20);
    flags_ = le16(sb.data() + 24);
    major_ = le16(sb.data() + 28);
    minor_ = le16(sb.data() + 30);
    root_ = metadataRef(le64(sb.data() + 32));
    xattrsTableStart_ = le64(sb.data() + 56);
    inodeTableStart_ = le64(sb.data() + 64);
    directoryTableStart_ = le64(sb.data() + 72);
    fragmentTableStart_ = le64(sb.data() + 80);

    if (major_ != 4) { error_ = "Unsupported SquashFS version"; return; }
    if (compression_ != 1) { error_ = "Unsupported SquashFS compression"; return; }
    if (xattrsTableStart_ != -1) { error_ = "Unsupported SquashFS extended attributes"; return; }
    if (blockSize_ == 0 || blockSize_ > 1024 * 1024) { error_ = "Invalid SquashFS block size"; return; }

    const Inode root = readInode(root_);
    if (!root.valid || !root.isDirectory()) { error_ = "Invalid SquashFS root inode"; return; }
    valid_ = true;
}

SquashFsReader::MetaBlock SquashFsReader::readMetaBlock(std::int64_t absolutePos) const {
    const auto cached = metaCache_.find(absolutePos);
    if (cached != metaCache_.end()) return cached->second;
    MetaBlock block;
    std::uint8_t head[2] = {0, 0};
    disc_->readExactly(absolutePos, head, 2);
    std::uint16_t len = le16(head);
    const bool compressed = (len & kMetaUncompressedBit) == 0;
    len &= 0x7FFF;
    if (len == 0) len = kMetaUncompressedBit;
    block.next = absolutePos + 2 + len;
    std::vector<std::uint8_t> payload = disc_->readRange(absolutePos + 2, len);
    block.data = compressed ? inflateZlib(payload, kMetadataSize) : payload;
    if (block.data.size() > kMetadataSize) block.data.resize(kMetadataSize);
    metaCache_[absolutePos] = block;
    return block;
}

std::vector<std::uint8_t> SquashFsReader::readMetaBytes(std::int64_t tableStart, MetadataRef ref,
                                                        std::size_t count, MetadataRef* end) const {
    std::vector<std::uint8_t> out;
    out.reserve(count);
    std::int64_t blockStart = ref.block;
    std::size_t off = ref.offset;
    while (out.size() < count) {
        const MetaBlock block = readMetaBlock(tableStart + blockStart);
        if (off >= block.data.size()) break;
        const std::size_t n = std::min<std::size_t>(count - out.size(), block.data.size() - off);
        out.insert(out.end(), block.data.begin() + static_cast<std::ptrdiff_t>(off),
                   block.data.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (off >= block.data.size() && out.size() < count) {
            blockStart = block.next - tableStart;
            off = 0;
        }
    }
    if (end) {
        end->block = blockStart;
        end->offset = static_cast<std::uint16_t>(off);
    }
    return out;
}

SquashFsReader::Inode SquashFsReader::readInode(MetadataRef ref) const {
    Inode inode;
    inode.ref = ref;
    std::vector<std::uint8_t> head = readMetaBytes(inodeTableStart_, ref, 16);
    if (head.size() < 16) return inode;
    inode.type = le16(head.data());
    std::size_t size = 0;
    if (inode.type == 1 || inode.type == 2 || inode.type == 3) size = 32;
    else if (inode.type == 8) size = 40;
    else return inode;

    MetadataRef end;
    std::vector<std::uint8_t> data = readMetaBytes(inodeTableStart_, ref, size, &end);
    if (data.size() < size) return inode;
    if (inode.type == 1) {
        inode.startBlock = le32(data.data() + 16);
        inode.size = le16(data.data() + 24);
        inode.dirOffset = le16(data.data() + 26);
    } else if (inode.type == 8) {
        inode.size = le32(data.data() + 20);
        inode.startBlock = le32(data.data() + 24);
        inode.dirOffset = le16(data.data() + 34);
    } else if (inode.type == 2) {
        inode.startBlock = le32(data.data() + 16);
        inode.fragmentKey = le32(data.data() + 20);
        inode.fragmentOffset = le32(data.data() + 24);
        inode.size = le32(data.data() + 28);
        const std::size_t blocks = inode.size / blockSize_ +
            ((inode.size % blockSize_) != 0 && inode.fragmentKey == kInvalidFragment ? 1 : 0);
        if (blocks) {
            std::vector<std::uint8_t> blockBytes = readMetaBytes(inodeTableStart_, end, blocks * 4);
            for (std::size_t i = 0; i < blocks && i * 4 + 4 <= blockBytes.size(); ++i)
                inode.blockSizes.push_back(le32(blockBytes.data() + i * 4));
        }
    } else if (inode.type == 3) {
        const std::uint32_t targetSize = le32(data.data() + 24);
        inode.size = targetSize;
    }
    inode.valid = true;
    return inode;
}

std::vector<SquashFsReader::DirRec> SquashFsReader::readDirectory(const Inode& dir) const {
    std::vector<DirRec> out;
    if (!dir.valid || !dir.isDirectory() || dir.size == 0) return out;
    MetadataRef pos;
    pos.block = dir.startBlock;
    pos.offset = dir.dirOffset;
    std::size_t consumed = 0;
    while (consumed + 12 < dir.size) {
        MetadataRef afterHeader;
        std::vector<std::uint8_t> h = readMetaBytes(directoryTableStart_, pos, 12, &afterHeader);
        if (h.size() < 12) break;
        const std::int32_t count = sle32(h.data());
        const std::int32_t startBlock = sle32(h.data() + 4);
        consumed += 12;
        pos = afterHeader;
        if (count < 0 || count > 4096) break;
        for (int i = 0; i <= count && consumed < dir.size; ++i) {
            MetadataRef afterRec;
            std::vector<std::uint8_t> r = readMetaBytes(directoryTableStart_, pos, 8, &afterRec);
            if (r.size() < 8) return out;
            const std::uint16_t off = le16(r.data());
            const std::int16_t inodeDelta = static_cast<std::int16_t>(le16(r.data() + 2));
            const std::uint16_t type = le16(r.data() + 4);
            const std::uint16_t nameLen = le16(r.data() + 6) + 1;
            consumed += 8;
            pos = afterRec;
            MetadataRef afterName;
            std::vector<std::uint8_t> nameBytes = readMetaBytes(directoryTableStart_, pos, nameLen, &afterName);
            if (nameBytes.size() < nameLen) return out;
            consumed += nameLen;
            pos = afterName;
            std::string name(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
            if (name == "." || name == "..") continue;
            DirRec rec;
            rec.name = name;
            (void)inodeDelta;  // Used by SquashFS for inode numbering; the metaref uses StartBlock+Offset.
            rec.inodeRef.block = startBlock;
            rec.inodeRef.offset = off;
            rec.isDirectory = type == 1 || type == 8;
            rec.isSymlink = type == 3 || type == 10;
            out.push_back(rec);
        }
    }
    return out;
}

std::vector<std::uint8_t> SquashFsReader::readDataBlock(std::int64_t pos, std::uint32_t diskLen,
                                                        std::size_t expected) const {
    const std::size_t readLen = diskLen & 0x00FFFFFFU;
    std::vector<std::uint8_t> payload = disc_->readRange(pos, readLen);
    if ((diskLen & kDataUncompressedBit) == 0)
        payload = inflateZlib(payload, expected);
    if (payload.size() > expected) payload.resize(expected);
    return payload;
}

bool SquashFsReader::readFragment(std::uint32_t key, std::vector<std::uint8_t>* out) const {
    if (key == kInvalidFragment || fragmentTableStart_ == -1 || !out) return false;
    const auto cached = fragmentCache_.find(key);
    if (cached != fragmentCache_.end()) { *out = cached->second; return true; }
    const std::uint32_t perMeta = static_cast<std::uint32_t>(kMetadataSize / 16);
    const std::uint32_t table = key / perMeta;
    const std::uint32_t offset = (key % perMeta) * 16;
    const std::uint32_t tableBlocks = (fragmentsCount_ * 16 + kMetadataSize - 1) / kMetadataSize;
    if (table >= tableBlocks) return false;
    std::vector<std::uint8_t> ptrBytes = disc_->readRange(fragmentTableStart_ + table * 8, 8);
    if (ptrBytes.size() < 8) return false;
    const std::int64_t metaStart = le64(ptrBytes.data());
    MetadataRef ref;
    ref.block = 0;
    ref.offset = static_cast<std::uint16_t>(offset);
    std::vector<std::uint8_t> rec = readMetaBytes(metaStart, ref, 16);
    if (rec.size() < 16) return false;
    const std::int64_t start = le64(rec.data());
    const std::uint32_t len = le32(rec.data() + 8);
    *out = readDataBlock(start, len, blockSize_);
    fragmentCache_[key] = *out;
    return true;
}

bool SquashFsReader::resolvePath(const std::string& path, Inode* out) const {
    Inode cur = readInode(root_);
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& part : parts) {
        bool found = false;
        for (const DirRec& rec : readDirectory(cur)) {
            if (rec.name == part) {
                cur = readInode(rec.inodeRef);
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (out) *out = cur;
    return cur.valid;
}

std::vector<DiscEntry> SquashFsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    Inode dir;
    if (!resolvePath(dirPath, &dir) || !dir.isDirectory()) return out;
    for (const DirRec& rec : readDirectory(dir)) {
        Inode child = readInode(rec.inodeRef);
        if (!child.valid) continue;
        DiscEntry e;
        e.name = rec.name;
        e.isDirectory = child.isDirectory();
        e.length = e.isDirectory ? 0 : static_cast<std::int64_t>(child.size);
        out.push_back(e);
    }
    return out;
}

ByteStorePtr SquashFsReader::openFile(const std::string& path) const {
    Inode inode;
    if (!resolvePath(path, &inode) || (!inode.isRegular() && !inode.isSymlink())) return nullptr;
    if (inode.isSymlink())
        return std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
    std::vector<std::uint8_t> fragment;
    readFragment(inode.fragmentKey, &fragment);
    return std::make_shared<SquashFileStore>(disc_, static_cast<std::int64_t>(inode.size), blockSize_,
                                             inode.startBlock, inode.blockSizes, fragment,
                                             inode.fragmentOffset);
}

}  // namespace fs
}  // namespace peare
