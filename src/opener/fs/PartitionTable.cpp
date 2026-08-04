#include "PartitionTable.h"
#include "JfsReader.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <set>

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


bool isPowerOfTwo(std::uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool looksLikeFatBoot(const std::vector<std::uint8_t>& sec, std::int64_t volumeBytes) {
    if (sec.size() < 512 || sec[510] != 0x55 || sec[511] != 0xAA) return false;
    const std::uint32_t bytesPerSector = std::uint32_t(sec[11]) |
        (std::uint32_t(sec[12]) << 8);
    const std::uint32_t sectorsPerCluster = sec[13];
    const std::uint32_t reserved = std::uint32_t(sec[14]) |
        (std::uint32_t(sec[15]) << 8);
    const std::uint32_t fats = sec[16];
    const std::uint32_t rootEntries = std::uint32_t(sec[17]) |
        (std::uint32_t(sec[18]) << 8);
    const std::uint32_t total16 = std::uint32_t(sec[19]) |
        (std::uint32_t(sec[20]) << 8);
    const std::uint32_t fat16 = std::uint32_t(sec[22]) |
        (std::uint32_t(sec[23]) << 8);
    const std::uint32_t total32 = le32(sec.data() + 32);
    const std::uint32_t fat32 = le32(sec.data() + 36);
    const std::uint64_t total = total16 ? total16 : total32;
    const std::uint32_t fatSectors = fat16 ? fat16 : fat32;
    if ((bytesPerSector != 512 && bytesPerSector != 1024 &&
         bytesPerSector != 2048 && bytesPerSector != 4096) ||
        !isPowerOfTwo(sectorsPerCluster) || sectorsPerCluster > 128 ||
        reserved == 0 || (fats != 1 && fats != 2) || total == 0 || fatSectors == 0)
        return false;
    if (volumeBytes > 0 && total > static_cast<std::uint64_t>(volumeBytes) / bytesPerSector)
        return false;
    const std::uint64_t rootSectors =
        (static_cast<std::uint64_t>(rootEntries) * 32 + bytesPerSector - 1) / bytesPerSector;
    const std::uint64_t overhead = reserved + static_cast<std::uint64_t>(fats) * fatSectors + rootSectors;
    return overhead < total;
}

bool looksLikeNtfsBoot(const std::vector<std::uint8_t>& sec, std::int64_t volumeBytes) {
    if (sec.size() < 512 || sec[510] != 0x55 || sec[511] != 0xAA ||
        std::memcmp(sec.data() + 3, "NTFS    ", 8) != 0)
        return false;
    const std::uint32_t bps = std::uint32_t(sec[11]) | (std::uint32_t(sec[12]) << 8);
    const std::uint32_t spc = sec[13];
    const std::uint64_t total = le64(sec.data() + 40);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) ||
        !isPowerOfTwo(spc) || total == 0)
        return false;
    return volumeBytes <= 0 || total <= static_cast<std::uint64_t>(volumeBytes) / bps;
}

bool looksLikeExfatBoot(const std::vector<std::uint8_t>& sec, std::int64_t volumeBytes) {
    if (sec.size() < 512 || std::memcmp(sec.data() + 3, "EXFAT   ", 8) != 0) return false;
    const std::uint8_t bpsShift = sec[108];
    const std::uint8_t spcShift = sec[109];
    const std::uint64_t total = le64(sec.data() + 72);
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25 || total == 0) return false;
    const std::uint64_t bps = std::uint64_t(1) << bpsShift;
    return volumeBytes <= 0 || total <= static_cast<std::uint64_t>(volumeBytes) / bps;
}

bool hasHpfsSuper(const ByteStorePtr& disk, std::int64_t byteStart, std::int64_t byteLength) {
    if (!disk || byteLength < 18 * kSector) return false;
    const std::vector<std::uint8_t> sb = disk->readRange(byteStart + 16 * kSector, 8);
    return sb.size() == 8 && le32(sb.data()) == 0xF995E849U &&
           le32(sb.data() + 4) == 0xFA53E9C5U;
}

