#include "BtrfsReader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>

#include <miniz.h>

namespace peare {
namespace fs {
namespace {

const std::uint64_t kMagic = 0x4d5f53665248425fULL;  // "_BHRfS_M" little-endian value
const std::uint64_t kSuperOffsets[] = {0x10000ULL, 0x4000000ULL, 0x4000000000ULL,
                                       0x4000000000000ULL};
const std::uint8_t kInodeItem = 0x01;
const std::uint8_t kDirItem = 0x54;
const std::uint8_t kDirIndex = 0x60;
const std::uint8_t kExtentData = 0x6c;
const std::uint8_t kRootItem = 0x84;
const std::uint8_t kChunkItem = 0xe4;
const std::uint64_t kRootTreeDir = 6;
const std::uint64_t kFsTree = 5;
const std::uint64_t kFirstChunkTree = 256;

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}
bool range(std::size_t size, std::size_t off, std::size_t len) {
    return off <= size && len <= size - off;
}
BtrfsReader::Key readKey(const std::vector<std::uint8_t>& b, std::size_t off) {
    BtrfsReader::Key k;
    if (!range(b.size(), off, 17)) return k;
    k.objectId = le64(b.data() + off);
    k.type = b[off + 8];
    k.offset = le64(b.data() + off + 9);
    return k;
}
int cmpKeyPrefix(const BtrfsReader::Key& a, const BtrfsReader::Key& b) {
    if (a.objectId != b.objectId) return a.objectId < b.objectId ? -1 : 1;
    if (a.type != b.type) return a.type < b.type ? -1 : 1;
    if (a.offset != b.offset) return a.offset < b.offset ? -1 : 1;
    return 0;
}
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == '/' || path[pos] == '\\')) ++pos;
        const std::size_t start = pos;
        while (pos < path.size() && path[pos] != '/' && path[pos] != '\\') ++pos;
        if (pos > start) out.push_back(path.substr(start, pos - start));
    }
    return out;
}
std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& data,
                                      std::size_t expected) {
    std::vector<std::uint8_t> out(expected);
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    if (inflateInit(&stream) != Z_OK) return std::vector<std::uint8_t>();
    const int status = inflate(&stream, Z_FINISH);
    const std::size_t produced = stream.total_out;
    inflateEnd(&stream);
    if (status != Z_STREAM_END) return std::vector<std::uint8_t>();
    out.resize(std::min(produced, expected));
    return out;
}

enum class LzoState { Literal, ShortMatch, LongMatch };

bool readLzoLength(const std::vector<std::uint8_t>& src, std::size_t* pos, std::size_t base,
                   std::size_t* len) {
    std::size_t extra = 0;
    while (*pos < src.size() && src[*pos] == 0) {
        extra += 255;
        ++(*pos);
    }
    if (*pos >= src.size()) return false;
    *len = extra + src[(*pos)++] + base;
    return true;
}

bool copyLzoLiteral(const std::vector<std::uint8_t>& src, std::size_t* inPos,
                    std::vector<std::uint8_t>* out, std::size_t len,
                    std::size_t expected) {
    if (*inPos > src.size() || len > src.size() - *inPos) return false;
    if (out->size() > expected || len > expected - out->size()) return false;
    out->insert(out->end(), src.begin() + static_cast<std::ptrdiff_t>(*inPos),
                src.begin() + static_cast<std::ptrdiff_t>(*inPos + len));
    *inPos += len;
    return true;
}

bool copyLzoMatch(std::vector<std::uint8_t>* out, std::size_t distance, std::size_t len,
                  std::size_t expected) {
    if (distance == 0 || distance > out->size()) return false;
    if (out->size() > expected || len > expected - out->size()) return false;
    const std::size_t start = out->size() - distance;
    for (std::size_t i = 0; i < len; ++i) out->push_back((*out)[start + i]);
    return true;
}

