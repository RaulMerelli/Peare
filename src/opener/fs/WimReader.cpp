#include "WimReader.h"

#include "BlockDecompressor.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace peare {
namespace fs {
namespace {

// WIM header / resource flag constants (DiscUtils FileFlags / ResourceFlags).
const std::uint32_t kFlagXpress = 0x00020000;
const std::uint32_t kFlagLzx = 0x00040000;
const std::uint8_t kResMetaData = 0x02;
const std::uint8_t kResCompressed = 0x04;
const std::uint32_t kAttrDirectory = 0x00000010;

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return std::uint16_t(b[o]) | (std::uint16_t(b[o + 1]) << 8);
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return std::uint32_t(b[o]) | (std::uint32_t(b[o + 1]) << 8) |
           (std::uint32_t(b[o + 2]) << 16) | (std::uint32_t(b[o + 3]) << 24);
}
std::int64_t le64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return std::int64_t(le32(b, o)) | (std::int64_t(le32(b, o + 4)) << 32);
}

std::int64_t roundUp(std::int64_t v, std::int64_t a) { return ((v + a - 1) / a) * a; }

// UTF-16LE (n bytes) -> UTF-8, stopping at a NUL code unit.
std::string utf16ToUtf8(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes && off + i + 1 < b.size(); i += 2) {
        const std::uint32_t c = std::uint32_t(b[off + i]) | (std::uint32_t(b[off + i + 1]) << 8);
        if (c == 0) break;
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// A compressed WIM resource: chunk table + LZX/XPRESS decode per chunk, one
// chunk cached (ported from DiscUtils FileResourceStream).
class WimChunkStore : public IByteStore {
public:
    WimChunkStore(ByteStorePtr disc, std::int64_t fileOffset, std::int64_t compressedSize,
                  std::int64_t originalSize, std::int32_t chunkSize, bool compressed,
                  BlockDecompressorPtr decomp)
        : disc_(std::move(disc)), fileOffset_(fileOffset), compressedSize_(compressedSize),
          originalSize_(originalSize), chunkSize_(chunkSize), compressed_(compressed),
          decomp_(std::move(decomp)) {
        numChunks_ = int((originalSize_ + chunkSize_ - 1) / chunkSize_);
        if (numChunks_ <= 0) numChunks_ = 1;
        chunkOffsets_.assign(numChunks_, 0);
        chunkLengths_.assign(numChunks_, 0);
        const std::int64_t tableBytes = std::int64_t(numChunks_ - 1) * 4;
        std::vector<std::uint8_t> tbl(static_cast<std::size_t>(tableBytes));
        if (tableBytes > 0) disc_->readExactly(fileOffset_, tbl.data(), int(tableBytes));
        for (int i = 1; i < numChunks_; ++i) {
            chunkOffsets_[i] = le32(tbl, std::size_t((i - 1) * 4));
            chunkLengths_[i - 1] = chunkOffsets_[i] - chunkOffsets_[i - 1];
        }
        offsetDelta_ = tableBytes;
        chunkLengths_[numChunks_ - 1] = compressedSize_ - offsetDelta_ - chunkOffsets_[numChunks_ - 1];
    }

    std::int64_t capacity() const override { return originalSize_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= originalSize_) return 0;
        int produced = 0;
        while (count > 0 && pos < originalSize_) {
            const int chunk = int(pos / chunkSize_);
            const int chunkOff = int(pos % chunkSize_);
            if (!ensureChunk(chunk)) break;
            const int avail = int(cache_.size()) - chunkOff;
            if (avail <= 0) break;
            int n = count < avail ? count : avail;
            const std::int64_t rem = originalSize_ - pos;
            if (n > rem) n = int(rem);
            std::memcpy(dst + produced, cache_.data() + chunkOff, std::size_t(n));
            pos += n; produced += n; count -= n;
        }
        return produced;
    }

private:
    bool ensureChunk(int chunk) const {
        if (cachedChunk_ == chunk) return true;
        if (chunk < 0 || chunk >= numChunks_) return false;
        const int target = (chunk == numChunks_ - 1)
            ? int(originalSize_ - std::int64_t(chunk) * chunkSize_)
            : chunkSize_;
        const int compressedLen = int(chunkLengths_[chunk]);
        if (compressedLen < 0) return false;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(compressedLen));
        if (compressedLen > 0)
            disc_->readExactly(fileOffset_ + offsetDelta_ + chunkOffsets_[chunk], raw.data(), compressedLen);

        cache_.assign(std::size_t(target), 0);
        if (!decomp_ || !compressed_ || compressedLen == target) {
            const int n = compressedLen < target ? compressedLen : target;
            std::memcpy(cache_.data(), raw.data(), std::size_t(n));
        } else {
            std::size_t out = 0;
            if (!decomp_->tryDecompress(raw.data(), raw.size(), cache_.data(),
                                        std::size_t(target), out))
                return false;
        }
        cachedChunk_ = chunk;
        return true;
    }

    ByteStorePtr disc_;
    std::int64_t fileOffset_, compressedSize_, originalSize_;
    std::int32_t chunkSize_;
    bool compressed_;
    BlockDecompressorPtr decomp_;
    int numChunks_ = 0;
    std::vector<std::int64_t> chunkOffsets_, chunkLengths_;
    std::int64_t offsetDelta_ = 0;
    mutable int cachedChunk_ = -1;
    mutable std::vector<std::uint8_t> cache_;
};

}  // namespace