bool validJfsSuperAt(const ByteStorePtr& disk, std::int64_t superOffset,
                     std::int64_t availableBytes, std::uint64_t* volumeBytes) {
    if (volumeBytes) *volumeBytes = 0;
    if (!disk || superOffset < 0 || availableBytes < 32 ||
        superOffset > disk->capacity() || availableBytes > disk->capacity() - superOffset)
        return false;
    std::uint8_t sb[32];
    if (disk->read(superOffset, sb, sizeof(sb)) != int(sizeof(sb)) ||
        std::memcmp(sb, "JFS1", 4) != 0)
        return false;
    const std::uint32_t version = le32(sb + 4);
    const std::uint64_t aggregateUnits = le64(sb + 8);
    const std::uint32_t blockSize = le32(sb + 16);
    const std::uint32_t blockShift = std::uint32_t(sb[20]) |
        (std::uint32_t(sb[21]) << 8);
    const std::uint32_t physicalBlockSize = le32(sb + 24);
    const std::uint32_t physicalBlockShift = std::uint32_t(sb[28]) |
        (std::uint32_t(sb[29]) << 8);
    if ((version != 1 && version != 2) || blockSize < 512 || blockSize > 4096 ||
        !isPowerOfTwo(blockSize) || blockShift < 9 || blockShift > 12 ||
        (std::uint32_t(1) << blockShift) != blockSize || aggregateUnits == 0)
        return false;

    std::uint32_t unitSize = 512;
    if (physicalBlockSize >= 512 && physicalBlockSize <= 4096 &&
        isPowerOfTwo(physicalBlockSize) && physicalBlockShift >= 9 &&
        physicalBlockShift <= 12 &&
        (std::uint32_t(1) << physicalBlockShift) == physicalBlockSize)
        unitSize = physicalBlockSize;
    if (aggregateUnits > std::uint64_t(INT64_MAX) / unitSize) return false;
    if (volumeBytes) *volumeBytes = aggregateUnits * unitSize;
    return true;
}

bool hasJfsSuper(const ByteStorePtr& disk, std::int64_t byteStart, std::int64_t byteLength) {
    if (!disk || byteStart < 0 || byteLength <= 0) return false;

    // JFS keeps two aggregate superblock copies. A recoverable volume can have
    // an unusable primary copy while the secondary copy remains authoritative.
    static const std::int64_t kOffsets[] = {0x8000, 0xF000};
    for (std::size_t i = 0; i < sizeof(kOffsets) / sizeof(kOffsets[0]); ++i) {
        const std::int64_t off = kOffsets[i];
        if (off > byteLength || byteLength - off < 32) continue;
        if (validJfsSuperAt(disk, byteStart + off, byteLength - off, nullptr))
            return true;
    }
    return false;
}

void addRecoveredJfsPartition(const ByteStorePtr& disk, std::int64_t byteStart,
                              std::int64_t statedLength, const std::string& typeName,
                              std::vector<PartitionInfo>* out) {
    if (!disk || !out || byteStart < 0 || byteStart >= disk->capacity()) return;
    const std::int64_t available = disk->capacity() - byteStart;
    if (statedLength <= 0 || statedLength > available) statedLength = available;
    std::uint64_t aggregateBytes = 0;
    bool valid = false;
    static const std::int64_t kOffsets[] = {0x8000, 0xF000};
    for (std::size_t i = 0; i < sizeof(kOffsets) / sizeof(kOffsets[0]); ++i) {
        const std::int64_t off = kOffsets[i];
        if (off <= statedLength && statedLength - off >= 32 &&
            validJfsSuperAt(disk, byteStart + off, statedLength - off, &aggregateBytes)) {
            valid = true;
            break;
        }
    }
    if (!valid) return;

    std::int64_t length = statedLength;
    if (aggregateBytes >= 0xF000 + 32 && aggregateBytes <= std::uint64_t(available))
        length = static_cast<std::int64_t>(aggregateBytes);
    PartitionInfo p;
    p.typeName = typeName;
    p.offset = byteStart;
    p.length = length;
    for (std::size_t i = 0; i < out->size(); ++i) {
        PartitionInfo& existing = out->at(i);
        const std::int64_t existingEnd = existing.offset + existing.length;
        const std::int64_t candidateEnd = p.offset + p.length;
        if (p.offset < existingEnd && existing.offset < candidateEnd) {
            if (p.offset < existing.offset) existing = p;
            return;
        }
    }
    out->push_back(p);
}

