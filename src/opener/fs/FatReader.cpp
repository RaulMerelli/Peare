#include "FatReader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool readBytes(const IByteStore& source, std::int64_t offset, std::int64_t length,
               std::vector<std::uint8_t>* out) {
    if (offset < 0 || length < 0 || length > source.capacity() ||
        offset > source.capacity() - length ||
        static_cast<std::uint64_t>(length) > out->max_size()) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const int count = static_cast<int>(std::min<std::size_t>(
            bytes.size() - done, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (!source.readExactlyChecked(offset + static_cast<std::int64_t>(done),
                                       bytes.data() + done, count)) return false;
        done += static_cast<std::size_t>(count);
    }
    out->swap(bytes);
    return true;
}

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
    else if (c < 0x10000) { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
    else { out.push_back(char(0xF0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3F))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
}

}  // namespace

FatReader::FatReader(ByteStorePtr disc, ByteStorePtr clusterFilter)
    : disc_(std::move(disc)), clusterFilter_(std::move(clusterFilter)) {
    try { parse(); }
    catch (...) { valid_ = false; if (error_.empty()) error_ = "FAT parse error"; }
}

void FatReader::parse() {
    if (!disc_ || disc_->capacity() < 512) { error_ = "Truncated boot sector"; return; }
    std::vector<std::uint8_t> bpb;
    if (!readBytes(*disc_, 0, 512, &bpb)) { error_ = "Cannot read boot sector"; return; }

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

    const bool validSectorSize = bytesPerSec_ >= 512 && bytesPerSec_ <= 4096 &&
        (bytesPerSec_ & (bytesPerSec_ - 1)) == 0;
    const bool validClusterSize = secPerClus_ != 0 && secPerClus_ <= 128 &&
        (secPerClus_ & (secPerClus_ - 1)) == 0;
    if (!validSectorSize || !validClusterSize || rsvdSec_ == 0 ||
        fatCount_ == 0 || fatCount_ > 2) {
        error_ = "Invalid FAT BPB";
        return;
    }

    const std::uint64_t rootDirSectors =
        (static_cast<std::uint64_t>(rootEntCnt_) * 32 + bytesPerSec_ - 1) / bytesPerSec_;
    fatSz_ = fatSz16 != 0 ? fatSz16 : fatSz32;
    const std::uint32_t totalSec = totSec16 != 0 ? totSec16 : totSec32;
    if (fatSz_ == 0 || totalSec == 0) { error_ = "Invalid BPB sizes"; return; }

    const std::uint64_t overhead = static_cast<std::uint64_t>(rsvdSec_) +
        static_cast<std::uint64_t>(fatCount_) * fatSz_ + rootDirSectors;
    const std::uint64_t availableSec = static_cast<std::uint64_t>(disc_->capacity()) / bytesPerSec_;
    if (overhead >= totalSec || totalSec > availableSec) {
        error_ = "FAT volume exceeds source bounds";
        return;
    }
    const std::uint64_t dataSec = static_cast<std::uint64_t>(totalSec) - overhead;
    clusterCount_ = static_cast<std::uint32_t>(dataSec / secPerClus_);
    if (clusterCount_ == 0) { error_ = "FAT volume has no data clusters"; return; }
    type_ = clusterCount_ < 4085 ? FatType::Fat12
          : clusterCount_ < 65525 ? FatType::Fat16 : FatType::Fat32;
    friendly_ = std::string("FAT") +
        (type_ == FatType::Fat12 ? "12" : type_ == FatType::Fat16 ? "16" : "32");

    firstDataSector_ = static_cast<std::int64_t>(overhead);
    rootDirStart_ = (static_cast<std::int64_t>(rsvdSec_) +
                     static_cast<std::int64_t>(fatCount_) * fatSz_) * bytesPerSec_;
    rootDirBytes_ = static_cast<std::int64_t>(rootEntCnt_) * 32;

    std::uint32_t activeFat = 0;
    if (type_ == FatType::Fat32) {
        const std::uint16_t extFlags = le16(bpb, 40);
        if (extFlags & 0x0080) {
            activeFat = extFlags & 0x000F;
            if (activeFat >= fatCount_) { error_ = "Invalid FAT32 active FAT"; return; }
        }
        if (rootClus_ < 2 || rootClus_ >= clusterCount_ + 2) {
            error_ = "Invalid FAT32 root cluster";
            return;
        }
    }

    const std::int64_t fatStart =
        (static_cast<std::int64_t>(rsvdSec_) + static_cast<std::int64_t>(activeFat) * fatSz_) *
        bytesPerSec_;
    const std::int64_t fatBytes = static_cast<std::int64_t>(fatSz_) * bytesPerSec_;
    if (!readBytes(*disc_, fatStart, fatBytes, &fat_)) {
        error_ = "Cannot read FAT table";
        return;
    }
    if (clusterFilter_) {
        const std::int64_t length = clusterFilter_->capacity();
        const std::int64_t needed = (static_cast<std::int64_t>(clusterCount_) + 7) / 8;
        if (length < 0 || !clusterFilter_->validate() ||
            !readBytes(*clusterFilter_, 0, std::min(length, needed), &filter_) ||
            !clusterFilter_->validate()) {
            error_ = "Cannot read FAT cluster filter";
            return;
        }
        std::uint8_t tail[4096];
        for (std::int64_t offset = static_cast<std::int64_t>(filter_.size()); offset < length;) {
            const int size = static_cast<int>(std::min<std::int64_t>(length - offset, sizeof(tail)));
            if (!clusterFilter_->readExactlyChecked(offset, tail, size)) {
                error_ = "Cannot read FAT cluster filter";
                return;
            }
            offset += size;
        }
        if (!clusterFilter_->validate()) { error_ = "Cannot read FAT cluster filter"; return; }
    }
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
    std::set<std::uint32_t> visited;
    std::uint32_t c = firstCluster;
    const std::size_t maxEntries = static_cast<std::size_t>(clusterCount_) + 2;
    while (c >= 2 && c < clusterCount_ + 2 && out.size() < maxEntries &&
           visited.insert(c).second) {
        if ((type_ == FatType::Fat12 && (c & 0x0FFF) == 0x0FF7) ||
            (type_ == FatType::Fat16 && (c & 0xFFFF) == 0xFFF7) ||
            (type_ == FatType::Fat32 && (c & 0x0FFFFFFF) == 0x0FFFFFF7))
            break;
        out.push_back(c);
        const std::uint32_t next = nextCluster(c);
        if (isEndOfChain(next)) break;
        if (next == 0 || next == 1) break;
        c = next;
    }
    return out;
}

std::int64_t FatReader::clusterPos(std::uint32_t cluster) const {
    return (firstDataSector_ + std::int64_t(cluster - 2) * secPerClus_) * bytesPerSec_;
}

bool FatReader::readClusterChain(std::uint32_t firstCluster, std::vector<std::uint8_t>* output) const {
    std::vector<std::uint8_t> out;
    const std::int64_t clusterSize = std::int64_t(secPerClus_) * bytesPerSec_;
    for (std::uint32_t c : chain(firstCluster)) {
        const std::size_t base = out.size();
        if (std::size_t(clusterSize) > out.max_size() - base) return false;
        out.resize(base + std::size_t(clusterSize));
        if (!disc_->readExactlyChecked(clusterPos(c), out.data() + base, int(clusterSize)))
            return false;
    }
    output->swap(out);
    return true;
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

std::vector<FatReader::DirEntry> FatReader::parseDirectory(
    const std::vector<std::uint8_t>& buf) const {
    std::vector<DirEntry> out;
    std::vector<std::uint16_t> lfn;
    int expectedSequence = 0;
    std::uint8_t lfnChecksum = 0;
    bool lfnActive = false;

    const auto resetLfn = [&]() {
        lfn.clear();
        expectedSequence = 0;
        lfnChecksum = 0;
        lfnActive = false;
    };
    const auto shortChecksum = [](const std::uint8_t* name) {
        std::uint8_t sum = 0;
        for (int i = 0; i < 11; ++i)
            sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
        return sum;
    };

    for (std::size_t off = 0; off + 32 <= buf.size(); off += 32) {
        const std::uint8_t first = buf[off];
        if (first == 0x00) break;
        const std::uint8_t attr = buf[off + 11];
        if (first == 0xE5) { resetLfn(); continue; }

        if ((attr & 0x0F) == 0x0F) {
            const int seq = first & 0x3F;
            const bool last = (first & 0x40) != 0;
            const std::uint8_t checksum = buf[off + 13];
            if (seq < 1 || seq > 20 || buf[off + 12] != 0 || le16(buf, off + 26) != 0) {
                resetLfn();
                continue;
            }
            if (last) {
                lfn.assign(static_cast<std::size_t>(seq) * 13, 0xFFFF);
                expectedSequence = seq;
                lfnChecksum = checksum;
                lfnActive = true;
            }
            if (!lfnActive || seq != expectedSequence || checksum != lfnChecksum) {
                resetLfn();
                continue;
            }
            const std::size_t pos = static_cast<std::size_t>(seq - 1) * 13;
            for (int k = 0; k < 5; ++k) lfn[pos + k] = le16(buf, off + 1 + k * 2);
            for (int k = 0; k < 6; ++k) lfn[pos + 5 + k] = le16(buf, off + 14 + k * 2);
            for (int k = 0; k < 2; ++k) lfn[pos + 11 + k] = le16(buf, off + 28 + k * 2);
            --expectedSequence;
            continue;
        }

        if (attr & 0x08) { resetLfn(); continue; }

        DirEntry e;
        if (lfnActive && expectedSequence == 0 &&
            shortChecksum(buf.data() + off) == lfnChecksum) {
            for (std::size_t i = 0; i < lfn.size(); ++i) {
                std::uint32_t c = lfn[i];
                if (c == 0 || c == 0xFFFF) break;
                if (c >= 0xD800 && c <= 0xDBFF) {
                    if (i + 1 < lfn.size() && lfn[i + 1] >= 0xDC00 && lfn[i + 1] <= 0xDFFF)
                        c = 0x10000 + ((c - 0xD800) << 10) + (lfn[++i] - 0xDC00);
                    else c = 0xFFFD;
                } else if (c >= 0xDC00 && c <= 0xDFFF) c = 0xFFFD;
                appendUtf8(e.name, c);
            }
        }
        resetLfn();
        if (e.name.empty()) {
            std::string base, ext;
            for (int k = 0; k < 8; ++k) {
                char c = static_cast<char>(buf[off + k]);
                if (c != ' ') base.push_back(c);
            }
            for (int k = 0; k < 3; ++k) {
                char c = static_cast<char>(buf[off + 8 + k]);
                if (c != ' ') ext.push_back(c);
            }
            if (!base.empty() && static_cast<std::uint8_t>(base[0]) == 0x05)
                base[0] = static_cast<char>(0xE5);
            e.name = ext.empty() ? base : base + "." + ext;
        }
        if (e.name == "." || e.name == "..") continue;

        e.isDirectory = (attr & 0x10) != 0;
        e.firstCluster = le16(buf, off + 26);
        if (type_ == FatType::Fat32)
            e.firstCluster |= static_cast<std::uint32_t>(le16(buf, off + 20)) << 16;
        e.size = le32(buf, off + 28);
        if (!filter_.empty() && (e.isDirectory || e.size) && e.firstCluster >= 2) {
            const std::uint32_t index = e.firstCluster - 2;
            if (index / 8 >= filter_.size() || !(filter_[index / 8] & (1U << (index % 8))))
                continue;
        }
        if (!e.name.empty()) out.push_back(std::move(e));
    }
    return out;
}

bool FatReader::directory(const std::string& dirPath, std::vector<DirEntry>* entries) const {
    // Resolve the directory's cluster (root is special for FAT12/16).
    if (dirPath.empty() || dirPath == "/" || dirPath == "\\") {
        if (type_ != FatType::Fat32) {
            std::vector<std::uint8_t> buf;
            if (!readBytes(*disc_, rootDirStart_, rootDirBytes_, &buf)) return false;
            *entries = parseDirectory(buf);
            return true;
        }
        std::vector<std::uint8_t> buf;
        if (!readClusterChain(rootClus_, &buf)) return false;
        *entries = parseDirectory(buf);
        return true;
    }
    DirEntry store;
    bool found = false;
    if (!find(dirPath, &store, &found)) return false;
    if (!found || !store.isDirectory) { entries->clear(); return true; }
    std::vector<std::uint8_t> buf;
    if (!readClusterChain(store.firstCluster, &buf)) return false;
    *entries = parseDirectory(buf);
    return true;
}

bool FatReader::find(const std::string& path, DirEntry* store, bool* foundEntry) const {
    *foundEntry = false;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return true;

    std::vector<DirEntry> dir;
    if (!directory(std::string(), &dir)) return false;
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
        if (!found) return true;
        if (i + 1 == parts.size()) { *store = *found; *foundEntry = true; return true; }
        if (!found->isDirectory) return true;
        std::vector<std::uint8_t> buf;
        if (!readClusterChain(found->firstCluster, &buf)) return false;
        dir = parseDirectory(buf);
    }
    return true;
}

std::vector<DiscEntry> FatReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    std::vector<ByteStorePtr> contents;
    listWithContent(dirPath, &out, &contents);
    return out;
}

bool FatReader::listWithContent(const std::string& dirPath, std::vector<DiscEntry>* entries,
                                std::vector<ByteStorePtr>* contents) const {
    if (!valid_ || !entries || !contents) return false;
    std::vector<DirEntry> dir;
    if (!directory(dirPath, &dir)) return false;
    std::vector<DiscEntry> out;
    std::vector<ByteStorePtr> stores;
    for (const DirEntry& e : dir) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.size;
        out.push_back(d);
        stores.push_back(e.isDirectory ? ByteStorePtr() : clusterContent(e.firstCluster, e.size));
    }
    entries->swap(out);
    contents->swap(stores);
    return true;
}

ByteStorePtr FatReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    DirEntry store;
    bool found = false;
    if (!find(path, &store, &found) || !found || store.isDirectory) return nullptr;
    return clusterContent(store.firstCluster, store.size);
}

}  // namespace fs
}  // namespace peare
