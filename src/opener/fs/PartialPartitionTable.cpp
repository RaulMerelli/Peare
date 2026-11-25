#include "PartialPartitionTable.h"
#include "../compression/Crc32.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace peare {
namespace fs {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}
bool nonzero(const std::uint8_t* p, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) if (p[i]) return true;
    return false;
}
PartialTableStatus exact(const ByteStorePtr& source, std::uint64_t pos,
                         std::uint8_t* data, std::size_t size) {
    if (!source || !source->validate() || source->capacity() < 0)
        return PartialTableStatus::IoError;
    const std::uint64_t capacity = static_cast<std::uint64_t>(source->capacity());
    if (pos > capacity || size > capacity - pos) return PartialTableStatus::Incomplete;
    while (size) {
        const int n = static_cast<int>(std::min<std::size_t>(size, 65536));
        const int got = source->read(static_cast<std::int64_t>(pos), data, n);
        if (got <= 0 || got > n || !source->validate()) return PartialTableStatus::IoError;
        pos += static_cast<unsigned int>(got);
        data += got;
        size -= static_cast<unsigned int>(got);
    }
    return PartialTableStatus::Valid;
}
bool extended(std::uint8_t type) { return type == 5 || type == 15 || type == 0x85; }

PartialMbrTable readMbr(const ByteStorePtr& source, std::uint32_t sectorSize) {
    PartialMbrTable result;
    if (sectorSize < 512 || (sectorSize & (sectorSize - 1)) != 0) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    std::uint8_t sector[512];
    result.status = exact(source, 0, sector, sizeof(sector));
    if (result.status != PartialTableStatus::Valid) return result;
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        result.status = PartialTableStatus::Absent;
        return result;
    }
    result.diskSignature = le32(sector + 440);
    std::vector<std::pair<std::uint64_t, std::uint64_t> > primary;
    std::vector<std::pair<std::uint64_t, std::uint64_t> > containers;
    for (unsigned int i = 0; i < 4; ++i) {
        const std::uint8_t* p = sector + 446 + i * 16;
        if (!p[4]) continue;
        const std::uint64_t start = le32(p + 8), length = le32(p + 12);
        if ((p[0] != 0 && p[0] != 0x80) || !start || !length) {
            result.status = PartialTableStatus::Invalid;
            result.entries.clear();
            return result;
        }
        if (p[4] == 0xee) { result.protective = true; continue; }
        for (const auto& range : primary) {
            if (start < range.second && range.first < start + length) {
                result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
            }
        }
        primary.push_back(std::make_pair(start, start + length));
        if (extended(p[4])) {
            result.hasExtended = true;
            containers.push_back(std::make_pair(start, length));
            continue;
        }
        PartialPartitionEntry entry;
        entry.mbrType = p[4]; entry.firstLba = start; entry.sectorCount = length;
        result.entries.push_back(entry);
    }
    std::set<std::uint64_t> ebrSectors;
    for (const auto& container : containers) {
        const std::uint64_t base = container.first, limit = base + container.second;
        std::uint64_t current = base;
        for (;;) {
            if (current < base || current >= limit || !ebrSectors.insert(current).second ||
                current > std::uint64_t(INT64_MAX) / sectorSize) {
                result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
            }
            std::uint8_t ebr[512];
            result.status = exact(source, current * sectorSize, ebr, sizeof(ebr));
            if (result.status != PartialTableStatus::Valid) { result.entries.clear(); return result; }
            if (ebr[510] != 0x55 || ebr[511] != 0xaa) {
                result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
            }
            const std::uint8_t* item = ebr + 446;
            const std::uint8_t* link = item + 16;
            const std::uint64_t relative = le32(item + 8), length = le32(item + 12);
            if ((item[0] != 0 && item[0] != 0x80) || !item[4] || extended(item[4]) ||
                item[4] == 0xee || !relative || !length || relative >= limit - current ||
                length > limit - current - relative || ebr[446 + 2 * 16 + 4] || ebr[446 + 3 * 16 + 4]) {
                result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
            }
            PartialPartitionEntry entry;
            entry.mbrType = item[4]; entry.firstLba = current + relative; entry.sectorCount = length;
            result.entries.push_back(entry);
            if (!link[4]) {
                if (le32(link + 8) || le32(link + 12)) {
                    result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
                }
                break;
            }
            const std::uint64_t next = le32(link + 8), nextLength = le32(link + 12);
            if ((link[0] != 0 && link[0] != 0x80) || !extended(link[4]) || !next || !nextLength ||
                next >= container.second || nextLength > container.second - next) {
                result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
            }
            current = base + next;
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t> > ranges;
    for (const auto& entry : result.entries) {
        const std::uint64_t end = entry.firstLba + entry.sectorCount;
        for (std::uint64_t ebr : ebrSectors) if (ebr >= entry.firstLba && ebr < end) {
            result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
        }
        ranges.push_back(std::make_pair(entry.firstLba, end));
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) {
        result.status = PartialTableStatus::Invalid; result.entries.clear(); return result;
    }
    return result;
}

PartialGptTable readGpt(const ByteStorePtr& source, std::uint32_t sectorSize) {
    PartialGptTable result;
    if (sectorSize < 512 || (sectorSize & (sectorSize - 1)) != 0) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    if (!source || !source->validate() || source->capacity() < 0) {
        result.status = PartialTableStatus::IoError; return result;
    }
    if (std::uint64_t(sectorSize) * 2 > static_cast<std::uint64_t>(source->capacity())) {
        result.status = PartialTableStatus::Incomplete; return result;
    }
    std::vector<std::uint8_t> header(sectorSize);
    result.status = exact(source, sectorSize, header.data(), header.size());
    if (result.status != PartialTableStatus::Valid) return result;
    if (std::memcmp(header.data(), "EFI PART", 8)) {
        result.status = PartialTableStatus::Absent; return result;
    }
    const std::uint32_t size = le32(header.data() + 12), expected = le32(header.data() + 16);
    const std::uint64_t first = le64(header.data() + 40), last = le64(header.data() + 48);
    const std::uint64_t tableLba = le64(header.data() + 72);
    const std::uint32_t count = le32(header.data() + 80), entrySize = le32(header.data() + 84);
    if (size < 92 || size > sectorSize || le64(header.data() + 24) != 1 ||
        first > last || le64(header.data() + 32) <= last || !nonzero(header.data() + 56, 16) ||
        tableLba < 2 || tableLba >= first || !count || entrySize < 128 ||
        entrySize % 128 || (entrySize / 128 & (entrySize / 128 - 1))) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    std::fill(header.begin() + 16, header.begin() + 20, 0);
    if (compression::crc32(header.data(), size) != expected) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    const std::uint64_t tableSize = std::uint64_t(count) * entrySize;
    const std::uint64_t tableSectors = tableSize / sectorSize + (tableSize % sectorSize != 0);
    if (tableLba > std::uint64_t(INT64_MAX) / sectorSize ||
        tableSectors > first - tableLba) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    const std::uint64_t offset = tableLba * sectorSize;
    if (offset > std::uint64_t(INT64_MAX) || tableSize > std::uint64_t(INT64_MAX) - offset) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    std::vector<PartialPartitionEntry> entries;
    std::uint32_t crc = 0xffffffffU;
    const auto& crcTable = compression::crc32Table()[0];
    std::uint8_t buffer[4096], prefix[128];
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t consumed = 0;
        while (consumed < entrySize) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(buffer), entrySize - consumed));
            result.status = exact(source, offset + std::uint64_t(i) * entrySize + consumed, buffer, n);
            if (result.status != PartialTableStatus::Valid) return result;
            if (!consumed) std::copy(buffer, buffer + sizeof(prefix), prefix);
            for (std::size_t j = 0; j < n; ++j) crc = (crc >> 8) ^ crcTable[(crc ^ buffer[j]) & 0xffU];
            consumed += n;
        }
        if (!nonzero(prefix, 16)) continue;
        const std::uint64_t start = le64(prefix + 32), end = le64(prefix + 40);
        if (!nonzero(prefix + 16, 16) || start < first || end > last || start > end || end == UINT64_MAX) {
            result.status = PartialTableStatus::Invalid; return result;
        }
        PartialPartitionEntry entry;
        entry.firstLba = start; entry.sectorCount = end - start + 1;
        std::copy(prefix, prefix + 16, entry.typeGuid.begin());
        std::copy(prefix + 16, prefix + 32, entry.partitionGuid.begin());
        entries.push_back(entry);
    }
    if (~crc != le32(header.data() + 88)) {
        result.status = PartialTableStatus::Invalid; return result;
    }
    std::copy(header.begin() + 56, header.begin() + 72, result.diskGuid.begin());
    result.entries.swap(entries);
    result.status = PartialTableStatus::Valid;
    return result;
}

} // namespace

void readPartialPartitionTables(const ByteStorePtr& disk, std::uint32_t sectorSize,
                                PartialMbrTable* mbr, PartialGptTable* gpt) {
    try {
        if (mbr) *mbr = readMbr(disk, sectorSize);
        if (gpt) *gpt = readGpt(disk, sectorSize);
    } catch (const std::bad_alloc&) {
        if (mbr) { *mbr = PartialMbrTable(); mbr->status = PartialTableStatus::IoError; }
        if (gpt) { *gpt = PartialGptTable(); gpt->status = PartialTableStatus::IoError; }
    } catch (const std::length_error&) {
        if (mbr) { *mbr = PartialMbrTable(); mbr->status = PartialTableStatus::Invalid; }
        if (gpt) { *gpt = PartialGptTable(); gpt->status = PartialTableStatus::Invalid; }
    }
}

} // namespace fs
} // namespace peare