std::vector<PartitionInfo> recoverJfsPartitions(const ByteStorePtr& disk) {
    std::vector<PartitionInfo> out;
    if (!disk || disk->capacity() < 0x8000 + 32) return out;

    // Recover explicit MBR entries even when the boot marker or active byte is
    // damaged. The JFS aggregate superblock is the authoritative validation.
    const std::vector<std::uint8_t> sector = disk->readRange(0, 512);
    if (sector.size() == 512) {
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t* e = sector.data() + 0x1BE + i * 16;
            const std::uint8_t type = e[4];
            const std::uint32_t startLba = le32(e + 8);
            const std::uint32_t sectors = le32(e + 12);
            if (type == 0 || startLba == 0 || sectors == 0) continue;
            if (startLba > std::uint64_t(INT64_MAX) / kSector) continue;
            const std::int64_t start = std::int64_t(startLba) * kSector;
            if (start <= 0 || start >= disk->capacity()) continue;
            std::int64_t length = disk->capacity() - start;
            if (sectors <= std::uint64_t(INT64_MAX) / kSector) {
                const std::int64_t stated = std::int64_t(sectors) * kSector;
                if (stated > 0 && stated < length) length = stated;
            }
            const std::string name = type == 0x35 ? "JFS (OS/2)" : "JFS";
            addRecoveredJfsPartition(disk, start, length, name, &out);
        }
    }
    if (!out.empty()) return out;

    // A damaged or absent partition table is common in partial images. Search
    // only the early disk area where PC partition starts normally reside, and
    // infer a volume start from either standard JFS superblock location.
    const std::int64_t scanLimit = std::min<std::int64_t>(
        disk->capacity(), std::int64_t(64) * 1024 * 1024 + 0xF000 + 32);
    const std::int64_t chunkSize = 1024 * 1024;
    std::int64_t base = 0;
    std::vector<std::uint8_t> carry;
    while (base < scanLimit) {
        const std::int64_t amount = std::min<std::int64_t>(chunkSize, scanLimit - base);
        std::vector<std::uint8_t> chunk = disk->readRange(base, amount);
        if (chunk.empty()) break;
        std::vector<std::uint8_t> probe;
        probe.reserve(carry.size() + chunk.size());
        probe.insert(probe.end(), carry.begin(), carry.end());
        probe.insert(probe.end(), chunk.begin(), chunk.end());
        const std::int64_t probeBase = base - std::int64_t(carry.size());
        for (std::size_t i = 0; i + 4 <= probe.size(); ++i) {
            if (std::memcmp(probe.data() + i, "JFS1", 4) != 0) continue;
            const std::int64_t magic = probeBase + std::int64_t(i);
            const std::int64_t superOffsets[] = {0x8000, 0xF000};
            for (std::size_t n = 0; n < 2; ++n) {
                const std::int64_t start = magic - superOffsets[n];
                if (start < 0 || (start % kSector) != 0) continue;
                addRecoveredJfsPartition(disk, start, disk->capacity() - start,
                                         "JFS (recovered)", &out);
            }
        }
        carry.assign(probe.end() - std::min<std::size_t>(3, probe.size()), probe.end());
        base += amount;
    }
    return out;
}

