#include "FatReader.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace peare {
namespace fs {
namespace {

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return std::uint16_t(b[o]) | (std::uint16_t(b[o + 1]) << 8);
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return std::uint32_t(b[o]) | (std::uint32_t(b[o + 1]) << 8) |
           (std::uint32_t(b[o + 2]) << 16) | (std::uint32_t(b[o + 3]) << 24);
}

void appendUtf8(std::string& out, std::uint32_t c) {
    if (c == 0 || c == 0xFFFF) return;
    if (c < 0x80) out.push_back(char(c));
    else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
    else { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
}

}  // namespace

FatReader::FatReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try { parse(); }
    catch (...) { valid_ = false; if (error_.empty()) error_ = "FAT parse error"; }
}

void FatReader::parse() {
    const std::vector<std::uint8_t> bpb = disc_->readRange(0, 512);
    if (bpb.size() < 512) { error_ = "Truncated boot sector"; return; }

    bytesPerSec_ = le16(bpb, 11);
    secPerClus_ = bpb[13];
    rsvdSec_ = le16(bpb, 14);
    fatCount_ = bpb[16];
    rootEntCnt_ = le16(bpb, 17);
    const std::uint16_t totSec16 = le16(bpb, 19);
    const std::uint16_t fatSz16 = le16(bpb, 22);
    const std::uint32_t totSec32 = le32(bpb, 32);
    const std::uint32_t fatSz32 = le32(bpb, 36);
    rootClus_ = le32(bpb, 44);

    if (bytesPerSec_ == 0 || secPerClus_ == 0) { error_ = "Invalid BPB"; return; }

    const std::uint32_t rootDirSectors = (rootEntCnt_ * 32u + bytesPerSec_ - 1) / bytesPerSec_;
    fatSz_ = fatSz16 != 0 ? fatSz16 : fatSz32;
    const std::uint32_t totalSec = totSec16 != 0 ? totSec16 : totSec32;
    if (fatSz_ == 0 || totalSec == 0) { error_ = "Invalid BPB sizes"; return; }
    const std::uint32_t dataSec = totalSec - (rsvdSec_ + fatCount_ * fatSz_ + rootDirSectors);
    const std::uint32_t countOfClusters = dataSec / secPerClus_;
    type_ = countOfClusters < 4085 ? FatType::Fat12
          : countOfClusters < 65525 ? FatType::Fat16 : FatType::Fat32;
    friendly_ = std::string("FAT") + (type_ == FatType::Fat12 ? "12" : type_ == FatType::Fat16 ? "16" : "32");

    firstDataSector_ = std::int64_t(rsvdSec_) + std::int64_t(fatCount_) * fatSz_ + rootDirSectors;
    rootDirStart_ = (std::int64_t(rsvdSec_) + std::int64_t(fatCount_) * fatSz_) * bytesPerSec_;
    rootDirBytes_ = std::int64_t(rootEntCnt_) * 32;

    // Materialise the (active/first) FAT table.
    const std::int64_t fatStart = std::int64_t(rsvdSec_) * bytesPerSec_;
    const std::int64_t fatBytes = std::int64_t(fatSz_) * bytesPerSec_;
    fat_ = disc_->readRange(fatStart, fatBytes);

    // Sanity: FAT32 root cluster must be >= 2.
    if (type_ == FatType::Fat32 && rootClus_ < 2) { error_ = "Invalid FAT32 root cluster"; return; }
    valid_ = true;
}

bool FatReader::isEndOfChain(std::uint32_t v) const {
    switch (type_) {
    case FatType::Fat12: return (v & 0x0FFF) >= 0x0FF8;
    case FatType::Fat16: return (v & 0xFFFF) >= 0xFFF8;
    default:             return (v & 0x0FFFFFF8) >= 0x0FFFFFF8;
    }
}

