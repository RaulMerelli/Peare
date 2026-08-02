#include "DynamicDisk.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSector = 512;
const std::int64_t kPrivateHeaderOffset = 0xc00;

std::uint16_t be16(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t(be32(p)) << 32) | std::uint64_t(be32(p + 4));
}

std::string fixedString(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && p[i] != 0; ++i)
        s.push_back(char(p[i]));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct PrivateHeader {
    std::int64_t dataStartLba = 0;
    std::int64_t configStartLba = 0;
    std::int64_t tocSizeLba = 0;
};

struct TocBlock {
    std::int64_t configStart = 0;
};

struct VolumeRecord {
    std::uint64_t id = 0;
    std::string name;
    std::uint64_t componentCount = 0;
    std::uint64_t sizeSectors = 0;
};

struct ComponentRecord {
    std::uint64_t id = 0;
    std::uint8_t mergeType = 0;
    std::uint32_t flags = 0;
    std::uint64_t numExtents = 0;
    std::int64_t stripeSizeSectors = 0;
    std::int64_t stripeStride = 0;
    std::uint64_t volumeId = 0;
};

struct ExtentRecord {
    std::uint64_t componentId = 0;
    std::int64_t diskOffsetLba = 0;
    std::int64_t offsetInVolumeLba = 0;
    std::int64_t sizeLba = 0;
    std::uint64_t interleaveOrder = 0;
};

std::uint64_t readVarULong(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    if (*pos >= b.size()) return 0;
    const std::uint8_t n = b[(*pos)++];
    std::uint64_t v = 0;
    for (std::uint8_t i = 0; i < n && *pos < b.size(); ++i)
        v = (v << 8) | b[(*pos)++];
    return v;
}

std::int64_t readVarLong(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    return static_cast<std::int64_t>(readVarULong(b, pos));
}

std::string readVarString(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    if (*pos >= b.size()) return {};
    const std::uint8_t n = b[(*pos)++];
    if (*pos + n > b.size()) {
        *pos = b.size();
        return {};
    }
    std::string s(reinterpret_cast<const char*>(b.data() + *pos),
                  reinterpret_cast<const char*>(b.data() + *pos + n));
    *pos += n;
    return s;
}

std::uint8_t readByte(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    return *pos < b.size() ? b[(*pos)++] : 0;
}

std::uint32_t readUInt(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    if (*pos + 4 > b.size()) {
        *pos = b.size();
        return 0;
    }
    const std::uint32_t v = be32(b.data() + *pos);
    *pos += 4;
    return v;
}

std::uint64_t readULong(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    if (*pos + 8 > b.size()) {
        *pos = b.size();
        return 0;
    }
    const std::uint64_t v = be64(b.data() + *pos);
    *pos += 8;
    return v;
}

std::int64_t readLong(const std::vector<std::uint8_t>& b, std::size_t* pos) {
    return static_cast<std::int64_t>(readULong(b, pos));
}

bool readPrivateHeader(const ByteStorePtr& disk, PrivateHeader* out) {
    std::vector<std::uint8_t> b = disk ? disk->readRange(kPrivateHeaderOffset, kSector)
                                       : std::vector<std::uint8_t>();
    if (b.size() < kSector || std::memcmp(b.data(), "PRIVHEAD", 8) != 0)
        return false;
    PrivateHeader h;
    h.dataStartLba = static_cast<std::int64_t>(be64(b.data() + 0x11b));
    h.configStartLba = static_cast<std::int64_t>(be64(b.data() + 0x12b));
    h.tocSizeLba = static_cast<std::int64_t>(be64(b.data() + 0x13b));
    if (h.dataStartLba < 0 || h.configStartLba <= 0 || h.tocSizeLba <= 0)
        return false;
    if (out) *out = h;
    return true;
}

bool readToc(const ByteStorePtr& disk, const PrivateHeader& h, TocBlock* out) {
    const std::int64_t tocBytes = h.tocSizeLba * kSector;
    const std::int64_t pos = h.configStartLba * kSector + h.tocSizeLba * kSector;
    std::vector<std::uint8_t> b = disk->readRange(pos, tocBytes);
    if (b.size() < 0x68 || std::memcmp(b.data(), "TOCBLOCK", 8) != 0)
        return false;
    TocBlock toc;
    const std::string item1 = fixedString(b.data() + 0x24, 10);
    const std::string item2 = fixedString(b.data() + 0x46, 10);
    if (item1 == "config")
        toc.configStart = static_cast<std::int64_t>(be64(b.data() + 0x2e));
    else if (item2 == "config")
        toc.configStart = static_cast<std::int64_t>(be64(b.data() + 0x50));
    else
        return false;
    if (toc.configStart < 0)
        return false;
    if (out) *out = toc;
    return true;
}