bool partitionPayloadPlausible(const ByteStorePtr& disk, std::uint8_t type,
                               std::int64_t byteStart, std::int64_t byteLength) {
    if (!disk || byteStart < 0 || byteLength <= 0) return false;
    if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C) {
        return looksLikeFatBoot(disk->readRange(byteStart, 512), byteLength);
    }
    if (type == 0x07) {
        const std::vector<std::uint8_t> boot = disk->readRange(byteStart, 512);
        return looksLikeNtfsBoot(boot, byteLength) || looksLikeExfatBoot(boot, byteLength) ||
               hasHpfsSuper(disk, byteStart, byteLength);
    }
    // OS/2 LVM assigns type 0x35 to every physical segment belonging to a
    // JFS-capable logical volume.  A segment is not necessarily a standalone
    // JFS image and therefore need not contain an aggregate superblock at
    // 0x8000/0xF000.  The partition type itself is authoritative here; the
    // JFS/LVM opener decides later whether this is a compatibility volume or
    // one component of a linked volume.
    if (type == 0x35) return true;
    // Keep common non-filesystem/system partitions whose contents do not have
    // a cheap boot-sector signature. Unknown type bytes in firmware headers are
    // not sufficient evidence of an MBR partition.
    switch (type) {
    case 0x05: case 0x0A: case 0x0F: case 0x82: case 0x83:
    case 0x85: case 0x8E: case 0xEE: case 0xEF:
        return true;
    default:
        return false;
    }
}

bool hasAuthoritativeOs2LvmEntry(const std::vector<std::uint8_t>& mbr,
                                  std::int64_t diskSectors) {
    if (mbr.size() < 512 || diskSectors <= 0) return false;
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t* e = mbr.data() + 0x1BE + i * 16;
        const std::uint8_t type = e[4];
        const std::uint32_t start = le32(e + 8);
        const std::uint32_t count = le32(e + 12);
        if (type != 0x35 && !isExtended(type)) continue;
        if (start == 0 || count == 0) continue;
        if (static_cast<std::int64_t>(start) >= diskSectors) continue;
        // A slightly truncated image may report a tail beyond the available
        // store.  The starting location is sufficient to establish that this
        // is a real partition-table entry rather than BPB noise.
        return true;
    }
    return false;
}

bool looksLikeStandaloneFileSystem(const ByteStorePtr& disk) {
    if (!disk || disk->capacity() < 512) return false;
    const std::vector<std::uint8_t> boot = disk->readRange(0, 512);
    return looksLikeFatBoot(boot, disk->capacity()) ||
           looksLikeNtfsBoot(boot, disk->capacity()) ||
           looksLikeExfatBoot(boot, disk->capacity()) ||
           hasHpfsSuper(disk, 0, disk->capacity()) || hasJfsSuper(disk, 0, disk->capacity());
}

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

