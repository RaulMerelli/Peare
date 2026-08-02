#include "ExFatReader.h"

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
std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

void appendUtf8(std::string& out, unsigned int ch) {
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
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

// A cluster value >= 0xFFFFFFF8 marks end-of-chain; < 2 is free/invalid.
bool isEndCluster(std::uint32_t c) { return c < 2 || c >= 0xFFFFFFF8u; }

}  // namespace

ExFatReader::ExFatReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

void ExFatReader::parse() {
    std::vector<std::uint8_t> bs = disc_->readRange(0, 512);
    if (bs.size() < 512) { error_ = "Truncated exFAT boot sector"; return; }
    if (std::memcmp(bs.data() + 3, "EXFAT   ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) {
        error_ = "Not an exFAT volume";
        return;
    }
    fatOffsetSector_ = le32(bs.data() + 80);
    clusterOffsetSector_ = le32(bs.data() + 88);
    clusterCount_ = le32(bs.data() + 92);
    rootDirCluster_ = le32(bs.data() + 96);
    const std::uint8_t bpsShift = bs[108];
    const std::uint8_t spcShift = bs[109];
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25) {
        error_ = "Invalid exFAT geometry";
        return;
    }
    bytesPerSector_ = 1u << bpsShift;
    sectorsPerCluster_ = 1u << spcShift;
    bytesPerCluster_ = bytesPerSector_ * sectorsPerCluster_;
    fatByteOffset_ = static_cast<std::int64_t>(fatOffsetSector_) * bytesPerSector_;
    if (rootDirCluster_ < 2) { error_ = "Invalid exFAT root cluster"; return; }
    valid_ = true;
}

std::uint32_t ExFatReader::nextCluster(std::uint32_t cluster) const {
    std::vector<std::uint8_t> e =
        disc_->readRange(fatByteOffset_ + static_cast<std::int64_t>(cluster) * 4, 4);
    if (e.size() < 4) return 0xFFFFFFFFu;
    return le32(e.data());
}

std::int64_t ExFatReader::clusterOffset(std::uint32_t cluster) const {
    return (static_cast<std::int64_t>(clusterOffsetSector_) +
            static_cast<std::int64_t>(cluster - 2) * sectorsPerCluster_) *
           bytesPerSector_;
}

std::vector<std::uint32_t> ExFatReader::clusterChain(const DataDesc& desc) const {
    std::vector<std::uint32_t> out;
    std::uint32_t cluster = desc.firstCluster;
    // Bound the walk to the total cluster count to defend against corrupt chains.
    const std::size_t cap = clusterCount_ ? clusterCount_ + 2 : 1u << 24;
    std::uint64_t covered = 0;
    while (!isEndCluster(cluster) && out.size() < cap) {
        out.push_back(cluster);
        covered += bytesPerCluster_;
        if (desc.length != 0 && covered >= desc.length) break;
        cluster = desc.contiguous ? cluster + 1 : nextCluster(cluster);
    }
    return out;
}

ByteStorePtr ExFatReader::buildContent(const DataDesc& desc, std::uint64_t logicalLen) const {
    const std::vector<std::uint32_t> clusters = clusterChain(desc);

    // Coalesce contiguous clusters into runs, one SubStore each.
    std::vector<ByteStorePtr> parts;
    std::size_t i = 0;
    while (i < clusters.size()) {
        const std::uint32_t start = clusters[i];
        std::size_t run = 1;
        while (i + run < clusters.size() && clusters[i + run] == start + run) ++run;
        const std::int64_t off = clusterOffset(start);
        const std::int64_t len = static_cast<std::int64_t>(run) * bytesPerCluster_;
        parts.push_back(std::make_shared<SubStore>(disc_, off, len));
        i += run;
    }

    ByteStorePtr concat = std::make_shared<ConcatStore>(std::move(parts));
    std::int64_t n = concat->capacity();
    if (logicalLen != 0)
        n = std::min<std::int64_t>(n, static_cast<std::int64_t>(logicalLen));
    return std::make_shared<SubStore>(concat, 0, n);
}