std::vector<std::uint8_t> decompressLzo1xBlock(const std::vector<std::uint8_t>& src,
                                               std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    if (src.empty()) return out;

    std::size_t inPos = 0;
    LzoState state = LzoState::Literal;
    const std::uint8_t first = src[inPos];
    if (first > 17) {
        ++inPos;
        const std::size_t litLen = std::size_t(first) - 17;
        if (!copyLzoLiteral(src, &inPos, &out, litLen, expected)) return std::vector<std::uint8_t>();
        state = first <= 20 ? LzoState::ShortMatch : LzoState::LongMatch;
    }

    while (inPos < src.size()) {
        const std::uint8_t op = src[inPos++];
        std::size_t matchLen = 0;
        std::size_t distance = 0;
        std::uint8_t literalBits = op;

        if (op <= 15) {
            if (state == LzoState::Literal) {
                std::size_t litLen = 0;
                if (op == 0) {
                    if (!readLzoLength(src, &inPos, 18, &litLen))
                        return std::vector<std::uint8_t>();
                } else {
                    litLen = std::size_t(op) + 3;
                }
                if (!copyLzoLiteral(src, &inPos, &out, litLen, expected))
                    return std::vector<std::uint8_t>();
                state = LzoState::LongMatch;
                continue;
            }
            matchLen = state == LzoState::ShortMatch ? 2 : 3;
            const std::size_t offset = state == LzoState::ShortMatch ? 1 : 2049;
            if (inPos >= src.size()) return std::vector<std::uint8_t>();
            distance = (std::size_t(src[inPos++]) << 2) + (op >> 2) + offset;
        } else if (op <= 31) {
            if ((op & 0x07) == 0) {
                if (!readLzoLength(src, &inPos, 9, &matchLen))
                    return std::vector<std::uint8_t>();
            } else {
                matchLen = std::size_t(op & 0x07) + 2;
            }
            if (inPos + 2 > src.size()) return std::vector<std::uint8_t>();
            const std::uint8_t sub = src[inPos];
            distance = (std::size_t((op & 0x08) >> 3) << 14) +
                       (std::size_t(src[inPos + 1]) << 6) + (sub >> 2) + 16384;
            inPos += 2;
            if (distance == 16384) {
                return out.size() == expected ? out : std::vector<std::uint8_t>();
            }
            literalBits = sub;
        } else if (op <= 63) {
            if ((op & 0x1f) == 0) {
                if (!readLzoLength(src, &inPos, 33, &matchLen))
                    return std::vector<std::uint8_t>();
            } else {
                matchLen = std::size_t(op & 0x1f) + 2;
            }
            if (inPos + 2 > src.size()) return std::vector<std::uint8_t>();
            const std::uint8_t sub = src[inPos];
            distance = (std::size_t(src[inPos + 1]) << 6) + (sub >> 2) + 1;
            inPos += 2;
            literalBits = sub;
        } else {
            matchLen = (op & 0x20) ? 4 : 3;
            if (op >= 128) matchLen = std::size_t((op & 0x60) >> 5) + 5;
            if (inPos >= src.size()) return std::vector<std::uint8_t>();
            distance = (std::size_t(src[inPos++]) << 3) + ((op & 0x1c) >> 2) + 1;
        }

        if (!copyLzoMatch(&out, distance, matchLen, expected)) return std::vector<std::uint8_t>();
        const std::size_t litLen = literalBits & 0x03;
        if (litLen == 0) {
            state = LzoState::Literal;
        } else {
            if (!copyLzoLiteral(src, &inPos, &out, litLen, expected))
                return std::vector<std::uint8_t>();
            state = LzoState::ShortMatch;
        }
    }

    return out.size() == expected ? out : std::vector<std::uint8_t>();
}