void readMbr(const ByteStorePtr& disk, std::int64_t baseSector,
             std::int64_t ebrBaseSector, std::vector<PartitionInfo>* out,
             std::set<std::int64_t>* visitedEbr, int depth) {
    if (!disk || !out || !visitedEbr || depth > 64 || baseSector < 0) return;
    const std::int64_t diskSectors = disk->capacity() / kSector;
    if (baseSector >= diskSectors || !visitedEbr->insert(baseSector).second) return;

    std::vector<std::uint8_t> sec = disk->readRange(baseSector * kSector, 512);
    if (sec.size() < 512 || sec[510] != 0x55 || sec[511] != 0xAA) return;

    for (int i = 0; i < 4; ++i) {
        const std::uint8_t* e = sec.data() + 0x1BE + i * 16;
        const std::uint8_t boot = e[0];
        const std::uint8_t type = e[4];
        const std::uint32_t startLba = le32(e + 8);
        const std::uint32_t sectors = le32(e + 12);
        const bool os2LvmSegment = type == 0x35;
        // OS/2 LVM metadata is authoritative for type 0x35.  Preserve such
        // segments even in imperfect forensic images whose active byte is
        // non-standard.  Other partition types retain strict MBR validation.
        if ((!os2LvmSegment && boot != 0 && boot != 0x80) ||
            type == 0 || startLba == 0 || sectors == 0)
            continue;

        if (isExtended(type)) {
            // The first extended entry is relative to the MBR.  Links in an
            // EBR chain are relative to the original extended-partition base.
            const std::int64_t next = ebrBaseSector == 0
                ? static_cast<std::int64_t>(startLba)
                : ebrBaseSector + static_cast<std::int64_t>(startLba);
            if (next > 0 && next < diskSectors) {
                const std::int64_t chainBase = ebrBaseSector == 0 ? next : ebrBaseSector;
                readMbr(disk, next, chainBase, out, visitedEbr, depth + 1);
            }
            continue;
        }

        const std::int64_t absoluteStart = baseSector + static_cast<std::int64_t>(startLba);
        std::int64_t count = static_cast<std::int64_t>(sectors);
        if (absoluteStart <= 0 || absoluteStart >= diskSectors || count <= 0)
            continue;
        const std::int64_t available = diskSectors - absoluteStart;
        if (count > available) {
            // A truncated VMDK/RAW capture can end before the size recorded by
            // OS/2 LVM.  Keep the readable prefix of a known type-0x35 segment;
            // all other types are rejected rather than silently clamped.
            if (!os2LvmSegment) continue;
            count = available;
        }

        const std::int64_t byteStart = absoluteStart * kSector;
        const std::int64_t byteLength = count * kSector;
        if (!partitionPayloadPlausible(disk, type, byteStart, byteLength)) continue;

        PartitionInfo p;
        p.typeName = mbrTypeName(type);
        p.offset = byteStart;
        p.length = byteLength;
        p.mbrType = type;
        out->push_back(p);
    }
}