std::uint32_t FatReader::nextCluster(std::uint32_t cluster) const {
    switch (type_) {
    case FatType::Fat16:
        return le16(fat_, std::size_t(cluster) * 2);
    case FatType::Fat32:
        return le32(fat_, std::size_t(cluster) * 4) & 0x0FFFFFFF;
    default: {  // Fat12
        const std::size_t idx = cluster + cluster / 2;
        const std::uint16_t v = le16(fat_, idx);
        return (cluster & 1) ? std::uint32_t((v >> 4) & 0x0FFF) : std::uint32_t(v & 0x0FFF);
    }
    }
}

std::vector<std::uint32_t> FatReader::chain(std::uint32_t firstCluster) const {
    std::vector<std::uint32_t> out;
    std::uint32_t c = firstCluster;
    // Bound the walk to the number of FAT entries to defend against loops.
    const std::size_t maxEntries = fat_.size() ? fat_.size() : 1;
    while (c >= 2 && out.size() < maxEntries) {
        // Bad cluster / free cluster ends the chain.
        if ((type_ == FatType::Fat12 && (c & 0x0FFF) == 0x0FF7) ||
            (type_ == FatType::Fat16 && (c & 0xFFFF) == 0xFFF7) ||
            (type_ == FatType::Fat32 && (c & 0x0FFFFFFF) == 0x0FFFFFF7))
            break;
        out.push_back(c);
        const std::uint32_t next = nextCluster(c);
        if (isEndOfChain(next))
            break;
        c = next;
    }
    return out;
}

std::int64_t FatReader::clusterPos(std::uint32_t cluster) const {
    return (firstDataSector_ + std::int64_t(cluster - 2) * secPerClus_) * bytesPerSec_;
}

std::vector<std::uint8_t> FatReader::readClusterChain(std::uint32_t firstCluster) const {
    std::vector<std::uint8_t> out;
    const std::int64_t clusterSize = std::int64_t(secPerClus_) * bytesPerSec_;
    for (std::uint32_t c : chain(firstCluster)) {
        const std::size_t base = out.size();
        out.resize(base + std::size_t(clusterSize));
        disc_->readExactly(clusterPos(c), out.data() + base, int(clusterSize));
    }
    return out;
}

ByteStorePtr FatReader::clusterContent(std::uint32_t firstCluster, std::uint32_t size) const {
    if (firstCluster < 2 || size == 0)
        return std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
    const std::int64_t clusterSize = std::int64_t(secPerClus_) * bytesPerSec_;
    const std::vector<std::uint32_t> clusters = chain(firstCluster);
    // Coalesce contiguous clusters into runs, one SubStore per run (zero-copy).
    std::vector<ByteStorePtr> parts;
    std::size_t i = 0;
    while (i < clusters.size()) {
        std::size_t j = i;
        while (j + 1 < clusters.size() && clusters[j + 1] == clusters[j] + 1) ++j;
        const std::int64_t runStart = clusterPos(clusters[i]);
        const std::int64_t runLen = std::int64_t(j - i + 1) * clusterSize;
        parts.push_back(std::make_shared<SubStore>(disc_, runStart, runLen));
        i = j + 1;
    }
    ByteStorePtr concat = std::make_shared<ConcatStore>(std::move(parts));
    // Truncate to the file's real length (the last cluster is usually partial).
    return std::make_shared<SubStore>(concat, 0, std::int64_t(size));
}