std::vector<std::uint8_t> inflateBtrfsLzo(const std::vector<std::uint8_t>& data,
                                          std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    if (data.size() < 4) return out;
    const std::uint32_t totalLength = le32(data.data());
    if (totalLength < 4 || totalLength > data.size()) return std::vector<std::uint8_t>();
    std::size_t pos = 4;
    std::size_t remaining = expected;
    while (pos < totalLength && remaining > 0) {
        if (pos + 4 > totalLength) return std::vector<std::uint8_t>();
        const std::uint32_t partLength = le32(data.data() + pos);
        pos += 4;
        if (partLength == 0 || pos + partLength > totalLength)
            return std::vector<std::uint8_t>();
        const std::size_t partExpected = std::min<std::size_t>(4096, remaining);
        std::vector<std::uint8_t> part(data.begin() + static_cast<std::ptrdiff_t>(pos),
                                       data.begin() + static_cast<std::ptrdiff_t>(pos + partLength));
        std::vector<std::uint8_t> decoded = decompressLzo1xBlock(part, partExpected);
        if (decoded.size() != partExpected) return std::vector<std::uint8_t>();
        out.insert(out.end(), decoded.begin(), decoded.end());
        remaining -= decoded.size();
        pos += partLength;
    }
    return out.size() == expected ? out : std::vector<std::uint8_t>();
}

class BtrfsFileStore final : public IByteStore {
public:
    BtrfsFileStore(ByteStorePtr disc, std::vector<BtrfsReader::Extent> extents,
                   std::vector<BtrfsReader::Chunk> chunks, std::uint64_t length)
        : disc_(std::move(disc)), extents_(std::move(extents)), chunks_(std::move(chunks)),
          length_(length) {}

    std::int64_t capacity() const override {
        return length_ > std::uint64_t(std::numeric_limits<std::int64_t>::max())
                   ? std::numeric_limits<std::int64_t>::max()
                   : std::int64_t(length_);
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!disc_ || pos < 0 || count <= 0 || std::uint64_t(pos) >= length_) return 0;
        std::uint64_t want = std::min<std::uint64_t>(count, length_ - std::uint64_t(pos));
        int done = 0;
        while (want > 0) {
            const std::uint64_t cur = std::uint64_t(pos) + done;
            const BtrfsReader::Extent* hit = nullptr;
            for (std::size_t i = 0; i < extents_.size(); ++i) {
                const BtrfsReader::Extent& e = extents_[i];
                const std::uint64_t len = e.type == 0 ? e.inlineData.size() : e.logicalSize;
                if (cur >= e.fileOffset && cur < e.fileOffset + len) {
                    hit = &e;
                    break;
                }
            }
            if (!hit) {
                dst[done++] = 0;
                --want;
                continue;
            }
            const std::uint64_t inExtent = cur - hit->fileOffset;
            const std::uint64_t logicalLen =
                hit->type == 0 ? hit->inlineData.size() : hit->logicalSize;
            const int toCopy = static_cast<int>(std::min<std::uint64_t>(want, logicalLen - inExtent));
            if (hit->type == 0) {
                std::vector<std::uint8_t> bytes = hit->inlineData;
                if (hit->compression == 1) bytes = inflateZlib(bytes, hit->decodedSize);
                if (hit->compression == 2) {
                    const std::uint64_t decoded = std::min<std::uint64_t>(
                        hit->decodedSize, length_ > hit->fileOffset ? length_ - hit->fileOffset : 0);
                    bytes = inflateBtrfsLzo(bytes, static_cast<std::size_t>(decoded));
                }
                if (inExtent >= bytes.size()) return done;
                const int n = static_cast<int>(std::min<std::uint64_t>(toCopy, bytes.size() - inExtent));
                std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(inExtent),
                          bytes.begin() + static_cast<std::ptrdiff_t>(inExtent + n), dst + done);
                done += n;
                want -= n;
            } else if (hit->type == 1) {
                if (hit->extentAddress == 0) {
                    std::fill(dst + done, dst + done + toCopy, std::uint8_t(0));
                    done += toCopy;
                    want -= toCopy;
                } else {
                    std::uint64_t physical = 0;
                    bool mapped = false;
                    for (std::size_t i = 0; i < chunks_.size(); ++i) {
                        const BtrfsReader::Chunk& c = chunks_[i];
                        if (hit->extentAddress >= c.logical &&
                            hit->extentAddress < c.logical + c.size) {
                            physical = c.physical + (hit->extentAddress - c.logical);
                            mapped = true;
                            break;
                        }
                    }
                    if (!mapped) return done;
                    std::vector<std::uint8_t> raw =
                        disc_->readRange(std::int64_t(physical + hit->extentOffset),
                                         std::int64_t(hit->extentSize));
                    if (hit->compression == 1) {
                        const std::uint64_t decoded = std::min<std::uint64_t>(
                            hit->logicalSize, length_ > hit->fileOffset ? length_ - hit->fileOffset : 0);
                        raw = inflateZlib(raw, static_cast<std::size_t>(decoded));
                    } else if (hit->compression == 2) {
                        const std::uint64_t decoded = std::min<std::uint64_t>(
                            hit->logicalSize, length_ > hit->fileOffset ? length_ - hit->fileOffset : 0);
                        raw = inflateBtrfsLzo(raw, static_cast<std::size_t>(decoded));
                    }
                    if (hit->compression == 1 || hit->compression == 2) {
                        if (inExtent >= raw.size()) return done;
                        const int n = static_cast<int>(
                            std::min<std::uint64_t>(toCopy, raw.size() - inExtent));
                        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(inExtent),
                                  raw.begin() + static_cast<std::ptrdiff_t>(inExtent + n),
                                  dst + done);
                        done += n;
                        want -= n;
                    } else if (hit->compression == 0) {
                        const int n = disc_->read(std::int64_t(physical + hit->extentOffset + inExtent),
                                                  dst + done, toCopy);
                        if (n <= 0) return done;
                        done += n;
                        want -= n;
                    } else {
                        return done;
                    }
                }
            } else {
                return done;
            }
        }
        return done;
    }

