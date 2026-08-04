#include "HpfsReader.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kSectorSize = 512;
const std::uint32_t kBootMagic = 0xAA55U;
const std::uint32_t kSuperMagic = 0xF995E849U;
const std::uint32_t kSuperMagic2 = 0xFA53E9C5U;
const std::uint32_t kSpareMagic = 0xF9911849U;
const std::uint32_t kSpareMagic2 = 0xFA5229C5U;
const std::uint32_t kFnodeMagic = 0xF7E40AAEU;
const std::uint32_t kAnodeMagic = 0x37E40AAEU;
const std::uint32_t kDnodeMagic = 0x77E40AAEU;
const std::uint32_t kCodePageDirectoryMagic = 0x494521F7U;
const std::uint8_t kBplusInternal = 0x80U;
const std::uint16_t kFnodeDirectory = 0x0100U;
const std::uint8_t kDirFirst = 0x01U;
const std::uint8_t kDirDown = 0x04U;
const std::uint8_t kDirLast = 0x08U;
const std::uint8_t kAttrDirectory = 0x10U;

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

void splitPath(const std::string& path, std::vector<std::string>* parts) {
    std::string current;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                parts->push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts->push_back(current);
}

unsigned char asciiFold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

bool nameEqual(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiFold(static_cast<unsigned char>(a[i])) !=
            asciiFold(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string trimLabel(const std::uint8_t* p, std::size_t n) {
    std::size_t end = n;
    while (end && (p[end - 1] == 0 || p[end - 1] == ' ')) --end;
    std::size_t begin = 0;
    while (begin < end && p[begin] == ' ') ++begin;
    std::string out(reinterpret_cast<const char*>(p + begin), end - begin);
    for (std::size_t i = 0; i < out.size(); ++i)
        if (static_cast<unsigned char>(out[i]) < 0x20) out[i] = '_';
    return out;
}

const std::uint16_t kCp437[128] = {
0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0};

const std::uint16_t kCp850[128] = {
0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00F8,0x00A3,0x00D8,0x00D7,0x0192,
0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x00AE,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
0x2591,0x2592,0x2593,0x2502,0x2524,0x00C1,0x00C2,0x00C0,0x00A9,0x2563,0x2551,0x2557,0x255D,0x00A2,0x00A5,0x2510,
0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x00E3,0x00C3,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x00A4,
0x00F0,0x00D0,0x00CA,0x00CB,0x00C8,0x0131,0x00CD,0x00CE,0x00CF,0x2518,0x250C,0x2588,0x2584,0x00A6,0x00CC,0x2580,
0x00D3,0x00DF,0x00D4,0x00D2,0x00F5,0x00D5,0x00B5,0x00FE,0x00DE,0x00DA,0x00DB,0x00D9,0x00FD,0x00DD,0x00AF,0x00B4,
0x00AD,0x00B1,0x2017,0x00BE,0x00B6,0x00A7,0x00F7,0x00B8,0x00B0,0x00A8,0x00B7,0x00B9,0x00B3,0x00B2,0x25A0,0x00A0};

const std::uint16_t kCp852[128] = {
0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x016F,0x0107,0x00E7,0x0142,0x00EB,0x0150,0x0151,0x00EE,0x0179,0x00C4,0x0106,
0x00C9,0x0139,0x013A,0x00F4,0x00F6,0x013D,0x013E,0x015A,0x015B,0x00D6,0x00DC,0x0164,0x0165,0x0141,0x00D7,0x010D,
0x00E1,0x00ED,0x00F3,0x00FA,0x0104,0x0105,0x017D,0x017E,0x0118,0x0119,0x00AC,0x017A,0x010C,0x015F,0x00AB,0x00BB,
0x2591,0x2592,0x2593,0x2502,0x2524,0x00C1,0x00C2,0x011A,0x015E,0x2563,0x2551,0x2557,0x255D,0x017B,0x017C,0x2510,
0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x0102,0x0103,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x00A4,
0x0111,0x0110,0x010E,0x00CB,0x010F,0x0147,0x00CD,0x00CE,0x011B,0x2518,0x250C,0x2588,0x2584,0x0162,0x016E,0x2580,
0x00D3,0x00DF,0x00D4,0x0143,0x0144,0x0148,0x0160,0x0161,0x0154,0x00DA,0x0155,0x0170,0x00FD,0x00DD,0x0163,0x00B4,
0x00AD,0x02DD,0x02DB,0x02C7,0x02D8,0x00A7,0x00F7,0x00B8,0x00B0,0x00A8,0x02D9,0x0171,0x0158,0x0159,0x25A0,0x00A0};

const std::uint16_t kCp866[128] = {
0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
0x0401,0x0451,0x0404,0x0454,0x0407,0x0457,0x040E,0x045E,0x00B0,0x2219,0x00B7,0x221A,0x2116,0x00A4,0x25A0,0x00A0};

const std::uint16_t* tableForCodePage(std::uint16_t cp) {
    switch (cp) {
    case 437: return kCp437;
    case 852: return kCp852;
    case 866: return kCp866;
    case 850:
    default: return kCp850;
    }
}

class HpfsExtentStore final : public IByteStore {
public:
    HpfsExtentStore(ByteStorePtr parent, std::int64_t length,
                    std::vector<HpfsReader::Extent> extents,
                    std::map<std::uint32_t, std::uint32_t> hotfixes)
        : parent_(std::move(parent)), length_(length), extents_(std::move(extents)),
          hotfixes_(std::move(hotfixes)) {}

    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!parent_ || !dst || pos < 0 || count <= 0 || pos >= length_) return 0;
        const std::int64_t available = length_ - pos;
        const int wanted = count < available ? count : static_cast<int>(available);
        int done = 0;
        while (done < wanted) {
            const std::uint64_t filePos = static_cast<std::uint64_t>(pos + done);
            const std::uint32_t logical = static_cast<std::uint32_t>(filePos / kSectorSize);
            const std::uint32_t within = static_cast<std::uint32_t>(filePos % kSectorSize);
            const int chunk = std::min<int>(wanted - done, kSectorSize - within);
            const HpfsReader::Extent* found = nullptr;
            for (std::size_t i = 0; i < extents_.size(); ++i) {
                const HpfsReader::Extent& e = extents_[i];
                if (logical >= e.logicalSector &&
                    logical < e.logicalSector + e.sectorCount) {
                    found = &e;
                    break;
                }
            }
            if (!found) {
                std::fill(dst + done, dst + done + chunk, std::uint8_t(0));
            } else {
                std::uint32_t physical = found->physicalSector +
                    (logical - found->logicalSector);
                const std::map<std::uint32_t, std::uint32_t>::const_iterator hot =
                    hotfixes_.find(physical);
                if (hot != hotfixes_.end()) physical = hot->second;
                const std::int64_t source = static_cast<std::int64_t>(physical) *
                    kSectorSize + within;
                const int got = parent_->read(source, dst + done, chunk);
                if (got != chunk) {
                    if (got > 0) std::fill(dst + done + got, dst + done + chunk, std::uint8_t(0));
                    else std::fill(dst + done, dst + done + chunk, std::uint8_t(0));
                }
            }
            done += chunk;
        }
        return done;
    }

private:
    ByteStorePtr parent_;
    std::int64_t length_;
    std::vector<HpfsReader::Extent> extents_;
    std::map<std::uint32_t, std::uint32_t> hotfixes_;
};

}  // namespace