std::vector<PartitionInfo> readGpt(const ByteStorePtr& disk) {
    std::vector<PartitionInfo> out;
    std::vector<std::uint8_t> hdr = disk->readRange(1 * kSector, 512);
    if (hdr.size() < 92 || std::memcmp(hdr.data(), "EFI PART", 8) != 0) return out;

    const std::uint32_t headerSize = le32(hdr.data() + 12);
    const std::uint64_t currentLba = le64(hdr.data() + 24);
    const std::uint64_t firstUsable = le64(hdr.data() + 40);
    const std::uint64_t lastUsable = le64(hdr.data() + 48);
    const std::uint64_t entryLba = le64(hdr.data() + 72);
    const std::uint32_t numEntries = le32(hdr.data() + 80);
    const std::uint32_t entrySize = le32(hdr.data() + 84);
    const std::uint64_t diskSectors = static_cast<std::uint64_t>(disk->capacity() / kSector);
    if (headerSize < 92 || headerSize > 512 || currentLba != 1 ||
        firstUsable > lastUsable || lastUsable >= diskSectors ||
        entrySize < 128 || entrySize > 4096 || numEntries == 0 || numEntries > 4096)
        return out;

    const std::uint64_t tableBytes64 = static_cast<std::uint64_t>(numEntries) * entrySize;
    if (entryLba >= diskSectors || tableBytes64 > static_cast<std::uint64_t>(INT64_MAX) ||
        tableBytes64 > (diskSectors - entryLba) * kSector)
        return out;
    const std::int64_t tableBytes = static_cast<std::int64_t>(tableBytes64);
    std::vector<std::uint8_t> table = disk->readRange(
        static_cast<std::int64_t>(entryLba * kSector), tableBytes);
    for (std::uint32_t i = 0; i < numEntries; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * entrySize;
        if (off + 128 > table.size()) break;
        const std::uint8_t* e = table.data() + off;
        std::string name = gptTypeName(e);
        if (name.empty()) continue;  // unused entry
        const std::uint64_t firstLba = le64(e + 32);
        const std::uint64_t lastLba = le64(e + 40);
        if (lastLba < firstLba || firstLba < firstUsable ||
            lastLba > lastUsable || lastLba >= diskSectors) continue;
        const std::uint64_t sectors = lastLba - firstLba + 1;
        if (firstLba > static_cast<std::uint64_t>(INT64_MAX / kSector) ||
            sectors > static_cast<std::uint64_t>(INT64_MAX / kSector)) continue;
        PartitionInfo p;
        p.typeName = name;
        p.offset = static_cast<std::int64_t>(firstLba) * kSector;
        p.length = static_cast<std::int64_t>(sectors) * kSector;
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

ByteStorePtr partitionStore(const ByteStorePtr& disk, const PartitionInfo& p,
                            std::int64_t usableLength = -1) {
    if (!disk || p.offset < 0 || p.length <= 0 || p.offset >= disk->capacity())
        return ByteStorePtr();
    std::int64_t length = std::min<std::int64_t>(p.length, disk->capacity() - p.offset);
    if (usableLength >= 0) length = std::min(length, usableLength);
    if (length <= 0) return ByteStorePtr();
    return std::make_shared<SubStore>(disk, p.offset, length);
}

void appendTrimPlan(std::vector<std::vector<std::int64_t> >* plans,
                    const std::vector<std::int64_t>& plan,
                    const std::vector<std::int64_t>& capacities) {
    if (!plans || plan.size() != capacities.size()) return;
    for (std::size_t i = 0; i < plan.size(); ++i)
        if (plan[i] < 0 || plan[i] >= capacities[i] || (plan[i] % kSector) != 0)
            return;
    if (std::find(plans->begin(), plans->end(), plan) == plans->end())
        plans->push_back(plan);
}

std::vector<std::vector<std::int64_t> > driveLinkTrimPlans(
        const std::vector<std::int64_t>& capacities, std::uint64_t declaredBytes) {
    std::vector<std::vector<std::int64_t> > plans;
    if (capacities.empty() || declaredBytes > static_cast<std::uint64_t>(INT64_MAX))
        return plans;
    std::int64_t total = 0;
    for (std::int64_t capacity : capacities) {
        if (capacity <= 0 || total > INT64_MAX - capacity) return plans;
        total += capacity;
    }
    const std::int64_t declared = static_cast<std::int64_t>(declaredBytes);
    if (total < declared) return plans;
    const std::int64_t overhead = total - declared;

    // No internal metadata gap.  A final SubStore below still clips any bytes
    // following the aggregate in the last child.
    appendTrimPlan(&plans, std::vector<std::int64_t>(capacities.size(), 0), capacities);
    if (overhead == 0 || (overhead % kSector) != 0) return plans;

    // OS/2 LVM reserves its metadata at the end of physical children.  Try the
    // useful exact distributions of the total non-aggregate bytes rather than
    // concatenating those trailers into the middle of the JFS address space.
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        std::vector<std::int64_t> plan(capacities.size(), 0);
        plan[i] = overhead;
        appendTrimPlan(&plans, plan, capacities);
    }

    const std::size_t n = capacities.size();
    if (n <= 12) {
        const std::uint64_t masks = std::uint64_t(1) << n;
        for (std::uint64_t mask = 1; mask < masks; ++mask) {
            std::size_t count = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (mask & (std::uint64_t(1) << i)) ++count;
            if (count == 0 || overhead % static_cast<std::int64_t>(count) != 0)
                continue;
            const std::int64_t each = overhead / static_cast<std::int64_t>(count);
            if ((each % kSector) != 0) continue;
            std::vector<std::int64_t> plan(n, 0);
            for (std::size_t i = 0; i < n; ++i)
                if (mask & (std::uint64_t(1) << i)) plan[i] = each;
            appendTrimPlan(&plans, plan, capacities);
        }
    }

    // Common LVM trailer sizes plus the remaining bytes on one child.  This
    // covers an aggregate signature stored in one member in addition to the
    // per-member data areas.
    static const std::int64_t common[] = {
        512, 4096, 16 * 512, 32 * 512, 64 * 512, 128 * 512,
        256 * 512, 512 * 512, 1024 * 512, 2048 * 512
    };
    for (std::int64_t base : common) {
        if (base <= 0 || base > overhead ||
            base > INT64_MAX / static_cast<std::int64_t>(n)) continue;
        const std::int64_t baseTotal = base * static_cast<std::int64_t>(n);
        if (baseTotal > overhead) continue;
        for (std::size_t extraAt = 0; extraAt < n; ++extraAt) {
            std::vector<std::int64_t> plan(n, base);
            plan[extraAt] += overhead - baseTotal;
            appendTrimPlan(&plans, plan, capacities);
        }
    }
    return plans;
}

std::vector<PartitionInfo> assembleOs2JfsVolumes(const ByteStorePtr& disk,
                                                  const std::vector<PartitionInfo>& input) {
    if (!disk || input.size() < 2) return input;
    std::vector<std::size_t> os2;
    for (std::size_t i = 0; i < input.size(); ++i)
        if (input[i].mbrType == 0x35) os2.push_back(i);
    if (os2.size() < 2) return input;

    // DriveLink exposes a linear address space assembled from type-0x35
    // children. Only attempt reconstruction when a child contains a valid JFS
    // superblock that declares an aggregate larger than that physical child.
    for (std::size_t firstPos = 0; firstPos < os2.size(); ++firstPos) {
        const std::size_t firstIndex = os2[firstPos];
        const PartitionInfo& first = input[firstIndex];
        ByteStorePtr directStore = partitionStore(disk, first);
        JfsReader direct(directStore);
        if (!direct.valid() || direct.declaredAggregateBytes() <=
                                   static_cast<std::uint64_t>(first.length))
            continue;

        std::vector<std::size_t> tail;
        for (std::size_t n = 0; n < os2.size(); ++n)
            if (n != firstPos) tail.push_back(os2[n]);
        std::sort(tail.begin(), tail.end(), [&](std::size_t a, std::size_t b) {
            return input[a].offset < input[b].offset;
        });

        ByteStorePtr best;
        int bestScore = direct.qualityScore();
        bool bestTrimmed = false;
        std::size_t permutationCount = 0;
        do {
            std::vector<std::size_t> order;
            order.reserve(os2.size());
            order.push_back(firstIndex);
            order.insert(order.end(), tail.begin(), tail.end());

            std::vector<std::int64_t> capacities;
            capacities.reserve(order.size());
            for (std::size_t index : order) {
                const PartitionInfo& p = input[index];
                const std::int64_t available = p.offset >= 0 && p.offset < disk->capacity()
                    ? std::min<std::int64_t>(p.length, disk->capacity() - p.offset) : 0;
                capacities.push_back(available);
            }
            const std::vector<std::vector<std::int64_t> > trimPlans =
                driveLinkTrimPlans(capacities, direct.declaredAggregateBytes());
            for (const std::vector<std::int64_t>& trims : trimPlans) {
                std::vector<ByteStorePtr> children;
                children.reserve(order.size());
                bool complete = true;
                bool trimmed = false;
                for (std::size_t i = 0; i < order.size(); ++i) {
                    const std::int64_t usable = capacities[i] - trims[i];
                    ByteStorePtr child = partitionStore(disk, input[order[i]], usable);
                    if (!child || child->capacity() <= 0) complete = false;
                    if (trims[i] != 0) trimmed = true;
                    children.push_back(child);
                }
                if (!complete) continue;

                ByteStorePtr joined = std::make_shared<ConcatStore>(children);
                const std::uint64_t declared = direct.declaredAggregateBytes();
                if (declared > static_cast<std::uint64_t>(INT64_MAX) ||
                    joined->capacity() < static_cast<std::int64_t>(declared))
                    continue;
                joined = std::make_shared<SubStore>(joined, 0,
                                                    static_cast<std::int64_t>(declared));
                JfsReader probe(joined);
                if (!probe.valid()) continue;
                // Require a material improvement over the partial child. The
                // score includes resolved inodes, nested directories and files.
                if (probe.qualityScore() > bestScore + 8) {
                    bestScore = probe.qualityScore();
                    best = joined;
                    bestTrimmed = trimmed;
                }
            }
            ++permutationCount;
            // Exact ordering search is bounded; large aggregates retain the
            // physical order to avoid factorial probing.
            if (tail.size() > 7 || permutationCount >= 5040) break;
        } while (std::next_permutation(tail.begin(), tail.end()));
        if (!best) continue;

        std::vector<PartitionInfo> out;
        out.reserve(input.size() - os2.size() + 1);
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (std::find(os2.begin(), os2.end(), i) != os2.end()) continue;
            out.push_back(input[i]);
        }
        PartitionInfo logical;
        logical.typeName = bestTrimmed ? "JFS (OS/2 DriveLink)" : "JFS (OS/2 linked)";
        logical.offset = first.offset;
        logical.length = best->capacity();
        logical.mbrType = 0x35;
        logical.content = best;
        out.push_back(logical);
        std::sort(out.begin(), out.end(), [](const PartitionInfo& a, const PartitionInfo& b) {
            return a.offset < b.offset;
        });
        return out;
    }
    return input;
}

}  // namespace