private:
    ByteStorePtr disc_;
    std::vector<BtrfsReader::Extent> extents_;
    std::vector<BtrfsReader::Chunk> chunks_;
    std::uint64_t length_;
};

}  // namespace

BtrfsReader::BtrfsReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

void BtrfsReader::parse() {
    if (!disc_ || disc_->capacity() < 0x11000) {
        error_ = "No Btrfs superblock detected";
        return;
    }
    bool found = false;
    std::uint64_t bestGeneration = 0;
    std::vector<std::uint8_t> best;
    for (std::size_t i = 0; i < sizeof(kSuperOffsets) / sizeof(kSuperOffsets[0]); ++i) {
        if (kSuperOffsets[i] + 0x1000 > std::uint64_t(disc_->capacity())) break;
        std::vector<std::uint8_t> sb = disc_->readRange(kSuperOffsets[i], 0x1000);
        if (sb.size() < 0x1000 || le64(sb.data() + 0x40) != kMagic) continue;
        const std::uint64_t gen = le64(sb.data() + 0x48);
        if (!found || gen > bestGeneration) {
            found = true;
            bestGeneration = gen;
            best = sb;
        }
    }
    if (!found) {
        error_ = "Invalid Btrfs superblock magic";
        return;
    }

    parseSuperblock(best);
    if (nodeSize_ == 0 || leafSize_ == 0 || chunkRootLogical_ == 0 || rootLogical_ == 0) {
        error_ = "Invalid Btrfs superblock";
        return;
    }

    try {
        Node chunkRoot = readTree(chunkRootLogical_, chunkRootLevel_);
        std::map<std::uint64_t, bool> seen;
        collectChunks(chunkRoot, &seen);
        rootTree_ = readTree(rootLogical_, rootLevel_);
        Entry root = rootEntry();
        (void)root;
    } catch (const std::exception& e) {
        error_ = e.what();
        return;
    } catch (...) {
        error_ = "Cannot initialize Btrfs";
        return;
    }
    valid_ = true;
    friendly_ = "Btrfs";
}