HpfsReader::HpfsReader(ByteStorePtr disc) : disc_(std::move(disc)) { parse(); }

void HpfsReader::parse() {
    if (!disc_ || disc_->capacity() < 18LL * kSectorSize) {
        error_ = "Truncated HPFS volume";
        return;
    }
    const std::vector<std::uint8_t> boot = disc_->readRange(0, kSectorSize);
    const bool standardBoot = boot.size() == kSectorSize &&
        le16(boot.data() + 510) == kBootMagic &&
        le16(boot.data() + 11) == kSectorSize &&
        std::memcmp(boot.data() + 54, "HPFS    ", 8) == 0;

    const std::vector<std::uint8_t> super = disc_->readRange(16LL * kSectorSize,
                                                             kSectorSize);
    if (super.size() != kSectorSize || le32(super.data()) != kSuperMagic ||
        le32(super.data() + 4) != kSuperMagic2) {
        error_ = "Invalid HPFS superblock";
        return;
    }
    rootFnode_ = le32(super.data() + 12);
    sectorCount_ = le32(super.data() + 16);
    const std::uint64_t availableSectors =
        static_cast<std::uint64_t>(disc_->capacity()) / kSectorSize;
    if (rootFnode_ < 20 || rootFnode_ >= availableSectors || sectorCount_ < 20 ||
        sectorCount_ > availableSectors) {
        error_ = "HPFS superblock contains invalid sector addresses";
        return;
    }

    const std::vector<std::uint8_t> spare = disc_->readRange(17LL * kSectorSize,
                                                             kSectorSize);
    if (spare.size() == kSectorSize && le32(spare.data()) == kSpareMagic &&
        le32(spare.data() + 4) == kSpareMagic2) {
        loadHotfixMap(spare);
        loadCodePages(spare);
    }

    const FnodeInfo root = readFnode(rootFnode_);
    if (!root.valid || !directoryRootDnode(root, true)) {
        error_ = "Invalid HPFS root directory fnode";
        return;
    }

    const std::string label = standardBoot ? trimLabel(boot.data() + 43, 11)
                                           : std::string();
    friendly_ = "HPFS";
    valid_ = true;
}