bool hasApplePartitionMap(const ByteStorePtr& disk) {
    if (!disk || disk->capacity() < 1024) return false;
    std::vector<std::uint8_t> first = disk->readRange(0, 1024);
    return first.size() >= 1024 && be16(first.data()) == 0x4552 &&
           be16(first.data() + 512) == 0x504d;
}

bool hasMbrOrGptPartitionTable(const ByteStorePtr& disk) {
    return disk && !readPartitionTable(disk).empty();
}

std::vector<PartitionInfo> readPartitionTable(const ByteStorePtr& disk) {
    if (!disk) return {};
    std::vector<PartitionInfo> apm = readApm(disk);
    if (!apm.empty()) return apm;

    std::vector<std::uint8_t> mbr = disk->readRange(0, 512);
    if (mbr.size() < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return recoverJfsPartitions(disk);
    // FAT/NTFS/exFAT/HPFS/JFS boot sectors also end in 55AA. Prefer a
    // directly mounted filesystem over random BPB bytes, except when the MBR
    // explicitly describes an OS/2 LVM/JFS partition or an extended chain.
    // Type 0x35 is authoritative even when the physical segment has no local
    // JFS superblock.
    const bool authoritativeOs2Layout =
        hasAuthoritativeOs2LvmEntry(mbr, disk->capacity() / kSector);
    if (!authoritativeOs2Layout && looksLikeStandaloneFileSystem(disk)) {
        const std::vector<PartitionInfo> recovered = recoverJfsPartitions(disk);
        if (!recovered.empty()) return recovered;
        return {};
    }

    // GPT if the protective MBR points at it, or an "EFI PART" header is present.
    bool gpt = false;
    for (int i = 0; i < 4; ++i)
        if (mbr[0x1BE + i * 16 + 4] == 0xEE) gpt = true;
    if (gpt) {
        std::vector<PartitionInfo> g = readGpt(disk);
        if (!g.empty()) return g;
    }

    std::vector<PartitionInfo> out;
    std::set<std::int64_t> visitedEbr;
    readMbr(disk, 0, 0, &out, &visitedEbr, 0);
    if (out.empty()) return recoverJfsPartitions(disk);
    bool hasJfs = false;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (hasJfsSuper(disk, out[i].offset, out[i].length)) hasJfs = true;
    if (!hasJfs) {
        const std::vector<PartitionInfo> recovered = recoverJfsPartitions(disk);
        out.insert(out.end(), recovered.begin(), recovered.end());
    }
    return assembleOs2JfsVolumes(disk, out);
}

}  // namespace fs
}  // namespace peare