void BtrfsReader::parseSuperblock(const std::vector<std::uint8_t>& sb) {
    rootLogical_ = le64(sb.data() + 0x50);
    chunkRootLogical_ = le64(sb.data() + 0x58);
    rootDirObjectId_ = le64(sb.data() + 0x80);
    nodeSize_ = le32(sb.data() + 0x94);
    leafSize_ = le32(sb.data() + 0x98);
    rootLevel_ = sb[0xc6];
    chunkRootLevel_ = sb[0xc7];
    const std::uint32_t sysChunkBytes = le32(sb.data() + 0xa0);
    if (sysChunkBytes > 0 && range(sb.size(), 0x32b, sysChunkBytes))
        parseChunkArray(sb, 0x32b, sysChunkBytes);
}

void BtrfsReader::parseChunkArray(const std::vector<std::uint8_t>& b, std::size_t off,
                                  std::size_t len) {
    std::size_t pos = off;
    const std::size_t end = off + len;
    while (pos + 17 + 0x30 <= end) {
        Key key = readKey(b, pos);
        pos += 17;
        if (key.type != kChunkItem) break;
        const std::uint64_t chunkSize = le64(b.data() + pos);
        const std::uint16_t stripes = le16(b.data() + pos + 0x2c);
        if (stripes < 1 || pos + 0x30 + std::size_t(stripes) * 0x20 > end) break;
        Chunk c;
        c.logical = key.offset;
        c.size = chunkSize;
        c.physical = le64(b.data() + pos + 0x30 + 8);
        chunks_.push_back(c);
        pos += 0x30 + std::size_t(stripes) * 0x20;
    }
}

BtrfsReader::Node BtrfsReader::readTree(std::uint64_t logical, std::uint8_t expectedLevel) const {
    const std::uint64_t physical = mapToPhysical(logical);
    const std::uint32_t size = expectedLevel > 0 ? nodeSize_ : leafSize_;
    std::vector<std::uint8_t> b = disc_->readRange(std::int64_t(physical), size);
    if (b.size() < 0x65) throw std::runtime_error("Truncated Btrfs tree node");
    Node n;
    n.logical = le64(b.data() + 0x30);
    n.level = b[0x64];
    const std::uint32_t count = le32(b.data() + 0x60);
    if (count > 100000) throw std::runtime_error("Invalid Btrfs item count");
    if (n.level == 0) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t io = 0x65 + std::size_t(i) * 25;
            if (!range(b.size(), io, 25)) break;
            RawItem item;
            item.key = readKey(b, io);
            const std::uint32_t dataOff = le32(b.data() + io + 17);
            const std::uint32_t dataSize = le32(b.data() + io + 21);
            const std::size_t pos = 0x65 + dataOff;
            if (!range(b.size(), pos, dataSize)) continue;
            item.data.assign(b.begin() + static_cast<std::ptrdiff_t>(pos),
                             b.begin() + static_cast<std::ptrdiff_t>(pos + dataSize));
            item.physicalData = physical + pos;
            n.items.push_back(item);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t po = 0x65 + std::size_t(i) * 0x21;
            if (!range(b.size(), po, 0x21)) break;
            KeyPtr kp;
            kp.key = readKey(b, po);
            kp.block = le64(b.data() + po + 17);
            n.ptrs.push_back(kp);
        }
    }
    return n;
}

void BtrfsReader::collectChunks(const Node& node, std::map<std::uint64_t, bool>* seen) {
    if (!seen || (*seen)[node.logical]) return;
    (*seen)[node.logical] = true;
    if (node.level == 0) {
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            const RawItem& item = node.items[i];
            if (item.key.type != kChunkItem || item.data.size() < 0x50) continue;
            const std::uint16_t stripes = le16(item.data.data() + 0x2c);
            if (stripes < 1 || item.data.size() < 0x30 + std::size_t(stripes) * 0x20) continue;
            Chunk c;
            c.logical = item.key.offset;
            c.size = le64(item.data.data());
            c.physical = le64(item.data.data() + 0x30 + 8);
            chunks_.push_back(c);
        }
        std::sort(chunks_.begin(), chunks_.end(), [](const Chunk& a, const Chunk& b) {
            return a.logical < b.logical;
        });
        return;
    }
    for (std::size_t i = 0; i < node.ptrs.size(); ++i)
        collectChunks(readTree(node.ptrs[i].block, node.level - 1), seen);
}

