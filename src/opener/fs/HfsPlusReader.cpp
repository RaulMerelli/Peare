#include "HfsPlusReader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace peare {
namespace fs {
namespace {

std::uint16_t be16(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return std::uint16_t((std::uint16_t(b[o]) << 8) | b[o + 1]);
}

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return (std::uint32_t(b[o]) << 24) | (std::uint32_t(b[o + 1]) << 16) |
           (std::uint32_t(b[o + 2]) << 8) | std::uint32_t(b[o + 3]);
}

std::uint64_t be64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return (std::uint64_t(be32(b, o)) << 32) | be32(b, o + 4);
}

bool validRange(std::size_t size, std::size_t off, std::size_t len) {
    return off <= size && len <= size - off;
}

void appendUtf8(std::string* s, std::uint32_t cp) {
    if (cp <= 0x7f) {
        s->push_back(char(cp));
    } else if (cp <= 0x7ff) {
        s->push_back(char(0xc0 | (cp >> 6)));
        s->push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        s->push_back(char(0xe0 | (cp >> 12)));
        s->push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        s->push_back(char(0x80 | (cp & 0x3f)));
    } else {
        s->push_back(char(0xf0 | (cp >> 18)));
        s->push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        s->push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        s->push_back(char(0x80 | (cp & 0x3f)));
    }
}

std::string readUtf16BeName(const std::vector<std::uint8_t>& b, std::size_t off,
                            std::size_t maxLen, std::size_t* bytesUsed) {
    if (bytesUsed) *bytesUsed = 0;
    if (!validRange(b.size(), off, 2)) return std::string();
    const std::uint16_t chars = be16(b, off);
    const std::size_t bytes = std::size_t(chars) * 2;
    if (!validRange(b.size(), off + 2, bytes) || 2 + bytes > maxLen) return std::string();
    if (bytesUsed) *bytesUsed = 2 + bytes;

    std::string out;
    for (std::size_t i = 0; i < chars; ++i) {
        const std::uint16_t w1 = be16(b, off + 2 + i * 2);
        if (w1 >= 0xd800 && w1 <= 0xdbff && i + 1 < chars) {
            const std::uint16_t w2 = be16(b, off + 2 + (i + 1) * 2);
            if (w2 >= 0xdc00 && w2 <= 0xdfff) {
                const std::uint32_t cp =
                    0x10000 + (((std::uint32_t(w1) - 0xd800) << 10) |
                               (std::uint32_t(w2) - 0xdc00));
                appendUtf8(&out, cp);
                ++i;
                continue;
            }
        }
        appendUtf8(&out, w1);
    }
    return out;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == '/' || path[pos] == '\\')) ++pos;
        const std::size_t start = pos;
        while (pos < path.size() && path[pos] != '/' && path[pos] != '\\') ++pos;
        if (pos > start) parts.push_back(path.substr(start, pos - start));
    }
    return parts;
}

class ExtentStore final : public IByteStore {
public:
    ExtentStore(ByteStorePtr parent, std::vector<HfsPlusReader::Extent> extents,
                std::uint32_t blockSize, std::uint64_t length)
        : parent_(std::move(parent)), extents_(std::move(extents)), blockSize_(blockSize),
          length_(length) {}

    std::int64_t capacity() const override {
        return length_ > std::uint64_t(std::numeric_limits<std::int64_t>::max())
                   ? std::numeric_limits<std::int64_t>::max()
                   : std::int64_t(length_);
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!parent_ || pos < 0 || count <= 0 || std::uint64_t(pos) >= length_) return 0;
        std::uint64_t wanted = std::uint64_t(count);
        if (wanted > length_ - std::uint64_t(pos)) wanted = length_ - std::uint64_t(pos);

        std::uint64_t logical = 0;
        int produced = 0;
        for (std::size_t i = 0; i < extents_.size() && wanted > 0; ++i) {
            const HfsPlusReader::Extent& e = extents_[i];
            if (e.blockCount == 0) break;
            const std::uint64_t bytes = std::uint64_t(e.blockCount) * blockSize_;
            if (std::uint64_t(pos) >= logical + bytes) {
                logical += bytes;
                continue;
            }
            const std::uint64_t inExtent = std::uint64_t(pos) > logical
                                               ? std::uint64_t(pos) - logical
                                               : 0;
            const std::uint64_t toRead64 = std::min<std::uint64_t>(wanted, bytes - inExtent);
            const int toRead = toRead64 > std::uint64_t(std::numeric_limits<int>::max())
                                   ? std::numeric_limits<int>::max()
                                   : int(toRead64);
            const std::int64_t physical =
                std::int64_t(std::uint64_t(e.startBlock) * blockSize_ + inExtent);
            const int n = parent_->read(physical, dst + produced, toRead);
            if (n <= 0) break;
            produced += n;
            wanted -= std::uint64_t(n);
            pos += n;
            logical += bytes;
        }
        return produced;
    }

