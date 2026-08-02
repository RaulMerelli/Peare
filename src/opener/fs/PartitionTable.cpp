#include "PartitionTable.h"

#include <cstring>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSector = 512;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}
std::uint16_t be16(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::string mbrTypeName(std::uint8_t t) {
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: case 0x06: return "FAT16";
        case 0x07: return "HPFS/NTFS/exFAT";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x0A: return "OS/2 Boot Manager";
        case 0x35: return "JFS (OS/2)";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI System";
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "type 0x%02X", t);
            return std::string(buf);
        }
    }
}

bool isExtended(std::uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

// GPT partition type GUIDs (first 4 bytes little-endian data1 + a couple bytes
// are enough to name the common ones).
std::string gptTypeName(const std::uint8_t* guid) {
    static const std::uint8_t kZero[16] = {0};
    if (std::memcmp(guid, kZero, 16) == 0) return std::string();  // unused
    const std::uint32_t d1 = le32(guid);
    if (d1 == 0xC12A7328u) return "EFI System";
    if (d1 == 0xEBD0A0A2u) return "Microsoft Basic Data";
    if (d1 == 0xE3C9E316u) return "Microsoft Reserved";
    if (d1 == 0x0FC63DAFu) return "Linux filesystem";
    if (d1 == 0xA19D880Fu) return "Linux RAID";
    if (d1 == 0x0657FD6Du) return "Linux swap";
    return "GPT partition";
}

void readMbr(const ByteStorePtr& disk, std::int64_t baseSector, std::int64_t ebrBaseSector,
             std::vector<PartitionInfo>* out, int depth) {
    if (depth > 64) return;  // guard against malformed extended chains
    std::vector<std::uint8_t> sec = disk->readRange(baseSector * kSector, 512);
    if (sec.size() < 512 || sec[510] != 0x55 || sec[511] != 0xAA) return;

    for (int i = 0; i < 4; ++i) {
        const std::uint8_t* e = sec.data() + 0x1BE + i * 16;
        const std::uint8_t type = e[4];
        const std::uint32_t startLba = le32(e + 8);
        const std::uint32_t sectors = le32(e + 12);
        if (type == 0 || sectors == 0) continue;

        if (isExtended(type)) {
            // Extended/logical: startLba is relative to the extended base.
            const std::int64_t next =
                (ebrBaseSector == 0 ? startLba : ebrBaseSector + startLba);
            readMbr(disk, next, ebrBaseSector == 0 ? next : ebrBaseSector, out, depth + 1);
            continue;
        }

        PartitionInfo p;
        p.typeName = mbrTypeName(type);
        p.offset = (baseSector + startLba) * kSector;
        p.length = static_cast<std::int64_t>(sectors) * kSector;
        out->push_back(p);
    }
}

std::vector<PartitionInfo> readGpt(const ByteStorePtr& disk) {
    std::vector<PartitionInfo> out;
    std::vector<std::uint8_t> hdr = disk->readRange(1 * kSector, 512);
    if (hdr.size() < 92 || std::memcmp(hdr.data(), "EFI PART", 8) != 0) return out;

    const std::uint64_t entryLba = le64(hdr.data() + 72);
    const std::uint32_t numEntries = le32(hdr.data() + 80);
    const std::uint32_t entrySize = le32(hdr.data() + 84);
    if (entrySize < 128 || numEntries == 0 || numEntries > 4096) return out;

    const std::int64_t tableBytes = static_cast<std::int64_t>(numEntries) * entrySize;
    std::vector<std::uint8_t> table = disk->readRange(entryLba * kSector, tableBytes);
    for (std::uint32_t i = 0; i < numEntries; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * entrySize;
        if (off + 128 > table.size()) break;
        const std::uint8_t* e = table.data() + off;
        std::string name = gptTypeName(e);
        if (name.empty()) continue;  // unused entry
        const std::uint64_t firstLba = le64(e + 32);
        const std::uint64_t lastLba = le64(e + 40);
        if (lastLba < firstLba) continue;
        PartitionInfo p;
        p.typeName = name;
        p.offset = static_cast<std::int64_t>(firstLba) * kSector;
        p.length = static_cast<std::int64_t>(lastLba - firstLba + 1) * kSector;
        out.push_back(p);
    }
    return out;
}

std::string latin1Trimmed(const std::uint8_t* p, int n) {
    std::string s;
    s.reserve(std::size_t(n));
    for (int i = 0; i < n && p[i] != 0; ++i)
        s.push_back(char(p[i]));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::vector<PartitionInfo> readApm(const ByteStorePtr& disk) {
    std::vector<std::uint8_t> first = disk ? disk->readRange(0, 1024) : std::vector<std::uint8_t>();
    if (first.size() < 1024 || be16(first.data()) != 0x4552 ||
        be16(first.data() + 512) != 0x504d)
        return {};

    const std::uint32_t entries = be32(first.data() + 512 + 4);
    if (entries < 2 || entries > 4096) return {};
    const std::int64_t tableBytes = std::int64_t(entries - 1) * kSector;
    std::vector<std::uint8_t> table = disk->readRange(2 * kSector, tableBytes);
    if (table.size() < kSector) return {};

    std::vector<PartitionInfo> out;
    for (std::uint32_t i = 0; i < entries - 1; ++i) {
        const std::size_t off = std::size_t(i) * std::size_t(kSector);
        if (off + kSector > table.size()) break;
        const std::uint8_t* e = table.data() + off;
        if (be16(e) != 0x504d) continue;
        const std::uint32_t start = be32(e + 8);
        const std::uint32_t blocks = be32(e + 12);
        if (blocks == 0) continue;
        const std::int64_t byteStart = std::int64_t(start) * kSector;
        const std::int64_t byteLength = std::int64_t(blocks) * kSector;
        if (byteStart < 0 || byteStart >= disk->capacity() || byteLength <= 0 ||
            byteLength > disk->capacity() - byteStart)
            continue;
        std::string type = latin1Trimmed(e + 48, 32);
        std::string name = latin1Trimmed(e + 16, 32);
        PartitionInfo p;
        p.typeName = type.empty() ? "Apple partition" : type;
        if (!name.empty() && name != type)
            p.typeName += " (" + name + ")";
        p.offset = byteStart;
        p.length = byteLength;
        out.push_back(p);
    }
    return out;
}

}  // namespace

bool hasApplePartitionMap(const ByteStorePtr& disk) {
    if (!disk || disk->capacity() < 1024) return false;
    std::vector<std::uint8_t> first = disk->readRange(0, 1024);
    return first.size() >= 1024 && be16(first.data()) == 0x4552 &&
           be16(first.data() + 512) == 0x504d;
}

bool hasMbrOrGptPartitionTable(const ByteStorePtr& disk) {
    if (!disk) return false;
    std::vector<std::uint8_t> mbr = disk->readRange(0, 512);
    return mbr.size() >= 512 && mbr[510] == 0x55 && mbr[511] == 0xAA;
}

std::vector<PartitionInfo> readPartitionTable(const ByteStorePtr& disk) {
    if (!disk) return {};
    std::vector<PartitionInfo> apm = readApm(disk);
    if (!apm.empty()) return apm;

    std::vector<std::uint8_t> mbr = disk->readRange(0, 512);
    if (mbr.size() < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) return {};

    // GPT if the protective MBR points at it, or an "EFI PART" header is present.
    bool gpt = false;
    for (int i = 0; i < 4; ++i)
        if (mbr[0x1BE + i * 16 + 4] == 0xEE) gpt = true;
    if (gpt) {
        std::vector<PartitionInfo> g = readGpt(disk);
        if (!g.empty()) return g;
    }

    std::vector<PartitionInfo> out;
    readMbr(disk, 0, 0, &out, 0);
    return out;
}

}  // namespace fs
}  // namespace peare