std::uint64_t BtrfsReader::mapToPhysical(std::uint64_t logical) const {
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& c = chunks_[i];
        if (logical >= c.logical && logical < c.logical + c.size)
            return c.physical + (logical - c.logical);
    }
    throw std::runtime_error("No Btrfs chunk for logical address");
}

std::vector<BtrfsReader::RawItem> BtrfsReader::findItems(const Node& node, const Key& key) const {
    std::vector<RawItem> out;
    if (node.level == 0) {
        for (std::size_t i = 0; i < node.items.size(); ++i)
            if (node.items[i].key.objectId == key.objectId && node.items[i].key.type == key.type)
                out.push_back(node.items[i]);
        return out;
    }
    if (node.ptrs.empty() || node.ptrs[0].key.objectId > key.objectId) return out;
    std::size_t i = 1;
    while (i < node.ptrs.size() && node.ptrs[i].key.objectId < key.objectId) ++i;
    for (std::size_t j = i - 1; j < node.ptrs.size(); ++j) {
        if (node.ptrs[j].key.objectId > key.objectId) break;
        std::vector<RawItem> child = findItems(readTree(node.ptrs[j].block, node.level - 1), key);
        out.insert(out.end(), child.begin(), child.end());
    }
    return out;
}

BtrfsReader::RawItem BtrfsReader::findFirst(const Node& node, const Key& key) const {
    std::vector<RawItem> items = findItems(node, key);
    return items.empty() ? RawItem() : items.front();
}

BtrfsReader::Node BtrfsReader::fsTree(std::uint64_t treeId) const {
    std::map<std::uint64_t, Node>::const_iterator cached = fsTrees_.find(treeId);
    if (cached != fsTrees_.end()) return cached->second;
    RawItem root = findFirst(rootTree_, Key{treeId, kRootItem, 0});
    if (root.data.size() < 239) return Node();
    const std::uint64_t byteNr = le64(root.data.data() + 176);
    const std::uint8_t level = root.data[238];
    Node tree = readTree(byteNr, level);
    fsTrees_[treeId] = tree;
    return tree;
}

BtrfsReader::Inode BtrfsReader::parseInode(const RawItem& item) const {
    Inode ino;
    if (item.data.size() < 160) return ino;
    ino.valid = true;
    ino.size = le64(item.data.data() + 16);
    ino.mode = le32(item.data.data() + 52);
    return ino;
}

BtrfsReader::Entry BtrfsReader::rootEntry() const {
    RawItem rootDir = findFirst(rootTree_, Key{rootDirObjectId_, kDirItem, 0});
    if (rootDir.data.size() < 0x1e) throw std::runtime_error("Missing Btrfs default root");
    Entry e;
    e.name.clear();
    e.treeId = le64(rootDir.data.data());
    e.objectId = e.treeId;
    e.childType = rootDir.data[0x1d];
    e.subtree = true;
    RawItem rootItem = findFirst(rootTree_, Key{e.treeId, kRootItem, 0});
    if (rootItem.data.size() >= 239) e.objectId = le64(rootItem.data.data() + 168);
    return e;
}