bool HpfsReader::loadHotfixMap(const std::vector<std::uint8_t>& spare) {
    if (spare.size() < 24) return false;
    const std::uint32_t mapSector = le32(spare.data() + 12);
    const std::uint32_t used = le32(spare.data() + 16);
    const std::uint32_t total = le32(spare.data() + 20);
    if (!mapSector || total == 0) return true;
    if (used > total || total > 256 || mapSector >= sectorCount_ ||
        mapSector + 4 > sectorCount_) return false;
    const std::vector<std::uint8_t> map = disc_->readRange(
        static_cast<std::int64_t>(mapSector) * kSectorSize, 4 * kSectorSize);
    if (map.size() != 4 * kSectorSize || total * 8U > map.size()) return false;
    for (std::uint32_t i = 0; i < used; ++i) {
        const std::uint32_t from = le32(map.data() + i * 4);
        const std::uint32_t to = le32(map.data() + (total + i) * 4);
        if (from && from < sectorCount_ && to && to < sectorCount_)
            hotfixes_[from] = to;
    }
    return true;
}

void HpfsReader::loadCodePages(const std::vector<std::uint8_t>& spare) {
    if (spare.size() < 40) return;
    const std::uint32_t directorySector = le32(spare.data() + 32);
    const std::uint32_t declared = le32(spare.data() + 36);
    if (!directorySector || directorySector >= sectorCount_) return;
    const std::vector<std::uint8_t> cp = readSectors(directorySector, 1);
    if (cp.size() != kSectorSize || le32(cp.data()) != kCodePageDirectoryMagic) return;
    const std::uint32_t count = std::min<std::uint32_t>(
        std::min<std::uint32_t>(le32(cp.data() + 4), declared ? declared : 31), 31);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off = 16 + static_cast<std::size_t>(i) * 16;
        if (off + 16 > cp.size()) break;
        const std::uint16_t index = le16(cp.data() + off);
        const std::uint16_t number = le16(cp.data() + off + 2);
        if (index <= 255 && number) {
            codePages_[static_cast<std::uint8_t>(index)] = number;
            if (i == 0) defaultCodePage_ = number;
        }
    }
}

std::uint32_t HpfsReader::remapSector(std::uint32_t sector) const {
    const std::map<std::uint32_t, std::uint32_t>::const_iterator it =
        hotfixes_.find(sector);
    return it == hotfixes_.end() ? sector : it->second;
}

int HpfsReader::readMapped(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (!disc_ || !dst || pos < 0 || count <= 0 || pos >= disc_->capacity()) return 0;
    const std::int64_t available = disc_->capacity() - pos;
    const int wanted = count < available ? count : static_cast<int>(available);
    int done = 0;
    while (done < wanted) {
        const std::int64_t absolute = pos + done;
        const std::uint32_t sector = static_cast<std::uint32_t>(absolute / kSectorSize);
        const std::uint32_t within = static_cast<std::uint32_t>(absolute % kSectorSize);
        const int chunk = std::min<int>(wanted - done, kSectorSize - within);
        const std::uint32_t physical = remapSector(sector);
        const int got = disc_->read(static_cast<std::int64_t>(physical) * kSectorSize + within,
                                    dst + done, chunk);
        if (got <= 0) break;
        done += got;
        if (got != chunk) break;
    }
    return done;
}