bool parseDatabase(const ByteStorePtr& disk, std::int64_t dbStart,
                   std::map<std::uint64_t, VolumeRecord>* volumes,
                   std::map<std::uint64_t, ComponentRecord>* components,
                   std::vector<ExtentRecord>* extents,
                   std::string* error) {
    std::vector<std::uint8_t> hdr = disk->readRange(dbStart, kSector);
    if (hdr.size() < kSector || std::memcmp(hdr.data(), "VMDB", 4) != 0) {
        if (error) *error = "LDM VMDB header not found";
        return false;
    }
    const std::uint32_t num = be32(hdr.data() + 0x04);
    const std::uint32_t blockSize = be32(hdr.data() + 0x08);
    const std::uint32_t headerSize = be32(hdr.data() + 0x0c);
    if (num == 0 || num > 65536 || blockSize < 0x18 || blockSize > 4096 || headerSize < kSector) {
        if (error) *error = "Invalid LDM VMDB header";
        return false;
    }
    std::vector<std::uint8_t> table =
        disk->readRange(dbStart + headerSize, std::int64_t(num) * blockSize);
    for (std::uint32_t i = 0; i < num; ++i) {
        const std::size_t off = std::size_t(i) * blockSize;
        if (off + blockSize > table.size()) break;
        std::vector<std::uint8_t> b(table.begin() + std::ptrdiff_t(off),
                                    table.begin() + std::ptrdiff_t(off + blockSize));
        if (std::memcmp(b.data(), "VBLK", 4) != 0 || be32(b.data() + 0x0c) == 0)
            continue;
        const std::uint8_t type = b[0x13] & 0x0f;
        const std::uint32_t flags = be32(b.data() + 0x10);
        std::size_t pos = 0x18;
        if (type == 1) {
            VolumeRecord v;
            v.id = readVarULong(b, &pos);
            v.name = readVarString(b, &pos);
            readVarString(b, &pos);
            readVarString(b, &pos);
            pos += 6;
            readVarULong(b, &pos);
            readULong(b, &pos);
            readVarULong(b, &pos);
            readUInt(b, &pos);
            v.componentCount = readVarULong(b, &pos);
            readUInt(b, &pos);
            readUInt(b, &pos);
            readULong(b, &pos);
            v.sizeSectors = std::uint64_t(readVarLong(b, &pos));
            if (v.id != 0)
                (*volumes)[v.id] = v;
        } else if (type == 2) {
            ComponentRecord c;
            c.flags = flags;
            c.id = readVarULong(b, &pos);
            readVarString(b, &pos);
            readVarString(b, &pos);
            c.mergeType = readByte(b, &pos);
            readUInt(b, &pos);
            c.numExtents = readVarULong(b, &pos);
            readUInt(b, &pos);
            readUInt(b, &pos);
            readULong(b, &pos);
            c.volumeId = readVarULong(b, &pos);
            readVarULong(b, &pos);
            if ((flags & 0x1000U) != 0) {
                c.stripeSizeSectors = readVarLong(b, &pos);
                c.stripeStride = readVarLong(b, &pos);
            }
            if (c.id != 0)
                (*components)[c.id] = c;
        } else if (type == 3) {
            ExtentRecord e;
            e.componentId = 0;
            readVarULong(b, &pos);
            readVarString(b, &pos);
            readUInt(b, &pos);
            readUInt(b, &pos);
            readUInt(b, &pos);
            e.diskOffsetLba = readLong(b, &pos);
            e.offsetInVolumeLba = readLong(b, &pos);
            e.sizeLba = readVarLong(b, &pos);
            e.componentId = readVarULong(b, &pos);
            readVarULong(b, &pos);
            if ((flags & 0x0800U) != 0)
                e.interleaveOrder = readVarULong(b, &pos);
            if (e.sizeLba > 0)
                extents->push_back(e);
        }
    }
    return true;
}

}  // namespace