std::vector<BtrfsReader::Entry> BtrfsReader::readDirectory(const Entry& dir) const {
    std::uint64_t treeId = dir.treeId;
    std::uint64_t objectId = dir.objectId;
    if (dir.subtree) {
        treeId = dir.treeId;
        RawItem rootItem = findFirst(rootTree_, Key{treeId, kRootItem, 0});
        if (rootItem.data.size() >= 239) objectId = le64(rootItem.data.data() + 168);
    }
    Node tree = fsTree(treeId);
    std::vector<RawItem> indexes = findItems(tree, Key{objectId, kDirIndex, 0});
    std::vector<Entry> out;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        const RawItem& item = indexes[i];
        if (item.data.size() < 0x1e) continue;
        const std::uint64_t childObject = le64(item.data.data());
        const std::uint8_t childItemType = item.data[8];
        const std::uint16_t dataLen = le16(item.data.data() + 0x19);
        const std::uint16_t nameLen = le16(item.data.data() + 0x1b);
        if (item.data.size() < 0x1e + std::size_t(nameLen) + dataLen) continue;
        Entry e;
        e.name.assign(reinterpret_cast<const char*>(item.data.data() + 0x1e), nameLen);
        e.treeId = treeId;
        e.objectId = childObject;
        e.childType = item.data[0x1d];
        if (childItemType == kRootItem) {
            e.subtree = true;
            e.treeId = childObject;
            e.objectId = childObject;
            Node sub = fsTree(childObject);
            RawItem ino = findFirst(sub, Key{kFirstChunkTree, kInodeItem, 0});
            e.inode = parseInode(ino);
        } else {
            RawItem ino = findFirst(tree, Key{childObject, kInodeItem, 0});
            e.inode = parseInode(ino);
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.isDirectory() != b.isDirectory()) return a.isDirectory() > b.isDirectory();
        return a.name < b.name;
    });
    return out;
}

std::vector<DiscEntry> BtrfsReader::list(const std::string& dirPath) const {
    Entry dir;
    if (!resolvePath(dirPath, &dir) || !dir.isDirectory()) return std::vector<DiscEntry>();
    std::vector<Entry> entries = readDirectory(dir);
    std::vector<DiscEntry> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        DiscEntry e;
        e.name = entries[i].name;
        e.isDirectory = entries[i].isDirectory();
        e.length = e.isDirectory ? 0 : std::int64_t(entries[i].inode.size);
        out.push_back(e);
    }
    return out;
}

ByteStorePtr BtrfsReader::openFile(const std::string& path) const {
    Entry file;
    if (!resolvePath(path, &file) || file.isDirectory()) return ByteStorePtr();
    return contentStore(file);
}

bool BtrfsReader::resolvePath(const std::string& path, Entry* out) const {
    if (!out) return false;
    Entry cur = rootEntry();
    const std::vector<std::string> parts = splitPath(path);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::vector<Entry> children = readDirectory(cur);
        bool found = false;
        for (std::size_t j = 0; j < children.size(); ++j) {
            if (children[j].name == parts[i]) {
                cur = children[j];
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    *out = cur;
    return true;
}

std::vector<BtrfsReader::Extent> BtrfsReader::readExtents(std::uint64_t treeId,
                                                          std::uint64_t objectId) const {
    Node tree = fsTree(treeId);
    std::vector<RawItem> items = findItems(tree, Key{objectId, kExtentData, 0});
    std::vector<Extent> out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const RawItem& item = items[i];
        if (item.data.size() < 0x15) continue;
        Extent e;
        e.fileOffset = item.key.offset;
        e.decodedSize = le64(item.data.data() + 8);
        e.compression = item.data[0x10];
        e.type = item.data[0x14];
        if (e.type == 0) {
            e.inlineData.assign(item.data.begin() + 0x15, item.data.end());
            e.logicalSize = e.decodedSize ? e.decodedSize : e.inlineData.size();
        } else if (e.type == 1 && item.data.size() >= 0x35) {
            e.extentAddress = le64(item.data.data() + 0x15);
            e.extentSize = le64(item.data.data() + 0x1d);
            e.extentOffset = le64(item.data.data() + 0x25);
            e.logicalSize = le64(item.data.data() + 0x2d);
        } else {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

ByteStorePtr BtrfsReader::contentStore(const Entry& file) const {
    std::vector<Extent> extents = readExtents(file.treeId, file.objectId);
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const Extent& e = extents[i];
        if ((e.compression != 0 && e.compression != 1 && e.compression != 2) ||
            (e.type != 0 && e.type != 1))
            return ByteStorePtr();
    }
    return std::make_shared<BtrfsFileStore>(disc_, extents, chunks_, file.inode.size);
}

}  // namespace fs
}  // namespace peare