std::vector<std::uint8_t> HpfsReader::readSectors(std::uint32_t sector,
                                                  std::uint32_t count) const {
    std::vector<std::uint8_t> out;
    if (!count || sector >= sectorCount_ || count > sectorCount_ - sector) return out;
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * kSectorSize;
    if (bytes > static_cast<std::uint64_t>(INT_MAX)) return out;
    out.resize(static_cast<std::size_t>(bytes));
    if (readMapped(static_cast<std::int64_t>(sector) * kSectorSize, out.data(),
                   static_cast<int>(bytes)) != static_cast<int>(bytes))
        out.clear();
    return out;
}

HpfsReader::FnodeInfo HpfsReader::readFnode(std::uint32_t sector) const {
    FnodeInfo out;
    if (!sector || sector >= sectorCount_) return out;
    out.raw = readSectors(sector, 1);
    if (out.raw.size() != kSectorSize || le32(out.raw.data()) != kFnodeMagic)
        return FnodeInfo();
    out.sector = sector;
    out.directory = (le16(out.raw.data() + 54) & kFnodeDirectory) != 0;
    out.fileSize = le32(out.raw.data() + 160);
    out.valid = true;
    return out;
}

void HpfsReader::parseBplusNode(const std::vector<std::uint8_t>& raw,
                                std::size_t headerOffset, std::size_t entriesOffset,
                                std::size_t leafCapacity, std::size_t internalCapacity,
                                std::vector<Extent>* extents,
                                std::set<std::uint32_t>* visitedAnodes,
                                int depth) const {
    if (!extents || !visitedAnodes || depth > 32 || headerOffset + 8 > raw.size() ||
        entriesOffset > raw.size()) return;
    const std::uint8_t flags = raw[headerOffset];
    const std::uint8_t used = raw[headerOffset + 5];
    const bool internal = (flags & kBplusInternal) != 0;
    const std::size_t capacity = internal ? internalCapacity : leafCapacity;
    const std::size_t itemSize = internal ? 8 : 12;
    if (used > capacity || entriesOffset + static_cast<std::size_t>(used) * itemSize > raw.size())
        return;

    if (!internal) {
        for (std::uint8_t i = 0; i < used; ++i) {
            const std::size_t off = entriesOffset + static_cast<std::size_t>(i) * 12;
            Extent e;
            e.logicalSector = le32(raw.data() + off);
            e.sectorCount = le32(raw.data() + off + 4);
            e.physicalSector = le32(raw.data() + off + 8);
            if (!e.sectorCount || !e.physicalSector || e.physicalSector >= sectorCount_ ||
                e.sectorCount > sectorCount_ - e.physicalSector)
                continue;
            extents->push_back(e);
        }
        return;
    }

    for (std::uint8_t i = 0; i < used; ++i) {
        const std::size_t off = entriesOffset + static_cast<std::size_t>(i) * 8;
        const std::uint32_t down = le32(raw.data() + off + 4);
        if (!down || down >= sectorCount_ || !visitedAnodes->insert(down).second) continue;
        const std::vector<std::uint8_t> anode = readSectors(down, 1);
        if (anode.size() != kSectorSize || le32(anode.data()) != kAnodeMagic ||
            le32(anode.data() + 4) != down)
            continue;
        parseBplusNode(anode, 12, 20, 40, 60, extents, visitedAnodes, depth + 1);
    }
}

std::vector<HpfsReader::Extent> HpfsReader::fnodeExtents(const FnodeInfo& fnode) const {
    std::vector<Extent> extents;
    if (!fnode.valid || fnode.raw.size() != kSectorSize) return extents;
    std::set<std::uint32_t> visited;
    parseBplusNode(fnode.raw, 56, 64, 8, 12, &extents, &visited, 0);
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) {
        if (a.logicalSector != b.logicalSector) return a.logicalSector < b.logicalSector;
        return a.physicalSector < b.physicalSector;
    });
    std::vector<Extent> clean;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const Extent& e = extents[i];
        if (!clean.empty()) {
            Extent& back = clean.back();
            const std::uint64_t logicalEnd = static_cast<std::uint64_t>(back.logicalSector) +
                back.sectorCount;
            if (e.logicalSector < logicalEnd) continue;
            if (e.logicalSector == logicalEnd &&
                e.physicalSector == back.physicalSector + back.sectorCount) {
                back.sectorCount += e.sectorCount;
                continue;
            }
        }
        clean.push_back(e);
    }
    return clean;
}