std::vector<FatReader::DirEntry> FatReader::parseDirectory(const std::vector<std::uint8_t>& buf) const {
    std::vector<DirEntry> out;
    std::vector<std::uint16_t> lfn;  // assembled UTF-16 code units, index = (seq-1)*13+k
    for (std::size_t off = 0; off + 32 <= buf.size(); off += 32) {
        const std::uint8_t first = buf[off];
        if (first == 0x00) break;              // end of directory
        const std::uint8_t attr = buf[off + 11];
        if (first == 0xE5) { lfn.clear(); continue; }  // deleted

        if ((attr & 0x0F) == 0x0F) {           // LFN entry
            const int seq = first & 0x3F;
            if (seq >= 1) {
                if (lfn.size() < std::size_t(seq) * 13) lfn.resize(std::size_t(seq) * 13, 0xFFFF);
                std::size_t p = std::size_t(seq - 1) * 13;
                for (int k = 0; k < 5; ++k) lfn[p + k] = le16(buf, off + 1 + k * 2);
                for (int k = 0; k < 6; ++k) lfn[p + 5 + k] = le16(buf, off + 14 + k * 2);
                for (int k = 0; k < 2; ++k) lfn[p + 11 + k] = le16(buf, off + 28 + k * 2);
            }
            continue;
        }

        // 8.3 entry. Skip volume-label entries.
        if (attr & 0x08) { lfn.clear(); continue; }

        DirEntry e;
        if (!lfn.empty()) {
            for (std::uint16_t c : lfn) { if (c == 0 || c == 0xFFFF) break; appendUtf8(e.name, c); }
        }
        lfn.clear();
        if (e.name.empty()) {  // decode the 8.3 short name
            std::string base, ext;
            for (int k = 0; k < 8; ++k) { char c = char(buf[off + k]); if (c != ' ') base.push_back(c); }
            for (int k = 0; k < 3; ++k) { char c = char(buf[off + 8 + k]); if (c != ' ') ext.push_back(c); }
            if (!base.empty() && std::uint8_t(base[0]) == 0x05) base[0] = char(0xE5);
            e.name = ext.empty() ? base : base + "." + ext;
        }
        if (e.name == "." || e.name == "..") continue;

        e.isDirectory = (attr & 0x10) != 0;
        e.firstCluster = (std::uint32_t(le16(buf, off + 20)) << 16) | le16(buf, off + 26);
        e.size = le32(buf, off + 28);
        if (!e.name.empty()) out.push_back(std::move(e));
    }
    return out;
}

std::vector<FatReader::DirEntry> FatReader::directory(const std::string& dirPath) const {
    // Resolve the directory's cluster (root is special for FAT12/16).
    if (dirPath.empty() || dirPath == "/" || dirPath == "\\") {
        if (type_ != FatType::Fat32) {
            std::vector<std::uint8_t> buf = disc_->readRange(rootDirStart_, rootDirBytes_);
            return parseDirectory(buf);
        }
        return parseDirectory(readClusterChain(rootClus_));
    }
    DirEntry store;
    const DirEntry* e = find(dirPath, &store);
    if (!e || !e->isDirectory) return {};
    return parseDirectory(readClusterChain(e->firstCluster));
}

const FatReader::DirEntry* FatReader::find(const std::string& path, DirEntry* store) const {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return nullptr;

    std::vector<DirEntry> dir = directory(std::string());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const DirEntry* found = nullptr;
        for (const DirEntry& d : dir) {
            if (d.name.size() != parts[i].size()) continue;
            bool eq = true;
            for (std::size_t k = 0; k < d.name.size(); ++k) {
                char a = d.name[k], b = parts[i][k];
                if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                if (a != b) { eq = false; break; }
            }
            if (eq) { found = &d; break; }
        }
        if (!found) return nullptr;
        if (i + 1 == parts.size()) { *store = *found; return store; }
        if (!found->isDirectory) return nullptr;
        dir = parseDirectory(readClusterChain(found->firstCluster));
    }
    return nullptr;
}

std::vector<DiscEntry> FatReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    for (const DirEntry& e : directory(dirPath)) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.size;
        out.push_back(d);
    }
    return out;
}

ByteStorePtr FatReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    DirEntry store;
    const DirEntry* e = find(path, &store);
    if (!e || e->isDirectory) return nullptr;
    return clusterContent(e->firstCluster, e->size);
}

}  // namespace fs
}  // namespace peare