std::vector<ExFatReader::DirRec> ExFatReader::parseDirectory(const DataDesc& dir) const {
    std::vector<DirRec> out;
    // The directory's raw bytes span its whole cluster chain.
    std::vector<std::uint8_t> content = buildContent(dir, 0)->readAll();

    std::size_t pos = 0;
    while (pos + 32 <= content.size()) {
        const std::uint8_t* e = content.data() + pos;
        const std::uint8_t type = e[0];
        if (type == 0x00) break;               // end of directory
        if (type != 0x85) { pos += 32; continue; }  // not an in-use File entry

        const std::uint8_t secondaryCount = e[1];
        const std::uint16_t attrs = le16(e + 4);
        const bool isDir = (attrs & 0x10) != 0;

        // Secondary entries follow: first the Stream extension (0xC0), then the
        // FileName extensions (0xC1).
        DataDesc data;
        std::uint64_t validLen = 0;
        std::uint8_t nameLen = 0;
        std::string name;
        int gathered = 0;
        std::size_t sp = pos + 32;
        for (int s = 0; s < secondaryCount && sp + 32 <= content.size(); ++s, sp += 32) {
            const std::uint8_t* se = content.data() + sp;
            if (se[0] == 0xC0) {               // Stream extension
                const std::uint8_t flags = se[1];
                nameLen = se[3];
                validLen = le64(se + 8);
                data.firstCluster = le32(se + 20);
                data.length = le64(se + 24);
                data.contiguous = (flags & 0x02) != 0;  // NoFatChain
            } else if (se[0] == 0xC1) {        // FileName extension (15 UTF-16 chars)
                for (int k = 0; k < 15 && gathered < nameLen; ++k, ++gathered)
                    appendUtf8(name, le16(se + 2 + k * 2));
            }
        }
        pos = sp;

        if (!name.empty()) {
            DirRec rec;
            rec.name = name;
            rec.isDirectory = isDir;
            rec.data = data;
            rec.logicalLen = validLen;
            out.push_back(rec);
        }
    }
    return out;
}

std::vector<ExFatReader::DirRec> ExFatReader::directory(const std::string& path) const {
    DataDesc dir;
    dir.firstCluster = rootDirCluster_;
    dir.contiguous = false;   // the root directory is never contiguous
    dir.length = 0;

    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& comp : parts) {
        std::vector<DirRec> recs = parseDirectory(dir);
        const std::string want = toLower(comp);
        bool matched = false;
        for (const DirRec& r : recs) {
            if (r.isDirectory && toLower(r.name) == want) {
                dir = r.data;
                matched = true;
                break;
            }
        }
        if (!matched) return {};
    }
    return parseDirectory(dir);
}

bool ExFatReader::find(const std::string& path, DirRec* out) const {
    std::vector<std::string> parts;
    splitPath(path, &parts);
    if (parts.empty()) return false;
    const std::string leaf = parts.back();
    parts.pop_back();
    std::string parent;
    for (const std::string& p : parts) { parent += '/'; parent += p; }

    const std::vector<DirRec> recs = directory(parent);
    const std::string want = toLower(leaf);
    for (const DirRec& r : recs) {
        if (toLower(r.name) == want) { *out = r; return true; }
    }
    return false;
}

std::vector<DiscEntry> ExFatReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    for (const DirRec& r : directory(dirPath)) {
        DiscEntry e;
        e.name = r.name;
        e.isDirectory = r.isDirectory;
        e.length = r.isDirectory ? 0 : static_cast<std::int64_t>(r.logicalLen);
        out.push_back(e);
    }
    return out;
}

ByteStorePtr ExFatReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    DirRec rec;
    if (!find(path, &rec) || rec.isDirectory) return nullptr;
    if (rec.data.firstCluster < 2 || rec.logicalLen == 0)
        return std::make_shared<MemoryStore>(std::vector<std::uint8_t>{});
    return buildContent(rec.data, rec.logicalLen);
}

}  // namespace fs
}  // namespace peare