ByteStorePtr HpfsReader::fileContent(const FnodeInfo& fnode, std::uint32_t length) const {
    if (!fnode.valid || fnode.directory) return ByteStorePtr();
    const std::uint32_t effective = length ? length : fnode.fileSize;
    if (!effective) return std::make_shared<ZeroStore>(0);
    const std::vector<Extent> extents = fnodeExtents(fnode);
    if (extents.empty()) return ByteStorePtr();
    return std::make_shared<HpfsExtentStore>(disc_, effective, extents, hotfixes_);
}

std::uint32_t HpfsReader::directoryRootDnode(const FnodeInfo& fnode,
                                                bool knownDirectory) const {
    if (!fnode.valid || (!knownDirectory && !fnode.directory) ||
        fnode.raw.size() != kSectorSize) return 0;

    // HPFS directories use the first external fnode allocation entry as a
    // direct pointer to the root dnode. Linux does the same and does not require
    // file_secno==0; treating it as a generic file extent rejected valid OS/2
    // directory fnodes produced by some installers/optimizers.
    // A directory fnode's first external allocation record is the dnode
    // pointer.  OS/2 and the Linux driver read it directly; historical/repair
    // tools do not always keep the generic B+ header counters consistent, so
    // do not gate this field on n_used_nodes or BP_internal.  Validate the
    // target itself instead.
    const std::uint32_t direct = le32(fnode.raw.data() + 72);
    if (direct >= 18 && direct + 4 <= sectorCount_) {
        const std::vector<std::uint8_t> dnode = readSectors(direct, 4);
        if (dnode.size() == 4 * kSectorSize && le32(dnode.data()) == kDnodeMagic)
            return direct;
    }

    // Defensive fallback for unusual internal trees.
    const std::vector<Extent> extents = fnodeExtents(fnode);
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const Extent& e = extents[i];
        if (e.physicalSector >= 18 && e.physicalSector + 4 <= sectorCount_)
            return e.physicalSector;
    }
    return 0;
}

std::uint16_t HpfsReader::codePageForIndex(std::uint8_t index) const {
    const std::map<std::uint8_t, std::uint16_t>::const_iterator it = codePages_.find(index);
    return it == codePages_.end() ? defaultCodePage_ : it->second;
}

std::string HpfsReader::decodeName(const std::uint8_t* bytes, std::size_t length,
                                   std::uint8_t codePageIndex) const {
    std::string out;
    const std::uint16_t* table = tableForCodePage(codePageForIndex(codePageIndex));
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t c = bytes[i];
        if (c == 0) break;
        appendUtf8(c < 0x80 ? c : table[c - 0x80], &out);
    }
    return out;
}

