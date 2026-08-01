#include "Iso9660Reader.h"

#include <algorithm>
#include <cstring>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSectorSize = 2048;

std::uint16_t u16le(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t u32le(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

// A directory record's both-endian scalars: read the little-endian half.
std::uint32_t bothU32(const std::uint8_t* p) { return u32le(p); }

// Trailing ";<digits>" version suffix is stripped from file names (the DiscUtils
// HideVersions behaviour); a lone leading/trailing dot is left untouched.
std::string stripVersion(std::string name) {
    const std::size_t sc = name.rfind(';');
    if (sc != std::string::npos && sc + 1 < name.size()) {
        bool digits = true;
        for (std::size_t i = sc + 1; i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9') { digits = false; break; }
        if (digits) name.erase(sc);
    }
    return name;
}

std::string decodeAscii(const std::uint8_t* p, int len) {
    std::string s(reinterpret_cast<const char*>(p), std::size_t(len));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// Joliet identifiers are UTF-16 big-endian; len is the byte count.
std::string decodeJoliet(const std::uint8_t* p, int len) {
    std::string out;
    int i = 0;
    while (i + 1 < len) {
        std::uint32_t u = (std::uint32_t(p[i]) << 8) | p[i + 1];
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
            const std::uint32_t lo = (std::uint32_t(p[i]) << 8) | p[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        appendUtf8(out, u);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

bool Iso9660Reader::detect(const IByteStore& disc) {
    // The first volume descriptor sits at sector 16 and carries "CD001" at
    // offset 1.
    std::uint8_t buf[8];
    if (disc.read(16 * kSectorSize + 1, buf, 5) != 5) return false;
    return std::memcmp(buf, "CD001", 5) == 0;
}

Iso9660Reader::Iso9660Reader(ByteStorePtr disc) : disc_(std::move(disc)) {
    parse();
}

void Iso9660Reader::parse() {
    if (!disc_) { error_ = "null disc"; return; }

    bool haveVd = false;
    bool joliet = false;
    std::uint32_t rootExtent = 0, rootLength = 0;
    std::string volumeId;

    std::vector<std::uint8_t> sector(static_cast<std::size_t>(kSectorSize));
    for (int i = 0; i < 64; ++i) {  // safety bound on descriptor scan
        const std::int64_t pos = (16 + i) * kSectorSize;
        if (disc_->read(pos, sector.data(), int(kSectorSize)) != int(kSectorSize)) break;
        if (std::memcmp(sector.data() + 1, "CD001", 5) != 0) break;
        const std::uint8_t type = sector[0];
        if (type == 255) break;  // set terminator

        const std::uint8_t* rec = sector.data() + 156;  // root directory record
        if (type == 1) {  // primary
            if (!joliet) {
                rootExtent = bothU32(rec + 2);
                rootLength = bothU32(rec + 10);
                volumeId = decodeAscii(sector.data() + 40, 32);
            }
            haveVd = true;
        } else if (type == 2) {  // supplementary — Joliet if UTF-16BE escape
            const std::uint8_t* esc = sector.data() + 88;
            const bool isJoliet = esc[0] == 0x25 && esc[1] == 0x2F &&
                                  (esc[2] == 0x40 || esc[2] == 0x43 || esc[2] == 0x45);
            if (isJoliet) {  // prefer Joliet names when available
                joliet = true;
                rootExtent = bothU32(rec + 2);
                rootLength = bothU32(rec + 10);
                volumeId = decodeJoliet(sector.data() + 40, 32);
                haveVd = true;
            }
        }
    }

    if (!haveVd) { error_ = "no ISO 9660 volume descriptor"; return; }

    root_.isDir = true;
    root_.extent = rootExtent;
    root_.length = rootLength;
    readDirectory(root_, joliet, 0);

    friendly_ = joliet ? "ISO 9660 (Joliet)" : "ISO 9660";
    if (!volumeId.empty()) friendly_ += " \"" + volumeId + "\"";
    valid_ = true;
}

void Iso9660Reader::readDirectory(Node& dir, bool joliet, int depth) {
    if (depth > 64 || dir.length == 0) return;
    const std::vector<std::uint8_t> buf =
        disc_->readRange(std::int64_t(dir.extent) * kSectorSize, dir.length);
    std::size_t p = 0;
    while (p < buf.size()) {
        const int len = buf[p];
        if (len == 0) {
            // Records never straddle a sector boundary; skip the tail padding.
            p = ((p / std::size_t(kSectorSize)) + 1) * std::size_t(kSectorSize);
            continue;
        }
        if (p + std::size_t(len) > buf.size() || len < 33) break;
        const std::uint8_t* r = buf.data() + p;
        const std::uint32_t extent = bothU32(r + 2);
        const std::uint32_t dataLen = bothU32(r + 10);
        const std::uint8_t flags = r[25];
        const int lenId = r[32];

        const bool special = lenId == 1 && (r[33] == 0 || r[33] == 1);
        if (!special && p + 33 + std::size_t(lenId) <= buf.size()) {
            Node child;
            child.isDir = (flags & 0x02) != 0;
            child.extent = extent;
            child.length = dataLen;
            std::string name = joliet ? decodeJoliet(r + 33, lenId)
                                      : decodeAscii(r + 33, lenId);
            if (!child.isDir) name = stripVersion(name);
            child.name = name;
            // Recurse into subdirectories (guarding against self-reference).
            if (child.isDir && child.extent != dir.extent)
                readDirectory(child, joliet, depth + 1);
            dir.children.push_back(std::move(child));
        }
        p += std::size_t(len);
    }
}

const Iso9660Reader::Node* Iso9660Reader::find(const std::string& path) const {
    const Node* cur = &root_;
    for (const std::string& part : splitPath(path)) {
        const Node* next = nullptr;
        for (const Node& c : cur->children) {
            if (c.name == part) { next = &c; break; }
        }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

std::vector<DiscEntry> Iso9660Reader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    const Node* dir = find(dirPath);
    if (!dir || !dir->isDir) return out;
    out.reserve(dir->children.size());
    for (const Node& c : dir->children) {
        DiscEntry e;
        e.name = c.name;
        e.isDirectory = c.isDir;
        e.length = c.isDir ? 0 : std::int64_t(c.length);
        out.push_back(std::move(e));
    }
    return out;
}

ByteStorePtr Iso9660Reader::openFile(const std::string& path) const {
    const Node* node = find(path);
    if (!node || node->isDir) return ByteStorePtr();
    // The file content is a contiguous window over the disc (ExtentStream role).
    return std::make_shared<SubStore>(disc_, std::int64_t(node->extent) * kSectorSize,
                                      std::int64_t(node->length));
}

}  // namespace fs
}  // namespace peare