WimReader::WimReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try {
        parse();
    } catch (...) {
        valid_ = false;
        if (error_.empty()) error_ = "WIM parse error";
    }
}

WimReader::ResHeader WimReader::readResHeader(const std::vector<std::uint8_t>& buf, std::size_t off) {
    ResHeader h;
    const std::int64_t field0 = le64(buf, off);
    h.flags = std::uint8_t((field0 >> 56) & 0xFF);
    h.compressedSize = field0 & 0x00FFFFFFFFFFFFFFLL;
    h.fileOffset = le64(buf, off + 8);
    h.originalSize = le64(buf, off + 16);
    return h;
}

ByteStorePtr WimReader::makeResourceStore(const ResHeader& hdr) const {
    if ((hdr.flags & kResCompressed) == 0)
        return std::make_shared<SubStore>(disc_, hdr.fileOffset, hdr.compressedSize);

    BlockDecompressorPtr decomp;
    if (fileFlags_ & kFlagLzx)
        decomp = std::make_shared<LzxWimDecompressor>(std::size_t(chunkSize_));
    else if (fileFlags_ & kFlagXpress)
        decomp = std::make_shared<XpressHuffmanDecompressor>();
    return std::make_shared<WimChunkStore>(disc_, hdr.fileOffset, hdr.compressedSize,
                                           hdr.originalSize, chunkSize_, true, decomp);
}

std::vector<std::uint8_t> WimReader::materialize(const ResHeader& hdr) const {
    return makeResourceStore(hdr)->readAll();
}

void WimReader::parse() {
    const std::vector<std::uint8_t> header = disc_->readRange(0, 512);
    if (header.size() < 148 ||
        std::memcmp(header.data(), "MSWIM\0\0\0", 8) != 0) {
        error_ = "Not a valid WIM file";
        return;
    }
    fileFlags_ = le32(header, 16);
    chunkSize_ = std::int32_t(le32(header, 20));
    if (chunkSize_ <= 0) chunkSize_ = 32768;
    offsetTable_ = readResHeader(header, 48);

    // Lookup table: SHA1 -> resource; capture the first metadata resource.
    const std::vector<std::uint8_t> table = materialize(offsetTable_);
    const std::size_t kResInfo = 24 + 26;  // ShortResourceHeader + 26
    bool haveMeta = false;
    for (std::size_t off = 0; off + kResInfo <= table.size(); off += kResInfo) {
        const ResHeader rh = readResHeader(table, off);
        std::vector<std::uint8_t> hash(table.begin() + off + 24 + 6,
                                       table.begin() + off + 24 + 6 + 20);
        if ((rh.flags & kResMetaData) != 0) {
            if (!haveMeta) { metadata_ = rh; haveMeta = true; }
        } else {
            resources_[hash] = rh;
        }
    }
    if (!haveMeta) { error_ = "WIM has no image metadata"; return; }

    // Metadata stream: security block then the directory tree.
    meta_ = materialize(metadata_);
    if (meta_.size() < 8) { error_ = "WIM metadata truncated"; return; }
    const std::uint32_t totalLength = le32(meta_, 0);
    const std::int64_t rawRoot = roundUp(std::int64_t(totalLength), 8);
    // The record at rawRoot is the (unnamed) root directory entry; the actual
    // top-level children begin at its subdirOffset.
    rootDirPos_ = rawRoot;
    if (std::size_t(rawRoot) + 24 <= meta_.size()) {
        const std::int64_t rootSub = le64(meta_, std::size_t(rawRoot) + 16);
        if (rootSub != 0) rootDirPos_ = rootSub;
    }

    valid_ = true;
}