private:
    ByteStorePtr parent_;
    std::vector<HfsPlusReader::Extent> extents_;
    std::uint32_t blockSize_;
    std::uint64_t length_;
};

}  // namespace

HfsPlusReader::HfsPlusReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    parse();
}

void HfsPlusReader::parse() {
    if (!disc_ || disc_->capacity() < 1536) {
        error_ = "Truncated HFS+ volume";
        return;
    }

    const std::vector<std::uint8_t> header = disc_->readRange(1024, 512);
    const std::uint16_t sig = be16(header, 0);
    if (sig != 0x482b && sig != 0x4858) {
        error_ = "Invalid HFS+ volume signature";
        return;
    }
    const std::uint16_t version = be16(header, 2);
    blockSize_ = be32(header, 40);
    const std::uint32_t totalBlocks = be32(header, 44);
    if (blockSize_ < 512 || (blockSize_ & (blockSize_ - 1)) != 0 || totalBlocks == 0) {
        error_ = "Invalid HFS+ allocation block size";
        return;
    }
    if (std::uint64_t(totalBlocks) * blockSize_ > std::uint64_t(disc_->capacity()) + blockSize_) {
        error_ = "HFS+ volume extends beyond source";
        return;
    }

    catalogFork_ = readFork(272);
    if (catalogFork_.logicalSize == 0 || catalogFork_.extents.empty() ||
        catalogFork_.extents[0].blockCount == 0) {
        error_ = "Missing HFS+ catalog file";
        return;
    }

    ByteStorePtr catalogStore = forkStore(catalogFork_);
    if (!catalogStore) {
        error_ = "Unsupported HFS+ catalog extents";
        return;
    }

    std::vector<std::uint8_t> node0 = catalogStore->readRange(0, 4096);
    NodeDesc desc0;
    if (node0.size() < 120) {
        error_ = "Truncated HFS+ catalog header node";
        return;
    }
    desc0.forward = be32(node0, 0);
    desc0.kind = std::int8_t(node0[8]);
    desc0.records = be16(node0, 10);
    if (desc0.kind != 1 || desc0.records == 0) {
        error_ = "Invalid HFS+ catalog header node";
        return;
    }
    if (!validRange(node0.size(), 14, 106)) {
        error_ = "Truncated HFS+ catalog header record";
        return;
    }
    nodeSize_ = be16(node0, 14 + 18);
    firstLeaf_ = be32(node0, 14 + 10);
    const std::uint16_t depth = be16(node0, 14);
    if (nodeSize_ < 512 || firstLeaf_ == 0 || depth == 0) {
        error_ = "Unsupported empty HFS+ catalog";
        return;
    }

    std::uint32_t node = firstLeaf_;
    for (int guard = 0; node != 0 && guard < 100000; ++guard) {
        std::vector<std::uint8_t> data;
        NodeDesc desc;
        if (!readNode(node, &data, &desc)) break;
        if (desc.kind == -1) parseLeafNode(data);
        node = desc.forward;
    }

    valid_ = true;
    std::ostringstream ss;
    ss << (sig == 0x4858 ? "HFSX" : "HFS+") << " volume"
       << " (version " << version << ", block " << blockSize_ << ")";
    friendly_ = ss.str();
}

HfsPlusReader::Fork HfsPlusReader::readFork(std::int64_t pos) const {
    const std::vector<std::uint8_t> b = disc_->readRange(1024 + pos, 80);
    Fork fork;
    fork.logicalSize = be64(b, 0);
    fork.totalBlocks = be32(b, 12);
    for (std::size_t i = 0; i < 8; ++i) {
        Extent e;
        e.startBlock = be32(b, 16 + i * 8);
        e.blockCount = be32(b, 20 + i * 8);
        fork.extents.push_back(e);
    }
    return fork;
}

bool HfsPlusReader::readNode(std::uint32_t nodeNumber, std::vector<std::uint8_t>* out,
                             NodeDesc* desc) const {
    if (!out || !desc) return false;
    const std::uint32_t size = nodeSize_ == 0 ? 4096 : nodeSize_;
    const std::uint64_t catalogOff = std::uint64_t(nodeNumber) * size;
    ByteStorePtr store = forkStore(catalogFork_);
    if (!store || catalogOff + size > std::uint64_t(store->capacity())) return false;
    *out = store->readRange(std::int64_t(catalogOff), size);
    if (out->size() < 14) return false;
    desc->forward = be32(*out, 0);
    desc->kind = std::int8_t((*out)[8]);
    desc->records = be16(*out, 10);
    return true;
}