bool hasDynamicDiskMetadata(const ByteStorePtr& disk) {
    return readPrivateHeader(disk, nullptr);
}

std::vector<DynamicVolumeInfo> readDynamicDiskVolumes(const ByteStorePtr& disk, std::string* error) {
    std::vector<DynamicVolumeInfo> out;
    PrivateHeader header;
    if (!readPrivateHeader(disk, &header)) {
        if (error) *error = "LDM private header not found";
        return out;
    }
    TocBlock toc;
    if (!readToc(disk, header, &toc)) {
        if (error) *error = "LDM table of contents not found";
        return out;
    }

    std::map<std::uint64_t, VolumeRecord> volumes;
    std::map<std::uint64_t, ComponentRecord> components;
    std::vector<ExtentRecord> extents;
    const std::int64_t dbStart = header.configStartLba * kSector + toc.configStart * kSector;
    if (!parseDatabase(disk, dbStart, &volumes, &components, &extents, error))
        return out;

    for (const auto& vp : volumes) {
        const VolumeRecord& volume = vp.second;
        std::vector<ByteStorePtr> componentStores;
        std::int64_t next = 0;
        bool ok = true;
        for (const auto& cp : components) {
            const ComponentRecord& component = cp.second;
            if (component.volumeId != volume.id)
                continue;
            std::vector<ExtentRecord> componentExtents;
            for (const ExtentRecord& e : extents)
                if (e.componentId == component.id)
                    componentExtents.push_back(e);
            if (component.mergeType == 2) {
                std::sort(componentExtents.begin(), componentExtents.end(),
                          [](const ExtentRecord& a, const ExtentRecord& b) {
                              return a.offsetInVolumeLba < b.offsetInVolumeLba;
                          });
                std::vector<ByteStorePtr> extentStores;
                for (const ExtentRecord& e : componentExtents) {
                    if (e.offsetInVolumeLba != next) {
                        ok = false;
                        break;
                    }
                    const std::int64_t start = (header.dataStartLba + e.diskOffsetLba) * kSector;
                    const std::int64_t len = e.sizeLba * kSector;
                    if (start < 0 || len <= 0 || start >= disk->capacity() ||
                        len > disk->capacity() - start) {
                        ok = false;
                        break;
                    }
                    extentStores.push_back(std::make_shared<SubStore>(disk, start, len));
                    next += e.sizeLba;
                }
                if (!ok || extentStores.empty())
                    break;
                componentStores.push_back(std::make_shared<ConcatStore>(std::move(extentStores)));
            } else if (component.mergeType == 1) {
                if (component.stripeSizeSectors <= 0 || componentExtents.empty()) {
                    ok = false;
                    break;
                }
                std::sort(componentExtents.begin(), componentExtents.end(),
                          [](const ExtentRecord& a, const ExtentRecord& b) {
                              return a.interleaveOrder < b.interleaveOrder;
                          });
                std::vector<ByteStorePtr> stripeStores;
                std::int64_t stripeCapacity = 0;
                for (const ExtentRecord& e : componentExtents) {
                    const std::int64_t start = (header.dataStartLba + e.diskOffsetLba) * kSector;
                    const std::int64_t len = e.sizeLba * kSector;
                    if (start < 0 || len <= 0 || start >= disk->capacity() ||
                        len > disk->capacity() - start) {
                        ok = false;
                        break;
                    }
                    stripeStores.push_back(std::make_shared<SubStore>(disk, start, len));
                    stripeCapacity += e.sizeLba;
                }
                if (!ok || stripeStores.empty())
                    break;
                componentStores.push_back(std::make_shared<StripedStore>(
                    component.stripeSizeSectors * kSector, std::move(stripeStores)));
                next += stripeCapacity;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok || componentStores.empty())
            continue;
        DynamicVolumeInfo info;
        info.name = volume.name.empty() ? "Dynamic volume" : volume.name;
        info.sizeSectors = volume.sizeSectors ? volume.sizeSectors : std::uint64_t(next);
        // Multiple healthy LDM components are mirrors in DiscUtils; any one
        // component provides the full logical volume for read-only access.
        info.content = componentStores[0];
        out.push_back(std::move(info));
    }
    if (out.empty() && error && error->empty())
        *error = "No supported LDM volumes found";
    return out;
}

}  // namespace fs
}  // namespace peare