const std::vector<WimReader::DirEntry>& WimReader::directoryAt(std::int64_t offset) const {
    auto it = dirCache_.find(offset);
    if (it != dirCache_.end()) return it->second;

    std::vector<DirEntry> dir;
    std::int64_t pos = (offset == 0) ? rootDirPos_ : offset;
    while (pos >= 0 && std::size_t(pos) + 8 <= meta_.size()) {
        const std::int64_t length = le64(meta_, std::size_t(pos));
        if (length == 0) break;  // end of this directory
        if (std::size_t(pos) + 102 > meta_.size()) break;
        DirEntry e;
        const std::uint32_t attributes = le32(meta_, std::size_t(pos) + 8);
        e.subdirOffset = le64(meta_, std::size_t(pos) + 16);
        e.hash.assign(meta_.begin() + std::size_t(pos) + 64, meta_.begin() + std::size_t(pos) + 84);
        const int fileNameLen = le16(meta_, std::size_t(pos) + 100);
        if (fileNameLen > 0)
            e.name = utf16ToUtf8(meta_, std::size_t(pos) + 102, std::size_t(fileNameLen));
        e.isDirectory = (attributes & kAttrDirectory) != 0;
        // File length comes from the content resource (originalSize).
        bool allZero = true;
        for (std::uint8_t h : e.hash) if (h) { allZero = false; break; }
        if (!e.isDirectory && !allZero) {
            auto r = resources_.find(e.hash);
            if (r != resources_.end()) e.length = r->second.originalSize;
        }
        if (!e.name.empty()) dir.push_back(std::move(e));
        pos += length;
    }
    auto res = dirCache_.emplace(offset, std::move(dir));
    return res.first->second;
}

const WimReader::DirEntry* WimReader::entryForPath(const std::string& path) const {
    // Split on '/' and '\\'.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);

    const std::vector<DirEntry>* dir = &directoryAt(0);
    const DirEntry* found = nullptr;
    for (const std::string& seg : parts) {
        found = nullptr;
        for (const DirEntry& e : *dir) {
            // Case-insensitive compare.
            if (e.name.size() == seg.size()) {
                bool eq = true;
                for (std::size_t i = 0; i < seg.size(); ++i) {
                    char a = e.name[i], b = seg[i];
                    if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                    if (a != b) { eq = false; break; }
                }
                if (eq) { found = &e; break; }
            }
        }
        if (!found) return nullptr;
        if (found->subdirOffset != 0) dir = &directoryAt(found->subdirOffset);
    }
    return found;
}

std::vector<DiscEntry> WimReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    const std::vector<DirEntry>* dir = nullptr;
    if (dirPath.empty() || dirPath == "/" || dirPath == "\\") {
        dir = &directoryAt(0);
    } else {
        const DirEntry* e = entryForPath(dirPath);
        if (!e || !e->isDirectory) return out;
        dir = &directoryAt(e->subdirOffset);
    }
    for (const DirEntry& e : *dir) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.length;
        out.push_back(d);
    }
    return out;
}

ByteStorePtr WimReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    const DirEntry* e = entryForPath(path);
    if (!e || e->isDirectory) return nullptr;
    bool allZero = true;
    for (std::uint8_t h : e->hash) if (h) { allZero = false; break; }
    if (allZero) return std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
    auto r = resources_.find(e->hash);
    if (r == resources_.end()) return nullptr;
    return makeResourceStore(r->second);
}

}  // namespace fs
}  // namespace peare