void HfsPlusReader::parseLeafNode(const std::vector<std::uint8_t>& node) {
    if (node.size() < 16) return;
    const std::uint16_t records = be16(node, 10);
    for (std::uint16_t i = 0; i < records; ++i) {
        const std::size_t offPos = node.size() - 2 - std::size_t(i) * 2;
        if (!validRange(node.size(), offPos, 2)) continue;
        const std::size_t start = be16(node, offPos);
        const std::size_t end = (i == records - 1) ? (node.size() - 2 - std::size_t(records) * 2)
                                                   : be16(node, offPos - 2);
        if (end <= start || !validRange(node.size(), start, end - start)) continue;

        const std::uint16_t keyLen = be16(node, start);
        if (keyLen < 6 || !validRange(node.size(), start + 2, keyLen)) continue;
        const std::uint32_t parent = be32(node, start + 2);
        std::size_t nameBytes = 0;
        const std::string name = readUtf16BeName(node, start + 6, keyLen - 4, &nameBytes);
        if (name.empty()) continue;
        const std::size_t dataOff = start + 2 + keyLen;
        if (!validRange(node.size(), dataOff, 2)) continue;
        const std::uint16_t type = be16(node, dataOff);

        Entry e;
        e.name = name;
        e.parent = parent;
        if (type == 1) {
            if (!validRange(node.size(), dataOff, 12)) continue;
            e.directory = true;
            e.id = be32(node, dataOff + 8);
        } else if (type == 2) {
            if (!validRange(node.size(), dataOff, 248)) continue;
            e.directory = false;
            e.id = be32(node, dataOff + 8);
            e.dataFork.logicalSize = be64(node, dataOff + 88);
            e.dataFork.totalBlocks = be32(node, dataOff + 100);
            for (std::size_t x = 0; x < 8; ++x) {
                Extent ex;
                ex.startBlock = be32(node, dataOff + 104 + x * 8);
                ex.blockCount = be32(node, dataOff + 108 + x * 8);
                e.dataFork.extents.push_back(ex);
            }
            e.length = e.dataFork.logicalSize;
        } else {
            continue;
        }
        children_[parent].push_back(e);
    }
}

std::vector<DiscEntry> HfsPlusReader::list(const std::string& dirPath) const {
    Entry dir;
    if (!resolvePath(dirPath, &dir) || !dir.directory) return std::vector<DiscEntry>();
    std::vector<DiscEntry> out;
    std::map<std::uint32_t, std::vector<Entry> >::const_iterator it = children_.find(dir.id);
    if (it == children_.end()) return out;
    for (std::size_t i = 0; i < it->second.size(); ++i) {
        const Entry& e = it->second[i];
        DiscEntry de;
        de.name = e.name;
        de.isDirectory = e.directory;
        de.length = e.directory ? 0 : std::int64_t(e.length);
        out.push_back(de);
    }
    std::sort(out.begin(), out.end(), [](const DiscEntry& a, const DiscEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
    return out;
}

ByteStorePtr HfsPlusReader::openFile(const std::string& path) const {
    Entry file;
    if (!resolvePath(path, &file) || file.directory) return ByteStorePtr();
    return forkStore(file.dataFork);
}

bool HfsPlusReader::resolvePath(const std::string& path, Entry* out) const {
    if (!out) return false;
    Entry cur;
    cur.id = 2;
    cur.directory = true;
    cur.name.clear();

    const std::vector<std::string> parts = splitPath(path);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::map<std::uint32_t, std::vector<Entry> >::const_iterator it = children_.find(cur.id);
        if (it == children_.end()) return false;
        bool found = false;
        for (std::size_t j = 0; j < it->second.size(); ++j) {
            if (it->second[j].name == parts[i]) {
                cur = it->second[j];
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    *out = cur;
    return true;
}

ByteStorePtr HfsPlusReader::forkStore(const Fork& fork) const {
    if (!disc_) return ByteStorePtr();
    std::uint64_t covered = 0;
    for (std::size_t i = 0; i < fork.extents.size(); ++i)
        covered += std::uint64_t(fork.extents[i].blockCount) * blockSize_;
    if (fork.logicalSize > covered) return ByteStorePtr();
    return std::make_shared<ExtentStore>(disc_, fork.extents, blockSize_, fork.logicalSize);
}

}  // namespace fs
}  // namespace peare