bool HpfsReader::walkDnode(std::uint32_t dnodeSector,
                           std::vector<EntryInfo>* entries,
                           std::set<std::uint32_t>* visited, int depth) const {
    if (!entries || !visited || depth > 64 || !dnodeSector ||
        dnodeSector >= sectorCount_ || !visited->insert(dnodeSector).second)
        return false;
    const std::vector<std::uint8_t> dnode = readSectors(dnodeSector, 4);
    if (dnode.size() != 4 * kSectorSize || le32(dnode.data()) != kDnodeMagic)
        return false;
    const std::uint32_t self = le32(dnode.data() + 16);
    const std::uint32_t firstFree = le32(dnode.data() + 4);
    // CHKDSK and hotfix recovery can leave a stale self field.  The Linux
    // reader reports it but continues; the sector reached through the fnode or
    // a verified down pointer remains the authoritative location.
    (void)self;
    if (firstFree < 20 || firstFree > dnode.size()) return false;

    std::size_t off = 20;
    while (off + 32 <= firstFree) {
        const std::uint16_t length = le16(dnode.data() + off);
        if (length < 32 || length > 292 || (length & 3U) != 0 ||
            off + length > firstFree)
            break;
        const std::uint8_t flags = dnode[off + 2];
        const std::uint8_t attributes = dnode[off + 3];
        const std::uint8_t nameLength = dnode[off + 30];
        if (31U + nameLength > length) break;

        if (flags & kDirDown) {
            if (length < 36) break;
            const std::uint32_t down = le32(dnode.data() + off + length - 4);
            if (down) walkDnode(down, entries, visited, depth + 1);
        }

        if (!(flags & (kDirFirst | kDirLast))) {
            EntryInfo entry;
            entry.fnodeSector = le32(dnode.data() + off + 4);
            entry.fileSize = le32(dnode.data() + off + 12);
            entry.attributes = attributes;
            entry.directory = (attributes & kAttrDirectory) != 0;
            entry.name = decodeName(dnode.data() + off + 31, nameLength,
                                    dnode[off + 29]);
            if (!entry.name.empty() && entry.fnodeSector &&
                entry.fnodeSector < sectorCount_) {
                const FnodeInfo child = readFnode(entry.fnodeSector);
                if (child.valid) {
                    if (child.directory) entry.directory = true;
                    if (!entry.fileSize && child.fileSize) entry.fileSize = child.fileSize;
                }
                bool duplicate = false;
                for (std::size_t i = 0; i < entries->size(); ++i) {
                    if ((*entries)[i].fnodeSector == entry.fnodeSector &&
                        (*entries)[i].name == entry.name) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) entries->push_back(entry);
            }
        }
        off += length;
    }
    return off == firstFree || !entries->empty();
}

std::vector<HpfsReader::EntryInfo> HpfsReader::readDirectory(
    std::uint32_t fnodeSector, bool knownDirectory) const {
    std::vector<EntryInfo> entries;
    const FnodeInfo fnode = readFnode(fnodeSector);
    const std::uint32_t root = directoryRootDnode(fnode, knownDirectory);
    if (!root) return entries;
    std::set<std::uint32_t> visited;
    // Keep every verified entry even when another branch is damaged.  HPFS
    // directory entries are independent and a read-only browser must not hide
    // an otherwise valid subtree because of one bad trailing record.
    walkDnode(root, &entries, &visited, 0);
    return entries;
}

bool HpfsReader::resolvePath(const std::string& path, EntryInfo* result,
                             FnodeInfo* fnode) const {
    if (!valid_) return false;
    std::vector<std::string> parts;
    splitPath(path, &parts);
    if (parts.empty()) {
        if (result) {
            result->name.clear();
            result->fnodeSector = rootFnode_;
            result->directory = true;
            result->fileSize = 0;
        }
        if (fnode) *fnode = readFnode(rootFnode_);
        return true;
    }

    std::uint32_t current = rootFnode_;
    bool currentIsDirectory = true;
    EntryInfo found;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const std::vector<EntryInfo> children = readDirectory(current, currentIsDirectory);
        bool have = false;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (nameEqual(children[i].name, parts[p])) {
                found = children[i];
                have = true;
                break;
            }
        }
        if (!have) return false;
        if (p + 1 < parts.size() && !found.directory) return false;
        current = found.fnodeSector;
        currentIsDirectory = found.directory;
    }
    const FnodeInfo resolved = readFnode(current);
    if (!resolved.valid) return false;
    if (result) *result = found;
    if (fnode) *fnode = resolved;
    return true;
}

std::vector<DiscEntry> HpfsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> result;
    EntryInfo entry;
    FnodeInfo fnode;
    if (!resolvePath(dirPath, &entry, &fnode) || !(entry.directory || fnode.directory)) return result;
    const std::vector<EntryInfo> children = readDirectory(fnode.sector, true);
    for (std::size_t i = 0; i < children.size(); ++i) {
        const EntryInfo& child = children[i];
        result.push_back(DiscEntry(child.name, child.directory,
                                   child.directory ? 0 : child.fileSize));
    }
    return result;
}

ByteStorePtr HpfsReader::openFile(const std::string& path) const {
    EntryInfo entry;
    FnodeInfo fnode;
    if (!resolvePath(path, &entry, &fnode) || entry.directory || fnode.directory) return ByteStorePtr();
    return fileContent(fnode, entry.fileSize);
}

}  // namespace fs
}  // namespace peare
