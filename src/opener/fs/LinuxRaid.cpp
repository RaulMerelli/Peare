#include "LinuxRaid.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSector = 512;
const std::uint32_t kLinuxRaidMagic = 0xa92b4efcU;

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

std::int64_t alignDown(std::int64_t value, std::int64_t alignment) {
    if (value <= 0) return 0;
    return (value / alignment) * alignment;
}

std::string zString(const std::uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n && p[i] != 0; ++i)
        s.push_back(char(p[i]));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool parseSuperblock(const std::vector<std::uint8_t>& b, std::uint32_t minor,
                     LinuxRaidSuperblock* out) {
    if (b.size() < 512 || le32(b.data()) != kLinuxRaidMagic)
        return false;

    LinuxRaidSuperblock sb;
    sb.valid = true;
    sb.minorVersion = minor;
    if (minor == 9) {
        sb.majorVersion = le32(b.data() + 4);
        sb.minorVersion = le32(b.data() + 8);
        if (sb.majorVersion != 0 || sb.minorVersion != 9)
            return false;
        sb.raidLevel = le32(b.data() + 4 * 7);
        sb.dataOffsetSectors = 0;
        // Version 0.90 stores the member size in KiB. Expose the same data range
        // as sectors for the common ByteStore layer.
        sb.arraySizeSectors = std::uint64_t(le32(b.data() + 4 * 8)) * 2ULL;
        sb.totalDisks = le32(b.data() + 4 * 9);
        sb.arrayName = "raid";
    } else {
        sb.majorVersion = le32(b.data() + 4);
        if (sb.majorVersion != 1)
            return false;
        sb.raidLevel = le32(b.data() + 0x48);
        sb.arrayName = zString(b.data() + 0x20, 32);
        sb.dataOffsetSectors = le64(b.data() + 0x80);
        sb.arraySizeSectors = le64(b.data() + 0x88);
        sb.totalDisks = le32(b.data() + 0x5c);
    }
    if (sb.arrayName.empty())
        sb.arrayName = "raid";
    if (sb.arraySizeSectors == 0)
        return false;
    if (out) *out = sb;
    return true;
}

}  // namespace

bool readLinuxRaidSuperblock(const ByteStorePtr& store, LinuxRaidSuperblock* out) {
    if (!store || store->capacity() < 512)
        return false;
    const std::int64_t size = store->capacity();
    struct Candidate {
        std::uint32_t minor;
        std::int64_t offset;
    };
    const Candidate candidates[] = {
        {1, 0},
        {2, 4096},
        {0, alignDown(size - 8192, 65536)},
        {9, alignDown(size - 65536, kSector)}
    };

    for (const Candidate& c : candidates) {
        if (c.offset < 0 || c.offset >= size)
            continue;
        const std::vector<std::uint8_t> b = store->readRange(c.offset, 4096);
        LinuxRaidSuperblock sb;
        if (!parseSuperblock(b, c.minor, &sb))
            continue;
        const std::uint64_t start = sb.dataOffsetSectors * std::uint64_t(kSector);
        const std::uint64_t bytes = sb.arraySizeSectors * std::uint64_t(kSector);
        if (start > std::uint64_t(size) || bytes == 0 || bytes > std::uint64_t(size) - start)
            continue;
        if (out) *out = sb;
        return true;
    }
    return false;
}

bool hasLinuxRaidSuperblock(const ByteStorePtr& store) {
    return readLinuxRaidSuperblock(store, nullptr);
}

}  // namespace fs
}  // namespace peare
