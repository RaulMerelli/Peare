// Windows CE container, XIP and IMGFS parsing is a clean C++11 adaptation of
// the MIT-licensed CERF parser by Yaroslav Kibysh (see third_party/cerf/LICENSE).
#include "WinceRomModule.h"
#include "WinceCompression.h"
#include "Compat.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::uint64_t kMaxMaterialisedImage = 512ULL * 1024ULL * 1024ULL;
const std::uint32_t kRomHdrSize = 84;
const std::uint32_t kTocEntrySize = 32;
const std::uint32_t kFileEntrySize = 28;
const std::uint32_t kO32Size = 24;
const std::uint32_t kSectionAlignment = 0x1000;

std::uint16_t u16(const std::vector<std::uint8_t>& data, std::uint64_t off)
{
    if (off + 2 > data.size()) return 0;
    return std::uint16_t(data[std::size_t(off)]) |
           (std::uint16_t(data[std::size_t(off + 1)]) << 8);
}

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::uint64_t off)
{
    if (off + 4 > data.size()) return 0;
    return std::uint32_t(data[std::size_t(off)]) |
           (std::uint32_t(data[std::size_t(off + 1)]) << 8) |
           (std::uint32_t(data[std::size_t(off + 2)]) << 16) |
           (std::uint32_t(data[std::size_t(off + 3)]) << 24);
}

std::uint16_t qU16(const QByteArray& data, int off)
{
    if (off < 0 || off + 2 > data.size()) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t qU32(const QByteArray& data, int off)
{
    if (off < 0 || off + 4 > data.size()) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void put16(QByteArray& data, std::uint64_t off, std::uint16_t value)
{
    if (off + 2 > std::uint64_t(data.size())) return;
    data[int(off)] = char(value & 0xff);
    data[int(off + 1)] = char((value >> 8) & 0xff);
}

void put32(QByteArray& data, std::uint64_t off, std::uint32_t value)
{
    if (off + 4 > std::uint64_t(data.size())) return;
    data[int(off)] = char(value & 0xff);
    data[int(off + 1)] = char((value >> 8) & 0xff);
    data[int(off + 2)] = char((value >> 16) & 0xff);
    data[int(off + 3)] = char((value >> 24) & 0xff);
}

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment)
{
    if (!alignment) return value;
    const std::uint64_t result = (std::uint64_t(value) + alignment - 1) & ~(std::uint64_t(alignment) - 1);
    return result > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max() : std::uint32_t(result);
}

QString readAscii(const std::vector<std::uint8_t>& data, std::int64_t off, int maxLength = 512)
{
    if (off < 0 || std::uint64_t(off) >= data.size()) return QString();
    QByteArray value;
    const std::size_t start = std::size_t(off);
    const std::size_t end = std::min(data.size(), start + std::size_t(maxLength));
    for (std::size_t i = start; i < end && data[i] != 0; ++i) {
        const unsigned char c = data[i];
        if (c < 0x20 || c > 0x7e) return QString();
        value.append(char(c));
    }
    return QString::fromLatin1(value);
}

QString safeName(QString name, const QString& fallback)
{
    if (name.isEmpty()) name = fallback;
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < name.size(); ++i) {
        if (name.at(i).unicode() < 0x20 || forbidden.contains(name.at(i)))
            name[i] = QLatin1Char('_');
    }
    while (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.')))
        name.chop(1);
    return name.isEmpty() ? fallback : name;
}

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return fs::ByteStorePtr();
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    }
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

struct BinRecord {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint64_t dataOffset = 0;
};

int b000ffSignatureLength(const fs::IByteStore& file)
{
    std::uint8_t head[16] = {};
    const int got = file.read(0, head, int(sizeof(head)));
    if (got >= 8 && std::memcmp(head, "B000FF\r\n", 8) == 0) return 8;
    if (got >= 7 && std::memcmp(head, "B000FF\n", 7) == 0) return 7;
    if (got >= 14 && std::memcmp(head, "B000FF", 6) == 0) {
        const std::uint32_t start = std::uint32_t(head[6]) |
            (std::uint32_t(head[7]) << 8) | (std::uint32_t(head[8]) << 16) |
            (std::uint32_t(head[9]) << 24);
        const std::uint32_t length = std::uint32_t(head[10]) |
            (std::uint32_t(head[11]) << 8) | (std::uint32_t(head[12]) << 16) |
            (std::uint32_t(head[13]) << 24);
        if (start && length) return 6;
    }
    return 0;
}

bool readStoreExact(const fs::IByteStore& store, std::int64_t offset,
                    std::uint8_t* dst, int count)
{
    if (offset < 0 || count < 0 || offset > store.capacity() - count) return false;
    int done = 0;
    while (done < count) {
        const int got = store.read(offset + done, dst + done, count - done);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}

std::uint16_t storeU16(const fs::IByteStore& store, std::uint64_t off)
{
    std::uint8_t bytes[2];
    if (off > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
        !readStoreExact(store, std::int64_t(off), bytes, 2)) return 0;
    return std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8);
}

std::uint32_t storeU32(const fs::IByteStore& store, std::uint64_t off)
{
    std::uint8_t bytes[4];
    if (off > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
        !readStoreExact(store, std::int64_t(off), bytes, 4)) return 0;
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

bool parseB000ffRecords(const fs::ByteStorePtr& source,
                        std::vector<BinRecord>* records,
                        std::uint32_t* baseAddress,
                        std::uint64_t* endAddress)
{
    if (!source || !records || !baseAddress || !endAddress) return false;
    const int signature = b000ffSignatureLength(*source);
    if (!signature || source->capacity() < signature + 8 + 12) return false;

    std::uint64_t pos = std::uint64_t(signature + 8);
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0;
    records->clear();
    while (pos + 12 <= std::uint64_t(source->capacity()) && records->size() < 1000000U) {
        std::uint8_t header[12];
        if (!readStoreExact(*source, std::int64_t(pos), header, 12)) break;
        BinRecord record;
        record.address = std::uint32_t(header[0]) | (std::uint32_t(header[1]) << 8) |
                         (std::uint32_t(header[2]) << 16) | (std::uint32_t(header[3]) << 24);
        record.size = std::uint32_t(header[4]) | (std::uint32_t(header[5]) << 8) |
                      (std::uint32_t(header[6]) << 16) | (std::uint32_t(header[7]) << 24);
        record.dataOffset = pos + 12;
        if (!record.address) break;
        const std::uint64_t recordEnd = std::uint64_t(record.address) + record.size;
        if (!record.size || recordEnd > 0x100000000ULL ||
            record.dataOffset > std::uint64_t(source->capacity()) - record.size)
            return false;
        minimum = std::min(minimum, std::uint64_t(record.address));
        maximum = std::max(maximum, recordEnd);
        records->push_back(record);
        pos = record.dataOffset + record.size;
    }
    if (records->empty() || maximum <= minimum || minimum > 0xffffffffULL) return false;
    *baseAddress = std::uint32_t(minimum);
    *endAddress = maximum;
    return true;
}

// Sparse logical view of a B000FF stream. A read touches only records that
// overlap the requested range; holes are returned as zero and later records
// override earlier records, matching the flashing semantics and the old
// materialising parser.
class B000ffStore final : public fs::IByteStore {
public:
    B000ffStore(fs::ByteStorePtr source, std::vector<BinRecord> records,
                std::uint32_t base, std::uint64_t end)
        : source_(std::move(source)), records_(std::move(records)), base_(base), end_(end) {}

    std::int64_t capacity() const override
    {
        const std::uint64_t span = end_ - base_;
        return span <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(span) : 0;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (!source_ || !dst || pos < 0 || count <= 0 || pos >= capacity()) return 0;
        const int wanted = int(std::min<std::int64_t>(count, capacity() - pos));
        std::fill(dst, dst + wanted, 0);
        const std::uint64_t requestStart = std::uint64_t(pos);
        const std::uint64_t requestEnd = requestStart + wanted;
        for (const BinRecord& record : records_) {
            const std::uint64_t recordStart = std::uint64_t(record.address) - base_;
            const std::uint64_t recordEnd = recordStart + record.size;
            const std::uint64_t overlapStart = std::max(requestStart, recordStart);
            const std::uint64_t overlapEnd = std::min(requestEnd, recordEnd);
            if (overlapEnd <= overlapStart) continue;
            const int length = int(overlapEnd - overlapStart);
            int done = 0;
            while (done < length) {
                const int got = source_->read(
                    std::int64_t(record.dataOffset + overlapStart - recordStart + done),
                    dst + int(overlapStart - requestStart) + done, length - done);
                if (got <= 0) break;
                done += got;
            }
        }
        return wanted;
    }

    std::uint32_t baseAddress() const { return base_; }
    const std::vector<BinRecord>& records() const { return records_; }

private:
    fs::ByteStorePtr source_;
    std::vector<BinRecord> records_;
    std::uint32_t base_;
    std::uint64_t end_;
};

struct RomHdr {
    std::uint32_t dllFirst = 0;
    std::uint32_t dllLast = 0;
    std::uint32_t physFirst = 0;
    std::uint32_t physLast = 0;
    std::uint32_t numMods = 0;
    std::uint32_t numFiles = 0;
    std::uint16_t cpuType = 0;
};

bool parseRomHdr(const std::vector<std::uint8_t>& data, std::uint64_t off, RomHdr* out)
{
    if (!out || off + kRomHdrSize > data.size()) return false;
    RomHdr h;
    h.dllFirst = u32(data, off + 0);
    h.dllLast = u32(data, off + 4);
    h.physFirst = u32(data, off + 8);
    h.physLast = u32(data, off + 12);
    h.numMods = u32(data, off + 16);
    h.numFiles = u32(data, off + 48);
    h.cpuType = u16(data, off + 68);
    if (h.dllFirst > h.dllLast || h.physFirst > h.physLast) return false;
    if (h.numMods > 4096 || h.numFiles > 50000) return false;
    if (h.numMods == 0 && h.numFiles == 0) return false;
    *out = h;
    return true;
}

bool parseRomHdr(const fs::IByteStore& data, std::uint64_t off, RomHdr* out)
{
    if (!out || off + kRomHdrSize > std::uint64_t(data.capacity())) return false;
    RomHdr h;
    h.dllFirst = storeU32(data, off + 0);
    h.dllLast = storeU32(data, off + 4);
    h.physFirst = storeU32(data, off + 8);
    h.physLast = storeU32(data, off + 12);
    h.numMods = storeU32(data, off + 16);
    h.numFiles = storeU32(data, off + 48);
    h.cpuType = storeU16(data, off + 68);
    if (h.dllFirst > h.dllLast || h.physFirst > h.physLast) return false;
    if (h.numMods > 4096 || h.numFiles > 50000) return false;
    if (h.numMods == 0 && h.numFiles == 0) return false;
    *out = h;
    return true;
}

struct Region {
    std::uint64_t ececOffset = 0;
    std::uint32_t ptoc = 0;
    std::uint64_t romHdrOffset = 0;
    std::uint32_t loadOffset = 0;
    RomHdr header;
};

void appendCandidate(std::vector<std::pair<std::uint64_t, std::uint32_t> >& candidates,
                     std::int64_t romOff, std::uint32_t load)
{
    if (romOff < 0) return;
    const std::pair<std::uint64_t, std::uint32_t> candidate(std::uint64_t(romOff), load);
    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        candidates.push_back(candidate);
}

bool resolveAt(const std::vector<std::uint8_t>& data, std::uint64_t ecec,
               std::uint32_t ptoc, std::uint32_t field, std::uint32_t baseOffset,
               Region* out)
{
    std::vector<std::pair<std::uint64_t, std::uint32_t> > candidates;
    if (field != 0 && field < 0x10000000U) {
        const std::uint64_t xipBase = ecec >= 0x40 ? ecec - 0x40 : 0;
        const std::uint64_t romOff = xipBase + field;
        if (romOff <= std::numeric_limits<std::uint32_t>::max())
            appendCandidate(candidates, std::int64_t(romOff), ptoc - std::uint32_t(romOff));
    } else {
        appendCandidate(candidates, std::int64_t(ptoc) - baseOffset, baseOffset);
        const std::uint32_t mirror = baseOffset | 0x80000000U;
        if (mirror != baseOffset)
            appendCandidate(candidates, std::int64_t(ptoc) - mirror, mirror);
        const std::uint32_t masks[] = {0xFF000000U, 0xF0000000U};
        for (std::uint32_t mask : masks) {
            const std::uint32_t load = ptoc & mask;
            appendCandidate(candidates, std::int64_t(ptoc) - load, load);
        }
    }

    for (const auto& candidate : candidates) {
        RomHdr hdr;
        if (!parseRomHdr(data, candidate.first, &hdr)) continue;
        out->ececOffset = ecec;
        out->ptoc = ptoc;
        out->romHdrOffset = candidate.first;
        out->loadOffset = candidate.second;
        out->header = hdr;
        return true;
    }

    if (field == 0) {
        for (std::uint64_t off = 0; off + kRomHdrSize <= data.size(); off += 4) {
            if (std::uint32_t(u32(data, off + 8) + std::uint32_t(off)) != ptoc) continue;
            RomHdr hdr;
            if (!parseRomHdr(data, off, &hdr)) continue;
            out->ececOffset = ecec;
            out->ptoc = ptoc;
            out->romHdrOffset = off;
            out->loadOffset = hdr.physFirst;
            out->header = hdr;
            return true;
        }
    }
    return false;
}

bool resolveAt(const fs::IByteStore& data, std::uint64_t ecec,
               std::uint32_t ptoc, std::uint32_t field, std::uint32_t baseOffset,
               Region* out)
{
    if (!out) return false;
    std::vector<std::pair<std::uint64_t, std::uint32_t> > candidates;
    if (field != 0 && field < 0x10000000U) {
        const std::uint64_t xipBase = ecec >= 0x40 ? ecec - 0x40 : 0;
        const std::uint64_t romOff = xipBase + field;
        if (romOff <= std::numeric_limits<std::uint32_t>::max())
            appendCandidate(candidates, std::int64_t(romOff), ptoc - std::uint32_t(romOff));
    } else {
        appendCandidate(candidates, std::int64_t(ptoc) - baseOffset, baseOffset);
        const std::uint32_t mirror = baseOffset | 0x80000000U;
        if (mirror != baseOffset)
            appendCandidate(candidates, std::int64_t(ptoc) - mirror, mirror);
        const std::uint32_t masks[] = {0xFF000000U, 0xF0000000U};
        for (std::uint32_t mask : masks) {
            const std::uint32_t load = ptoc & mask;
            appendCandidate(candidates, std::int64_t(ptoc) - load, load);
        }
    }

    for (const auto& candidate : candidates) {
        RomHdr hdr;
        if (!parseRomHdr(data, candidate.first, &hdr)) continue;
        out->ececOffset = ecec;
        out->ptoc = ptoc;
        out->romHdrOffset = candidate.first;
        out->loadOffset = candidate.second;
        out->header = hdr;
        return true;
    }
    return false;
}

std::vector<Region> findB000ffRegions(const B000ffStore& logical)
{
    std::vector<Region> regions;
    std::set<std::uint64_t> romHeaders;
    const std::size_t chunkSize = 1024U * 1024U;
    const std::uint64_t quickProbe = 128U * 1024U;
    std::vector<std::uint8_t> buffer(chunkSize + 11);

    const auto scanRecord = [&](const BinRecord& record, std::uint64_t first,
                                std::uint64_t length) {
        std::uint64_t cursor = first;
        const std::uint64_t limit = std::min<std::uint64_t>(record.size, first + length);
        while (cursor < limit) {
            const std::uint64_t overlap = cursor > first
                ? std::min<std::uint64_t>(11, cursor - first) : 0;
            const std::uint64_t startInRecord = cursor - overlap;
            const int request = int(std::min<std::uint64_t>(buffer.size(), limit - startInRecord));
            if (!readStoreExact(logical, std::int64_t(std::uint64_t(record.address) -
                                      logical.baseAddress() + startInRecord),
                                buffer.data(), request))
                break;
            const int scanStart = cursor > first ? int(overlap) - 3 : 0;
            for (int i = std::max(0, scanStart); i + 12 <= request; ++i) {
                if (std::memcmp(buffer.data() + i, "ECEC", 4) != 0) continue;
                const std::uint64_t ecec = std::uint64_t(record.address) -
                    logical.baseAddress() + startInRecord + std::uint64_t(i);
                const std::uint32_t ptoc = std::uint32_t(buffer[i + 4]) |
                    (std::uint32_t(buffer[i + 5]) << 8) |
                    (std::uint32_t(buffer[i + 6]) << 16) |
                    (std::uint32_t(buffer[i + 7]) << 24);
                if (ptoc == 0) continue;
                std::uint32_t field = std::uint32_t(buffer[i + 8]) |
                    (std::uint32_t(buffer[i + 9]) << 8) |
                    (std::uint32_t(buffer[i + 10]) << 16) |
                    (std::uint32_t(buffer[i + 11]) << 24);
                if (field >= 0x10000000U) field = 0;
                Region region;
                if (resolveAt(logical, ecec, ptoc, field, logical.baseAddress(), &region) &&
                    romHeaders.insert(region.romHdrOffset).second)
                    regions.push_back(region);
            }
            cursor += std::min<std::uint64_t>(chunkSize, limit - cursor);
        }
    };

    // Standard B000FF records place the ECEC launch header near the beginning.
    // Probe every record there first, so a multi-gigabyte image opens in time
    // proportional to its record count rather than its payload size.
    for (const BinRecord& record : logical.records())
        scanRecord(record, 0, std::min<std::uint64_t>(record.size, quickProbe));
    if (!regions.empty()) return regions;

    // Unusual OEM layouts may place ECEC deeper in a record. Preserve support
    // with a full streaming fallback only when the constant-cost probe failed.
    for (const BinRecord& record : logical.records())
        if (record.size > quickProbe)
            scanRecord(record, quickProbe - 11, std::uint64_t(record.size) - quickProbe + 11);
    return regions;
}

std::vector<Region> findRegions(const std::vector<std::uint8_t>& data,
                                std::uint32_t baseOffset)
{
    std::vector<Region> regions;
    std::set<std::uint64_t> romHeaders;
    for (std::uint64_t pos = 0; pos + 12 <= data.size(); ++pos) {
        if (data[std::size_t(pos)] != 'E' || data[std::size_t(pos + 1)] != 'C' ||
            data[std::size_t(pos + 2)] != 'E' || data[std::size_t(pos + 3)] != 'C')
            continue;
        const std::uint32_t ptoc = u32(data, pos + 4);
        if (ptoc == 0) continue;
        std::uint32_t field = u32(data, pos + 8);
        if (field >= 0x10000000U) field = 0;
        Region region;
        if (resolveAt(data, pos, ptoc, field, baseOffset, &region) &&
            romHeaders.insert(region.romHdrOffset).second)
            regions.push_back(region);
        pos += 3;
    }
    return regions;
}

QString cpuName(std::uint16_t cpu)
{
    switch (cpu) {
    case 0x014c: return QStringLiteral("x86");
    case 0x0166: return QStringLiteral("MIPS");
    case 0x01a2: return QStringLiteral("SH3");
    case 0x01a6: return QStringLiteral("SH4");
    case 0x01c0: return QStringLiteral("ARM");
    case 0x01c2: return QStringLiteral("Thumb");
    case 0x01f0: return QStringLiteral("PowerPC");
    default: return QStringLiteral("CPU 0x%1").arg(cpu, 4, 16, QLatin1Char('0'));
    }
}

struct E32Info {
    std::uint16_t objectCount = 0;
    std::uint16_t imageFlags = 0;
    std::uint32_t entryRva = 0;
    std::uint32_t imageBase = 0;
    std::uint16_t subsystemMajor = 0;
    std::uint16_t subsystemMinor = 0;
    std::uint32_t stackMax = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t timestamp = 0;
    std::array<std::pair<std::uint32_t, std::uint32_t>, 9> directories;
    std::uint16_t subsystem = 9;
    std::uint32_t section14Rva = 0;
    std::uint32_t section14Size = 0;
    bool ce20 = false;
};

bool parseE32Base(const std::vector<std::uint8_t>& data, std::int64_t off,
                  std::uint32_t ddOffset, E32Info* out)
{
    if (!out || off < 0) return false;
    const std::uint64_t uoff = std::uint64_t(off);
    if (uoff + ddOffset + 74 > data.size()) return false;

    E32Info info;
    info.objectCount = u16(data, uoff);
    info.imageFlags = u16(data, uoff + 2);
    info.entryRva = u32(data, uoff + 4);
    info.imageBase = u32(data, uoff + 8);
    info.subsystemMajor = u16(data, uoff + 0x0c);
    info.subsystemMinor = u16(data, uoff + 0x0e);
    info.stackMax = u32(data, uoff + 0x10);

    if (ddOffset == 0x18) {
        info.virtualSize = u32(data, uoff + 0x10);
        info.stackMax = 0;
        info.subsystem = u16(data, uoff + 0x14);
        info.ce20 = true;
    } else if (ddOffset == 0x1c) {
        info.virtualSize = u32(data, uoff + 0x14);
        info.subsystem = u16(data, uoff + 0x18);
    } else {
        info.virtualSize = u32(data, uoff + 0x14);
        info.section14Rva = u32(data, uoff + 0x18);
        info.section14Size = u32(data, uoff + 0x1c);
        info.timestamp = ddOffset == 0x24 ? u32(data, uoff + 0x20) : 0;
        info.subsystem = u16(data, uoff + ddOffset + 72);
    }

    for (std::size_t i = 0; i < info.directories.size(); ++i) {
        const std::uint64_t d = uoff + ddOffset + i * 8;
        info.directories[i] = std::make_pair(u32(data, d), u32(data, d + 4));
    }
    *out = info;
    return true;
}

bool validE32(const E32Info& info)
{
    if (info.objectCount == 0 || info.objectCount > 96) return false;
    const bool subsystemOk = info.ce20
        ? (info.subsystem == 1 || info.subsystem == 2 || info.subsystem == 3 ||
           info.subsystem == 4 || info.subsystem == 7 || info.subsystem == 9 ||
           info.subsystem == 10 || info.subsystem == 11)
        : (info.subsystem == 1 || info.subsystem == 2 || info.subsystem == 3 ||
           info.subsystem == 7 || info.subsystem == 9 || info.subsystem == 10 ||
           info.subsystem == 11);
    if (!subsystemOk || info.virtualSize == 0 || info.virtualSize > 0x10000000U)
        return false;
    for (const auto& dd : info.directories) {
        const std::uint32_t rva = dd.first;
        const std::uint32_t size = dd.second;
        if (rva && rva >= info.virtualSize) return false;
        if (size && size > info.virtualSize) return false;
        if (rva && size && std::uint64_t(rva) + size > std::uint64_t(info.virtualSize) + 0x1000)
            return false;
        if (rva && rva < 0x100) return false;
    }
    return true;
}

int scoreE32(const E32Info& info)
{
    int score = 0;
    for (const auto& dd : info.directories) {
        if (dd.first && (dd.first & 0xfff) == 0) score += 2;
        if (dd.first && dd.second) ++score;
    }
    return score;
}

bool parseE32Auto(const std::vector<std::uint8_t>& data, std::int64_t off, E32Info* out)
{
    const std::uint32_t layouts[] = {0x24, 0x20, 0x1c, 0x18};
    bool found = false;
    int bestScore = -1;
    E32Info best;
    for (std::uint32_t layout : layouts) {
        E32Info candidate;
        if (!parseE32Base(data, off, layout, &candidate) || !validE32(candidate)) continue;
        const int score = scoreE32(candidate);
        if (!found || score > bestScore) {
            found = true;
            bestScore = score;
            best = candidate;
        }
    }
    if (found && out) *out = best;
    return found;
}

struct O32Record {
    std::uint32_t virtualSize = 0;
    std::uint32_t rva = 0;
    std::uint32_t physicalSize = 0;
    std::uint32_t dataAddress = 0;
    std::uint32_t realAddress = 0;
    std::uint32_t flags = 0;
};

struct PeSection {
    QByteArray name;
    std::uint32_t virtualSize = 0;
    std::uint32_t rva = 0;
    std::uint32_t flags = 0;
    QByteArray data;
    std::uint32_t fileOffset = 0;
    std::uint32_t alignedRawSize = 0;
};

QByteArray sectionName(const O32Record& record,
                       const std::array<std::pair<std::uint32_t, std::uint32_t>, 9>& directories)
{
    for (std::size_t i = 0; i < directories.size(); ++i) {
        if (directories[i].first != record.rva || directories[i].second == 0 ||
            directories[i].second != record.virtualSize)
            continue;
        if (i == 2) return QByteArrayLiteral(".rsrc");
        if (i == 3) return QByteArrayLiteral(".pdata");
        if (i == 5) return QByteArrayLiteral(".reloc");
    }
    if (record.flags & 0x20) return QByteArrayLiteral(".text");
    if (record.flags & 0x80) return QByteArrayLiteral(".bss");
    if (record.flags & 0x40)
        return record.flags & 0x80000000U ? QByteArrayLiteral(".data") : QByteArrayLiteral(".rdata");
    return QByteArrayLiteral(".sec");
}

bool directoryCovered(const std::vector<PeSection>& sections, std::uint32_t rva)
{
    for (const PeSection& section : sections) {
        const std::uint64_t size = std::max<std::uint64_t>(section.virtualSize,
                                                           std::uint64_t(section.data.size()));
        if (section.rva <= rva && std::uint64_t(rva) < std::uint64_t(section.rva) + size)
            return true;
    }
    return false;
}

QByteArray buildPe(const E32Info& info,
                   std::array<std::pair<std::uint32_t, std::uint32_t>, 9> directories,
                   std::vector<PeSection> sections, std::uint16_t machine)
{
    std::sort(sections.begin(), sections.end(), [](const PeSection& a, const PeSection& b) {
        if (a.rva != b.rva) return a.rva < b.rva;
        return a.name < b.name;
    });
    if (sections.empty() || sections.size() > std::numeric_limits<std::uint16_t>::max())
        return QByteArray();

    const std::uint32_t fileAlignment = info.subsystemMajor >= 7 ? 0x1000 : 0x200;
    const std::uint32_t dosSize = 64;
    const std::uint32_t optionalSize = 224;
    const std::uint64_t rawHeaderSize = std::uint64_t(dosSize) + 4 + 20 + optionalSize + sections.size() * 40;
    if (rawHeaderSize > std::numeric_limits<std::uint32_t>::max()) return QByteArray();
    const std::uint32_t headerSize = alignUp(std::uint32_t(rawHeaderSize), fileAlignment);

    std::uint64_t fileOffset = headerSize;
    std::uint64_t sizeOfImageEnd = headerSize;
    std::uint64_t codeSize = 0;
    std::uint64_t initDataSize = 0;
    std::uint64_t uninitDataSize = 0;
    std::uint32_t baseOfCode = 0;
    std::uint32_t baseOfData = 0;
    for (PeSection& section : sections) {
        section.fileOffset = section.data.isEmpty() ? 0 : std::uint32_t(fileOffset);
        section.alignedRawSize = section.data.isEmpty() ? 0
            : alignUp(std::uint32_t(section.data.size()), fileAlignment);
        fileOffset += section.alignedRawSize;
        sizeOfImageEnd = std::max<std::uint64_t>(sizeOfImageEnd,
            std::uint64_t(section.rva) + std::max<std::uint64_t>(section.virtualSize,
                                                                 std::uint64_t(section.data.size())));
        if (section.flags & 0x20) {
            codeSize += section.alignedRawSize;
            if (!baseOfCode) baseOfCode = section.rva;
        }
        if (section.flags & 0x40) {
            initDataSize += section.alignedRawSize;
            if (!baseOfData) baseOfData = section.rva;
        }
        if (section.flags & 0x80) uninitDataSize += section.virtualSize;
    }
    if (!baseOfCode) baseOfCode = sections.front().rva;
    if (fileOffset > std::uint64_t(std::numeric_limits<int>::max())) return QByteArray();

    QByteArray pe(int(fileOffset), '\0');
    pe[0] = 'M'; pe[1] = 'Z';
    put32(pe, 0x3c, dosSize);
    std::uint64_t p = dosSize;
    pe[int(p)] = 'P'; pe[int(p + 1)] = 'E';
    p += 4;
    put16(pe, p + 0, machine);
    put16(pe, p + 2, std::uint16_t(sections.size()));
    put32(pe, p + 4, info.timestamp);
    put16(pe, p + 16, optionalSize);
    put16(pe, p + 18, std::uint16_t(info.imageFlags | 0x0002));
    p += 20;

    const std::uint64_t o = p;
    put16(pe, o + 0, 0x10b);
    put32(pe, o + 4, std::uint32_t(std::min<std::uint64_t>(codeSize, 0xffffffffULL)));
    put32(pe, o + 8, std::uint32_t(std::min<std::uint64_t>(initDataSize, 0xffffffffULL)));
    put32(pe, o + 12, std::uint32_t(std::min<std::uint64_t>(uninitDataSize, 0xffffffffULL)));
    put32(pe, o + 16, info.entryRva);
    put32(pe, o + 20, baseOfCode);
    put32(pe, o + 24, baseOfData);
    put32(pe, o + 28, info.imageBase);
    put32(pe, o + 32, kSectionAlignment);
    put32(pe, o + 36, fileAlignment);
    put16(pe, o + 40, info.subsystemMajor >= 7 ? info.subsystemMajor : 4);
    put16(pe, o + 42, info.subsystemMajor >= 7 ? info.subsystemMinor : 0);
    put16(pe, o + 48, info.subsystemMajor);
    put16(pe, o + 50, info.subsystemMinor);
    put32(pe, o + 56, alignUp(std::uint32_t(std::min<std::uint64_t>(sizeOfImageEnd, 0xffffffffULL)),
                              kSectionAlignment));
    put32(pe, o + 60, headerSize);
    put16(pe, o + 68, info.subsystem);
    put16(pe, o + 70, info.subsystemMajor >= 6 ? 0x0100 : 0);
    put32(pe, o + 72, info.stackMax);
    put32(pe, o + 76, 0x1000);
    put32(pe, o + 80, 0x100000);
    put32(pe, o + 84, 0x1000);
    put32(pe, o + 92, 16);

    static const int ceToPe[9] = {0, 1, 2, 3, 4, 5, 6, 12, 14};
    for (int i = 0; i < 9; ++i) {
        if (!directories[std::size_t(i)].first) continue;
        const std::uint64_t d = o + 96 + std::uint64_t(ceToPe[i]) * 8;
        put32(pe, d, directories[std::size_t(i)].first);
        put32(pe, d + 4, directories[std::size_t(i)].second);
    }
    if (info.section14Rva) {
        put32(pe, o + 96 + 14 * 8, info.section14Rva);
        put32(pe, o + 96 + 14 * 8 + 4, info.section14Size);
    }

    p += optionalSize;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const PeSection& section = sections[i];
        const std::uint64_t h = p + i * 40;
        const QByteArray name = section.name.left(8);
        for (int n = 0; n < name.size(); ++n) pe[int(h) + n] = name.at(n);
        put32(pe, h + 8, section.virtualSize);
        put32(pe, h + 12, section.rva);
        put32(pe, h + 16, section.alignedRawSize);
        put32(pe, h + 20, section.fileOffset);
        put32(pe, h + 36, section.flags & ~0x2002U);
        if (!section.data.isEmpty())
            std::copy(section.data.constBegin(), section.data.constEnd(),
                      pe.begin() + int(section.fileOffset));
    }
    return pe;
}

QByteArray reconstructPe(const std::vector<std::uint8_t>& flat, const Region& region,
                         std::uint32_t e32Va, std::uint32_t o32Va)
{
    const std::int64_t e32Off = std::int64_t(e32Va) - region.loadOffset;
    const std::int64_t o32Off = std::int64_t(o32Va) - region.loadOffset;
    E32Info info;
    if (!parseE32Auto(flat, e32Off, &info) || o32Off < 0 ||
        std::uint64_t(o32Off) + std::uint64_t(info.objectCount) * kO32Size > flat.size())
        return QByteArray();

    std::vector<O32Record> records;
    records.reserve(info.objectCount);
    for (std::uint16_t i = 0; i < info.objectCount; ++i) {
        const std::uint64_t off = std::uint64_t(o32Off) + std::uint64_t(i) * kO32Size;
        O32Record record;
        record.virtualSize = u32(flat, off + 0);
        record.rva = u32(flat, off + 4);
        record.physicalSize = u32(flat, off + 8);
        record.dataAddress = u32(flat, off + 12);
        record.realAddress = u32(flat, off + 16);
        record.flags = u32(flat, off + 20);
        records.push_back(record);
    }

    std::map<std::uint32_t, std::size_t> primaryByRva;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto found = primaryByRva.find(records[i].rva);
        if (found == primaryByRva.end() ||
            (records[found->second].physicalSize == 0 && records[i].physicalSize > 0))
            primaryByRva[records[i].rva] = i;
    }

    std::vector<PeSection> sections;
    sections.reserve(primaryByRva.size() + 9);
    for (const auto& item : primaryByRva) {
        const O32Record& record = records[item.second];
        PeSection section;
        section.name = sectionName(record, info.directories);
        section.virtualSize = record.virtualSize;
        section.rva = record.rva;
        section.flags = record.flags;
        if (record.physicalSize > 0) {
            const std::int64_t dataOff = std::int64_t(record.dataAddress) - region.loadOffset;
            if (dataOff < 0 || std::uint64_t(dataOff) + record.physicalSize > flat.size())
                return QByteArray();
            const QByteArray stored(reinterpret_cast<const char*>(flat.data() + dataOff),
                                    int(record.physicalSize));
            if ((record.flags & 0x2000U) && record.physicalSize < record.virtualSize) {
                section.data = wince::decompressCeRom(stored, record.virtualSize);
                if (quint32(section.data.size()) != record.virtualSize) return QByteArray();
                section.flags &= ~0x2000U;
            } else {
                section.data = stored;
            }
        }
        sections.push_back(std::move(section));
    }

    std::array<std::pair<std::uint32_t, std::uint32_t>, 9> directories = info.directories;
    static const char* ddNames[9] = {
        ".edata", ".idata", ".rsrc", ".pdata", ".certs",
        ".reloc", ".debug", ".imd", ".msp"
    };
    for (std::size_t i = 0; i < directories.size(); ++i) {
        const std::uint32_t rva = directories[i].first;
        const std::uint32_t size = directories[i].second;
        if (!rva || !size || directoryCovered(sections, rva)) continue;
        const std::int64_t dataOff = std::int64_t(info.imageBase) + rva - region.loadOffset;
        if (dataOff < 0 || std::uint64_t(dataOff) + size > flat.size()) {
            directories[i] = std::make_pair(0U, 0U);
            continue;
        }
        PeSection section;
        section.name = QByteArray(ddNames[i]);
        section.rva = rva & ~(kSectionAlignment - 1);
        const std::uint32_t prefix = rva - section.rva;
        section.virtualSize = alignUp(prefix + size, kSectionAlignment);
        section.flags = 0x40000040U;
        section.data = QByteArray(int(prefix), '\0');
        section.data.append(reinterpret_cast<const char*>(flat.data() + dataOff), int(size));
        sections.push_back(std::move(section));
    }

    return buildPe(info, directories, std::move(sections), region.header.cpuType);
}

bool parseB000ff(const std::vector<std::uint8_t>& input,
                 std::vector<std::uint8_t>* flatBytes, std::uint32_t* baseOffset)
{
    if (!flatBytes || !baseOffset || input.size() < 14) return false;
    std::uint64_t signature = 0;
    if (input.size() >= 8 && std::memcmp(input.data(), "B000FF\r\n", 8) == 0)
        signature = 8;
    else if (input.size() >= 7 && std::memcmp(input.data(), "B000FF\n", 7) == 0)
        signature = 7;
    else if (std::memcmp(input.data(), "B000FF", 6) == 0 &&
             u32(input, 6) != 0 && u32(input, 10) != 0)
        signature = 6;
    if (!signature || input.size() < signature + 8 + 12) return false;
    std::vector<BinRecord> records;
    std::uint64_t pos = signature + 8;
    std::uint64_t minAddress = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxEnd = 0;
    while (pos + 12 <= input.size()) {
        BinRecord record;
        record.address = u32(input, pos + 0);
        record.size = u32(input, pos + 4);
        record.dataOffset = pos + 12;
        if (record.address == 0) break;
        if (record.size > 0x10000000U || record.dataOffset + record.size > input.size())
            break;
        minAddress = std::min(minAddress, std::uint64_t(record.address));
        maxEnd = std::max(maxEnd, std::uint64_t(record.address) + record.size);
        records.push_back(record);
        pos = record.dataOffset + record.size;
    }
    if (records.empty() || maxEnd <= minAddress || maxEnd - minAddress > kMaxMaterialisedImage)
        return false;
    *baseOffset = std::uint32_t(minAddress);
    flatBytes->assign(std::size_t(maxEnd - minAddress), 0);
    for (const BinRecord& record : records) {
        const std::size_t target = std::size_t(std::uint64_t(record.address) - minAddress);
        std::copy(input.begin() + std::ptrdiff_t(record.dataOffset),
                  input.begin() + std::ptrdiff_t(record.dataOffset + record.size),
                  flatBytes->begin() + std::ptrdiff_t(target));
    }
    return true;
}


const std::uint8_t kImgfsUuid[16] = {
    0xF8, 0xAC, 0x2C, 0x9D, 0xE3, 0xD4, 0x2B, 0x4D,
    0xBD, 0x30, 0x91, 0x6E, 0xD8, 0x4F, 0x31, 0xDC
};
const std::uint32_t kImgfsDirMagic = 0x2F5314CEU;
const std::uint32_t kImgfsModule = 0xFFFFFEFEU;
const std::uint32_t kImgfsFile = 0xFFFFF6FEU;
const std::uint32_t kImgfsName = 0xFFFFFEFBU;
const std::uint32_t kImgfsSection = 0xFFFFF6FDU;
const std::uint32_t kImgfsModuleSection = 0xFFFFFEFDU;
const std::uint32_t kImgfsDirentSize = 0x34U;

bool bytesEqual(const std::vector<std::uint8_t>& data, std::uint64_t off,
                const void* needle, std::size_t length)
{
    return off + length <= data.size() &&
        std::memcmp(data.data() + std::size_t(off), needle, length) == 0;
}

bool findXipEcec(const std::vector<std::uint8_t>& data, std::uint64_t start,
                 std::uint64_t* out)
{
    if (!out) return false;
    for (std::uint64_t pos = start; pos + 8 <= data.size(); ++pos) {
        if (!bytesEqual(data, pos, "ECEC", 4)) continue;
        const std::uint32_t ptoc = u32(data, pos + 4);
        if (ptoc >= 0x80000000U && ptoc < 0xC0000000U) {
            *out = pos;
            return true;
        }
    }
    return false;
}

bool unwrapNosaj(const std::vector<std::uint8_t>& input,
                 std::vector<std::uint8_t>* flat, std::uint32_t* base,
                 QString* description)
{
    static const char signature[6] = {'N','O','S','A','J','\0'};
    static const char launchMarker[8] = {'e','U','g','O','l','A','i','D'};
    if (!flat || !base || !bytesEqual(input, 0, signature, 6) || input.size() < 0x60)
        return false;
    std::uint64_t desc = u16(input, 6);
    std::uint64_t launch = 0;
    for (int i = 0; i < 64 && desc + 0x5c <= input.size(); ++i) {
        const std::uint32_t next = u32(input, desc);
        const std::uint32_t start = u32(input, desc + 0x58);
        if (bytesEqual(input, start, launchMarker, 8)) { launch = start; break; }
        if (!next || next <= desc || next >= input.size()) break;
        desc = next;
    }
    if (!launch || launch + 0x14 > input.size()) return false;
    const std::uint32_t pfOff = u32(input, launch + 8);
    const std::uint32_t span = u32(input, launch + 12);
    const std::uint64_t xip = launch + 0x14;
    if (!span || xip + span > input.size() || !bytesEqual(input, xip + 0x40, "ECEC", 4))
        return false;
    const std::uint32_t ptoc = u32(input, xip + 0x44);
    const std::uint32_t top = ptoc & 0xff000000U;
    for (std::uint32_t i = 0; i < 16; ++i) {
        if (top < i * 0x01000000U) break;
        const std::uint32_t candidate = top - i * 0x01000000U + pfOff;
        if (candidate > ptoc || ptoc - candidate >= span) continue;
        RomHdr hdr;
        if (!parseRomHdr(input, xip + (ptoc - candidate), &hdr)) continue;
        if (hdr.physFirst != candidate || hdr.physLast - hdr.physFirst != span) continue;
        flat->assign(input.begin() + std::ptrdiff_t(xip),
                     input.begin() + std::ptrdiff_t(xip + span));
        *base = candidate;
        if (description) *description = QStringLiteral("Windows CE NOSAJ firmware");
        return true;
    }
    return false;
}

bool unwrapArnold(const std::vector<std::uint8_t>& input,
                  std::vector<std::uint8_t>* flat, std::uint32_t* base,
                  QString* description)
{
    static const char signature[16] = {
        'A','R','N','O','L','D','B','O','O','T','B','L','O','C','K','\0'};
    if (!flat || !base || !bytesEqual(input, 0, signature, 16)) return false;
    std::uint64_t ecec = 0;
    if (!findXipEcec(input, 16, &ecec) || ecec < 0x40) return false;
    const std::uint64_t xip = ecec - 0x40;
    const std::uint32_t ptoc = u32(input, ecec + 4);
    const std::uint64_t span = input.size() - xip;
    const std::uint32_t baseTop = ptoc & ~0xfffU;
    const std::uint32_t baseMin = ptoc > span ? ptoc - std::uint32_t(span) : 0;
    for (std::uint32_t candidate = baseTop; candidate >= baseMin; candidate -= 0x1000U) {
        const std::uint32_t off = ptoc - candidate;
        RomHdr hdr;
        if (off + kRomHdrSize <= span && parseRomHdr(input, xip + off, &hdr) &&
            hdr.physFirst == candidate) {
            flat->assign(input.begin() + std::ptrdiff_t(xip), input.end());
            *base = candidate;
            if (description) *description = QStringLiteral("Windows CE ARNOLDBOOTBLOCK firmware");
            return true;
        }
        if (candidate < baseMin + 0x1000U) break;
    }
    return false;
}

bool unwrapIpaqNbf(const std::vector<std::uint8_t>& input,
                   std::vector<std::uint8_t>* flat, std::uint32_t* base,
                   QString* description)
{
    if (!flat || !base || !bytesEqual(input, 0, "iPAQ ", 5)) return false;
    std::uint64_t ecec = 0;
    if (!findXipEcec(input, 5, &ecec) || ecec < 0x40) return false;
    const std::uint64_t xip = ecec - 0x40;
    flat->assign(input.begin() + std::ptrdiff_t(xip), input.end());
    *base = 0;
    if (description) *description = QStringLiteral("Windows CE iPAQ NBF firmware");
    return true;
}

bool validTocNames(const std::vector<std::uint8_t>& data, std::uint64_t romHdrOff,
                   std::uint32_t loadOffset, const RomHdr& hdr, bool requireNk)
{
    if (hdr.numMods == 0 || romHdrOff + kRomHdrSize +
        std::uint64_t(hdr.numMods) * kTocEntrySize > data.size()) return false;
    bool haveNk = false;
    const std::uint64_t toc = romHdrOff + kRomHdrSize;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(hdr.numMods, 32); ++i) {
        const std::uint64_t e = toc + std::uint64_t(i) * kTocEntrySize;
        const std::uint32_t nameVa = u32(data, e + 16);
        const QString name = readAscii(data, std::int64_t(nameVa) - loadOffset);
        if (name.isEmpty()) return false;
        if (name.compare(QStringLiteral("nk.exe"), Qt::CaseInsensitive) == 0) haveNk = true;
    }
    return !requireNk || haveNk;
}

bool findStructuralRegion(const std::vector<std::uint8_t>& data, Region* out)
{
    if (!out) return false;
    for (std::uint64_t off = 0; off + kRomHdrSize <= data.size(); off += 4) {
        RomHdr hdr;
        if (!parseRomHdr(data, off, &hdr)) continue;
        if (hdr.physFirst < 0x80000000U || hdr.physLast <= hdr.physFirst ||
            std::uint64_t(hdr.physLast - hdr.physFirst) > data.size()) continue;
        if (!validTocNames(data, off, hdr.physFirst, hdr, true)) continue;
        out->romHdrOffset = off;
        out->ptoc = hdr.physFirst + std::uint32_t(off);
        out->loadOffset = hdr.physFirst;
        out->header = hdr;
        return true;
    }
    return false;
}

struct Ce1Region {
    std::uint64_t romHdrOffset = 0;
    std::uint32_t baseVa = 0;
    RomHdr header;
};

bool ce1InlineName(const std::vector<std::uint8_t>& data, std::uint64_t off,
                   QString* out)
{
    if (!out || off >= data.size()) return false;
    QByteArray name;
    for (std::uint64_t i = off; i < data.size() && i < off + 276; ++i) {
        const unsigned char c = data[std::size_t(i)];
        if (!c) { *out = QString::fromLatin1(name); return !name.isEmpty(); }
        if (c < 0x20 || c > 0x7e) return false;
        name.append(char(c));
    }
    return false;
}

std::vector<Ce1Region> findCe1Regions(const std::vector<std::uint8_t>& data)
{
    const std::uint64_t tocSize = 0x130;
    const std::uint64_t fileSize = 0x12c;
    struct Candidate { std::uint64_t off; RomHdr hdr; };
    std::vector<Candidate> candidates;
    for (std::uint64_t off = 0; off + kRomHdrSize <= data.size(); off += 4) {
        RomHdr hdr;
        if (!parseRomHdr(data, off, &hdr) || !hdr.numMods ||
            hdr.physFirst < 0x80000000U || hdr.physLast <= hdr.physFirst ||
            std::uint64_t(hdr.physLast - hdr.physFirst) > data.size()) continue;
        const std::uint64_t arrays = std::uint64_t(hdr.numMods) * tocSize +
                                     std::uint64_t(hdr.numFiles) * fileSize;
        if (off + kRomHdrSize + arrays > data.size()) continue;
        QString name;
        if (!ce1InlineName(data, off + kRomHdrSize + 0x10, &name)) continue;
        candidates.push_back({off, hdr});
    }

    std::uint32_t commonBase = 0;
    for (const Candidate& c : candidates) {
        const std::uint32_t ntVa = u32(data, c.off + kRomHdrSize + 0x124);
        if (ntVa >= c.hdr.physFirst &&
            std::uint64_t(ntVa - c.hdr.physFirst) + 4 <= data.size() &&
            u32(data, ntVa - c.hdr.physFirst) == 0x00004550U) {
            commonBase = c.hdr.physFirst;
            break;
        }
    }
    if (!commonBase) return std::vector<Ce1Region>();

    std::vector<Ce1Region> result;
    for (const Candidate& c : candidates) {
        const std::uint32_t ntVa = u32(data, c.off + kRomHdrSize + 0x124);
        if (ntVa < commonBase || std::uint64_t(ntVa - commonBase) + 4 > data.size() ||
            u32(data, ntVa - commonBase) != 0x00004550U) continue;
        Ce1Region region;
        region.romHdrOffset = c.off;
        region.baseVa = commonBase;
        region.header = c.hdr;
        result.push_back(region);
    }
    std::sort(result.begin(), result.end(), [](const Ce1Region& a, const Ce1Region& b) {
        return a.header.physFirst < b.header.physFirst;
    });
    return result;
}

QByteArray reconstructCe1Pe(const std::vector<std::uint8_t>& flat, std::uint32_t baseVa,
                            std::uint32_t ntVa, std::uint32_t sectionHeadersVa)
{
    if (ntVa < baseVa || sectionHeadersVa < baseVa) return QByteArray();
    const std::uint64_t nt = ntVa - baseVa;
    const std::uint64_t sh = sectionHeadersVa - baseVa;
    if (nt + 24 > flat.size() || u32(flat, nt) != 0x00004550U) return QByteArray();
    const std::uint16_t count = u16(flat, nt + 6);
    const std::uint16_t optionalSize = u16(flat, nt + 0x14);
    if (!count || count > 96 || nt + 24 + optionalSize > flat.size() ||
        sh + std::uint64_t(count) * 40 > flat.size()) return QByteArray();

    struct Section { QByteArray header; QByteArray data; std::uint32_t rva; std::uint32_t vsize; };
    std::vector<Section> sections;
    std::map<std::uint32_t, std::size_t> primary;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint64_t so = sh + std::uint64_t(i) * 40;
        QByteArray header(reinterpret_cast<const char*>(flat.data() + so), 40);
        const std::uint32_t vsize = u32(flat, so + 8);
        const std::uint32_t rva = u32(flat, so + 12);
        const std::uint32_t storedSize = u32(flat, so + 16);
        const std::uint32_t dataVa = u32(flat, so + 20);
        std::uint32_t flags = u32(flat, so + 36);
        QByteArray bytes;
        if (storedSize) {
            if (dataVa < baseVa || std::uint64_t(dataVa - baseVa) + storedSize > flat.size())
                return QByteArray();
            bytes = QByteArray(reinterpret_cast<const char*>(flat.data() + (dataVa - baseVa)),
                               int(storedSize));
            if ((flags & 0x2000U) && storedSize < vsize) {
                bytes = wince::decompressCe1Lzw(bytes, vsize);
                if (quint32(bytes.size()) != vsize) return QByteArray();
                flags &= ~0x2000U;
            }
        }
        put32(header, 16, std::uint32_t(bytes.size()));
        put32(header, 36, flags);
        const auto found = primary.find(rva);
        if (found == primary.end() || sections[found->second].data.isEmpty()) {
            if (found == primary.end()) {
                primary[rva] = sections.size();
                sections.push_back({header, bytes, rva, vsize});
            } else {
                sections[found->second] = {header, bytes, rva, vsize};
            }
        }
    }
    if (sections.empty()) return QByteArray();

    QByteArray ntHeaders(reinterpret_cast<const char*>(flat.data() + nt), int(24 + optionalSize));
    put16(ntHeaders, 6, std::uint16_t(sections.size()));
    const std::uint32_t fileAlignment = 0x200;
    const std::uint32_t bodyOffset = alignUp(0x80U + std::uint32_t(ntHeaders.size()) +
                                             std::uint32_t(sections.size()) * 40U,
                                             fileAlignment);
    std::uint32_t cursor = bodyOffset;
    QByteArray body;
    for (Section& section : sections) {
        if (section.data.isEmpty()) put32(section.header, 20, 0);
        else {
            put32(section.header, 20, cursor);
            body.append(section.data);
            const std::uint32_t aligned = alignUp(std::uint32_t(section.data.size()), fileAlignment);
            body.append(int(aligned - section.data.size()), '\0');
            cursor += aligned;
        }
    }
    QByteArray pe(0x80, '\0');
    pe[0] = 'M'; pe[1] = 'Z'; put32(pe, 0x3c, 0x80);
    pe.append(ntHeaders);
    for (const Section& section : sections) pe.append(section.header);
    if (pe.size() < int(bodyOffset)) pe.append(int(bodyOffset) - pe.size(), '\0');
    pe.append(body);
    return pe;
}

QByteArray extractCe1File(const std::vector<std::uint8_t>& flat, std::uint32_t baseVa,
                          std::uint32_t loadVa, std::uint32_t compressedSize,
                          std::uint32_t realSize)
{
    if (loadVa < baseVa || std::uint64_t(loadVa - baseVa) + compressedSize > flat.size())
        return QByteArray();
    const QByteArray raw(reinterpret_cast<const char*>(flat.data() + (loadVa - baseVa)),
                         int(compressedSize));
    const std::uint32_t pages = (realSize + 0xfffU) / 0x1000U;
    if (std::uint64_t(pages) * 4 > compressedSize) return QByteArray();
    QByteArray out;
    out.reserve(int(realSize));
    for (std::uint32_t i = 0; i < pages; ++i) {
        const std::uint32_t marker = qU32(raw, int(i * 4));
        const std::uint32_t start = marker & 0x7fffffffU;
        const bool stored = (marker & 0x80000000U) != 0;
        const std::uint32_t end = i + 1 < pages
            ? (qU32(raw, int((i + 1) * 4)) & 0x7fffffffU) : compressedSize;
        if (start > end || end > compressedSize) return QByteArray();
        const std::uint32_t wanted = std::min<std::uint32_t>(0x1000U, realSize - i * 0x1000U);
        const QByteArray page = raw.mid(int(start), int(end - start));
        QByteArray decoded = stored ? page.left(int(wanted)) : wince::decompressCe1Lzw(page, wanted);
        if (quint32(decoded.size()) != wanted) return QByteArray();
        out.append(decoded);
    }
    out.resize(int(realSize));
    return out;
}

void appendEmbedded(QVector<ResourceEntry>& entries, const QString& name,
                    const QByteArray& bytes, quint64 offset = 0,
                    const fs::ByteStorePtr& store = fs::ByteStorePtr())
{
    ResourceEntry entry;
    entry.type = QStringLiteral("WINCE_FILE");
    entry.name = safeName(name, QStringLiteral("unnamed.bin"));
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = offset;
    entry.dataSize = store ? quint64(bytes.size()) : quint64(bytes.size());
    entry.format = ModuleFormat::Unknown;
    entry.isEmbeddedFile = true;
    if (store) entry.content = store;
    else entry.data = bytes;
    entries.push_back(std::move(entry));
}

void appendEmbeddedStore(QVector<ResourceEntry>& entries, const QString& name,
                         quint64 offset, quint64 size, const fs::ByteStorePtr& store)
{
    ResourceEntry entry;
    entry.type = QStringLiteral("WINCE_FILE");
    entry.name = safeName(name, QStringLiteral("unnamed.bin"));
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = offset;
    entry.dataSize = size;
    entry.format = ModuleFormat::Unknown;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::SubStore>(store, std::int64_t(offset), std::int64_t(size));
    entries.push_back(std::move(entry));
}

void extractClassicXip(const fs::ByteStorePtr& flatStore,
                       const std::vector<std::uint8_t>& flat,
                       const std::vector<Region>& regions,
                       QVector<ResourceEntry>* entries, int* rebuiltModules,
                       int* ordinaryFiles)
{
    if (!entries) return;
    for (const Region& region : regions) {
        const std::uint64_t toc = region.romHdrOffset + kRomHdrSize;
        const std::uint64_t files = toc + std::uint64_t(region.header.numMods) * kTocEntrySize;
        for (std::uint32_t i = 0; i < region.header.numMods; ++i) {
            const std::uint64_t e = toc + std::uint64_t(i) * kTocEntrySize;
            if (e + kTocEntrySize > flat.size()) break;
            QString name = readAscii(flat, std::int64_t(u32(flat, e + 16)) - region.loadOffset);
            name = safeName(name, QStringLiteral("module_%1.dll").arg(i, 3, 10, QLatin1Char('0')));
            const QByteArray pe = reconstructPe(flat, region, u32(flat, e + 20), u32(flat, e + 24));
            if (pe.isEmpty()) continue;
            appendEmbedded(*entries, name, pe);
            if (rebuiltModules) ++*rebuiltModules;
        }
        for (std::uint32_t i = 0; i < region.header.numFiles; ++i) {
            const std::uint64_t e = files + std::uint64_t(i) * kFileEntrySize;
            if (e + kFileEntrySize > flat.size()) break;
            const std::uint32_t realSize = u32(flat, e + 12);
            const std::uint32_t compressedSize = u32(flat, e + 16);
            const std::uint32_t loadVa = u32(flat, e + 24);
            if (!compressedSize || loadVa < region.loadOffset ||
                std::uint64_t(loadVa - region.loadOffset) + compressedSize > flat.size()) continue;
            QString name = readAscii(flat, std::int64_t(u32(flat, e + 20)) - region.loadOffset);
            name = safeName(name, QStringLiteral("file_%1.bin").arg(i, 3, 10, QLatin1Char('0')));
            const std::uint64_t dataOff = loadVa - region.loadOffset;
            if (compressedSize == realSize) {
                appendEmbeddedStore(*entries, name, dataOff, realSize, flatStore);
            } else {
                const QByteArray stored(reinterpret_cast<const char*>(flat.data() + dataOff),
                                        int(compressedSize));
                const QByteArray decoded = wince::decompressCeRom(stored, realSize);
                if (quint32(decoded.size()) != realSize) continue;
                appendEmbedded(*entries, name, decoded);
            }
            if (ordinaryFiles) ++*ordinaryFiles;
        }
    }
}

void extractCe1(const std::vector<std::uint8_t>& flat,
                const std::vector<Ce1Region>& regions,
                QVector<ResourceEntry>* entries, int* rebuiltModules,
                int* ordinaryFiles)
{
    if (!entries) return;
    for (const Ce1Region& region : regions) {
        const std::uint64_t toc = region.romHdrOffset + kRomHdrSize;
        const std::uint64_t files = toc + std::uint64_t(region.header.numMods) * 0x130U;
        for (std::uint32_t i = 0; i < region.header.numMods; ++i) {
            const std::uint64_t e = toc + std::uint64_t(i) * 0x130U;
            if (e + 0x130 > flat.size()) break;
            QString name;
            if (!ce1InlineName(flat, e + 0x10, &name)) continue;
            const QByteArray pe = reconstructCe1Pe(flat, region.baseVa,
                                                   u32(flat, e + 0x124),
                                                   u32(flat, e + 0x128));
            if (pe.isEmpty()) continue;
            appendEmbedded(*entries, name, pe);
            if (rebuiltModules) ++*rebuiltModules;
        }
        for (std::uint32_t i = 0; i < region.header.numFiles; ++i) {
            const std::uint64_t e = files + std::uint64_t(i) * 0x12cU;
            if (e + 0x12c > flat.size()) break;
            QString name;
            if (!ce1InlineName(flat, e + 0x14, &name)) continue;
            const QByteArray bytes = extractCe1File(flat, region.baseVa, u32(flat, e + 0x128),
                                                    u32(flat, e + 0x10), u32(flat, e + 0x0c));
            if (bytes.isNull()) continue;
            appendEmbedded(*entries, name, bytes);
            if (ordinaryFiles) ++*ordinaryFiles;
        }
    }
}

std::int64_t findImgfsBase(const std::vector<std::uint8_t>& data)
{
    for (std::uint64_t pos = 0; pos + 0x28 <= data.size(); ++pos) {
        if ((pos & 0xfff) || !bytesEqual(data, pos, kImgfsUuid, 16)) continue;
        if (u32(data, pos + 0x1c) != kImgfsDirentSize) continue;
        const std::uint32_t block = u32(data, pos + 0x24);
        if (block >= 0x200 && block <= 0x10000) return std::int64_t(pos);
    }
    return -1;
}

class ImgfsTranslator {
public:
    ImgfsTranslator(const std::vector<std::uint8_t>& raw, std::uint64_t base)
        : raw_(raw), base_(base)
    {
        const std::uint64_t blocks = (raw.size() - std::min<std::uint64_t>(base, raw.size())) / 0x10000U;
        std::uint64_t valid = 0, total = 0;
        for (std::uint64_t b = 0; b < std::min<std::uint64_t>(blocks, 8); ++b) {
            const std::uint64_t map = base + b * 0x10000U + 15 * 0x1000U;
            for (int e = 0; e < 15 && map + e * 8 + 8 <= raw.size(); ++e) {
                const std::uint32_t sector = u32(raw, map + e * 8);
                const std::uint32_t flags = u32(raw, map + e * 8 + 4);
                ++total;
                if (sector != 0xffffffffU && (flags & 0xfff00000U) == 0xfff00000U) ++valid;
            }
        }
        ftl_ = total && valid * 10 >= total * 4;
        if (!ftl_) return;
        std::map<std::uint32_t, std::uint32_t> pairs;
        for (std::uint64_t b = 0; b < blocks; ++b) {
            const std::uint64_t map = base + b * 0x10000U + 15 * 0x1000U;
            for (int e = 0; e < 15 && map + e * 8 + 8 <= raw.size(); ++e) {
                const std::uint32_t sector = u32(raw, map + e * 8);
                const std::uint32_t flags = u32(raw, map + e * 8 + 4);
                if (sector == 0xffffffffU || (flags & 0xfff00000U) != 0xfff00000U) continue;
                pairs[sector] = std::uint32_t(b * 16 + e);
            }
        }
        bool haveBase = false;
        for (const auto& p : pairs) {
            if (p.second == 0) { baseSector_ = p.first; haveBase = true; }
            physicalToLogical_[p.second] = p.first;
        }
        if (!haveBase) {
            ftl_ = false;
            physicalToLogical_.clear();
            return;
        }
        mapping_.swap(pairs);
    }

    std::int64_t translate(std::uint32_t logical) const
    {
        if (!logical) return -1;
        if (!ftl_) {
            const std::uint64_t off = base_ + logical;
            return off < raw_.size() ? std::int64_t(off) : -1;
        }
        const std::uint32_t sector = logical / 0x1000U + baseSector_;
        const auto found = mapping_.find(sector);
        if (found == mapping_.end()) return -1;
        const std::uint64_t off = base_ + std::uint64_t(found->second) * 0x1000U +
                                  (logical & 0xfffU);
        return off < raw_.size() ? std::int64_t(off) : -1;
    }

    QByteArray read(std::uint32_t logical, std::uint32_t size) const
    {
        QByteArray out;
        out.reserve(int(size));
        std::uint32_t cursor = logical;
        std::uint32_t remaining = size;
        while (remaining) {
            const std::int64_t off = translate(cursor);
            if (off < 0) return QByteArray();
            const std::uint32_t part = std::min<std::uint32_t>(remaining, 0x1000U - (cursor & 0xfffU));
            if (std::uint64_t(off) + part > raw_.size()) return QByteArray();
            out.append(reinterpret_cast<const char*>(raw_.data() + off), int(part));
            cursor += part;
            remaining -= part;
        }
        return out;
    }

    std::int64_t logicalAddress(std::uint64_t physicalOffset) const
    {
        if (physicalOffset < base_ || physicalOffset >= raw_.size()) return -1;
        const std::uint64_t relative = physicalOffset - base_;
        if (!ftl_) return std::int64_t(relative);
        const std::uint32_t physicalPage = std::uint32_t(relative / 0x1000U);
        const auto found = physicalToLogical_.find(physicalPage);
        if (found == physicalToLogical_.end() || found->second < baseSector_) return -1;
        return std::int64_t(found->second - baseSector_) * 0x1000 +
               std::int64_t(relative & 0xfffU);
    }

    bool isFtl() const { return ftl_; }
    std::uint64_t base() const { return base_; }

private:
    const std::vector<std::uint8_t>& raw_;
    std::uint64_t base_ = 0;
    bool ftl_ = false;
    std::uint32_t baseSector_ = 0;
    std::map<std::uint32_t, std::uint32_t> mapping_;
    std::map<std::uint32_t, std::uint32_t> physicalToLogical_;
};

QString utf16Ascii(const QByteArray& data)
{
    QString result;
    for (int i = 0; i + 1 < data.size(); i += 2) {
        const std::uint16_t c = qU16(data, i);
        if (!c) break;
        result.append(QChar(c));
    }
    return result;
}

QString imgfsName(const std::vector<std::uint8_t>& raw, const ImgfsTranslator& tr,
                  const QByteArray& nameInfo,
                  const std::map<std::uint32_t, QString>& nameMap)
{
    if (nameInfo.size() < 12) return QString();
    const std::uint16_t length = qU16(nameInfo, 0);
    const std::uint16_t flags = qU16(nameInfo, 2);
    if (!length) return QString();
    if (length <= 4) return utf16Ascii(nameInfo.mid(4, int(length) * 2));
    const std::uint32_t ptr = qU32(nameInfo, 8);
    if (flags & 2) {
        const std::int64_t off = tr.translate(ptr);
        if (off >= 0 && std::uint64_t(off) + kImgfsDirentSize <= raw.size() &&
            u32(raw, off) == kImgfsName)
            return utf16Ascii(QByteArray(reinterpret_cast<const char*>(raw.data() + off + 4),
                                        int(kImgfsDirentSize - 4)));
        const auto fallback = nameMap.find(ptr);
        return fallback == nameMap.end() ? QString() : fallback->second;
    }
    return utf16Ascii(tr.read(ptr, std::uint32_t(length) * 2));
}

QByteArray imgfsIndexData(const ImgfsTranslator& tr, std::uint32_t ptr,
                         std::uint32_t size, std::uint32_t expected)
{
    if (!ptr || !size) return expected == 0 ? QByteArray(0, '\0') : QByteArray();
    const QByteArray index = tr.read(ptr, size);
    if (quint32(index.size()) != size) return QByteArray();
    QByteArray out;
    out.reserve(int(expected));
    for (std::uint32_t off = 0; off + 8 <= size; off += 8) {
        const std::uint16_t compressed = qU16(index, int(off));
        const std::uint16_t full = qU16(index, int(off + 2));
        const std::uint32_t dataPtr = qU32(index, int(off + 4));
        if (!compressed && !full && !dataPtr) break;
        if (!dataPtr) { out.append(int(full), '\0'); continue; }
        const QByteArray chunk = tr.read(dataPtr, compressed);
        if (quint32(chunk.size()) != compressed) return QByteArray();
        if (compressed == full) out.append(chunk);
        else {
            const QByteArray decoded = wince::decompressImgfsXpress(chunk, full);
            if (quint32(decoded.size()) != full) return QByteArray();
            out.append(decoded);
        }
    }
    if (expected && quint32(out.size()) < expected) return QByteArray();
    if (expected) out.resize(int(expected));
    return out;
}

QByteArray reconstructImgfsPe(const QByteArray& header,
                              const std::map<QString, QByteArray>& sectionData,
                              std::uint16_t machine)
{
    const std::vector<std::uint8_t> h(reinterpret_cast<const std::uint8_t*>(header.constData()),
                                      reinterpret_cast<const std::uint8_t*>(header.constData()) + header.size());
    E32Info info;
    if (!parseE32Base(h, 0, 0x24, &info) || !validE32(info) ||
        0x70U + std::uint64_t(info.objectCount) * kO32Size > h.size()) return QByteArray();
    std::vector<O32Record> records;
    for (std::uint16_t i = 0; i < info.objectCount; ++i) {
        const std::uint64_t off = 0x70U + std::uint64_t(i) * kO32Size;
        O32Record record;
        record.virtualSize = u32(h, off);
        record.rva = u32(h, off + 4);
        record.physicalSize = u32(h, off + 8);
        record.dataAddress = u32(h, off + 12);
        record.realAddress = u32(h, off + 16);
        record.flags = u32(h, off + 20);
        records.push_back(record);
    }
    std::map<std::uint32_t, std::size_t> primaries;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto found = primaries.find(records[i].rva);
        if (found == primaries.end() ||
            (records[found->second].physicalSize == 0 && records[i].physicalSize))
            primaries[records[i].rva] = i;
    }
    std::vector<PeSection> sections;
    for (const auto& p : primaries) {
        const std::size_t i = p.second;
        const O32Record& r = records[i];
        PeSection section;
        section.name = sectionName(r, info.directories);
        section.virtualSize = r.virtualSize;
        section.rva = r.rva;
        section.flags = r.flags & ~0x2000U;
        const QString key = QStringLiteral("S%1").arg(i, 3, 10, QLatin1Char('0'));
        const auto found = sectionData.find(key);
        if (found != sectionData.end()) section.data = found->second;
        if (r.physicalSize && section.data.isEmpty()) return QByteArray();
        sections.push_back(std::move(section));
    }
    std::array<std::pair<std::uint32_t, std::uint32_t>, 9> directories = info.directories;
    for (auto& dd : directories) if (dd.first && !directoryCovered(sections, dd.first)) dd = {0U, 0U};
    return buildPe(info, directories, std::move(sections), machine);
}

int extractImgfs(const std::vector<std::uint8_t>& raw, std::uint16_t machine,
                 QVector<ResourceEntry>* entries, bool* wasFtl)
{
    if (!entries) return 0;
    const std::int64_t base = findImgfsBase(raw);
    if (base < 0) return 0;
    const std::uint32_t blockSize = u32(raw, base + 0x24);
    const std::uint32_t perBlock = (blockSize - 8) / kImgfsDirentSize;
    ImgfsTranslator tr(raw, std::uint64_t(base));
    if (wasFtl) *wasFtl = tr.isFtl();
    struct Ref { std::uint64_t off; std::uint32_t magic; };
    std::vector<Ref> refs;
    for (std::uint64_t block = std::uint64_t(base); block + 8 <= raw.size(); block += blockSize) {
        if (u32(raw, block) != kImgfsDirMagic) continue;
        for (std::uint32_t i = 0; i < perBlock; ++i) {
            const std::uint64_t e = block + 8 + std::uint64_t(i) * kImgfsDirentSize;
            if (e + kImgfsDirentSize > raw.size()) break;
            refs.push_back({e, u32(raw, e)});
        }
    }
    std::map<std::uint32_t, QString> nameMap;
    for (const Ref& ref : refs) {
        if (ref.magic != kImgfsName) continue;
        const std::int64_t logical = tr.logicalAddress(ref.off);
        if (logical < 0 || std::uint64_t(logical) > std::numeric_limits<std::uint32_t>::max()) continue;
        nameMap[std::uint32_t(logical)] = utf16Ascii(
            QByteArray(reinterpret_cast<const char*>(raw.data() + ref.off + 4),
                       int(kImgfsDirentSize - 4)));
    }

    int count = 0;
    for (std::size_t i = 0; i < refs.size();) {
        const Ref ref = refs[i];
        if (ref.magic == kImgfsFile) {
            const QByteArray dirent(reinterpret_cast<const char*>(raw.data() + ref.off),
                                    int(kImgfsDirentSize));
            QString name = imgfsName(raw, tr, dirent.mid(0x0c, 12), nameMap);
            name = safeName(name, QStringLiteral("unnamed_%1.dat").arg(ref.off - base, 6, 16, QLatin1Char('0')));
            const QByteArray data = imgfsIndexData(tr, qU32(dirent, 0x2c), qU32(dirent, 0x30),
                                                   qU32(dirent, 0x18));
            if (!data.isNull()) { appendEmbedded(*entries, name, data); ++count; }
            ++i;
            continue;
        }
        if (ref.magic == kImgfsModule) {
            const QByteArray dirent(reinterpret_cast<const char*>(raw.data() + ref.off),
                                    int(kImgfsDirentSize));
            QString name = imgfsName(raw, tr, dirent.mid(0x0c, 12), nameMap);
            name = safeName(name, QStringLiteral("unnamed_%1.dll").arg(ref.off - base, 6, 16, QLatin1Char('0')));
            const QByteArray header = imgfsIndexData(tr, qU32(dirent, 0x2c), qU32(dirent, 0x30),
                                                     qU32(dirent, 0x18));
            std::map<QString, QByteArray> sections;
            std::size_t j = i + 1;
            while (j < refs.size()) {
                if (refs[j].magic == kImgfsName) { ++j; continue; }
                if (refs[j].magic != kImgfsSection && refs[j].magic != kImgfsModuleSection) break;
                const QByteArray sd(reinterpret_cast<const char*>(raw.data() + refs[j].off),
                                    int(kImgfsDirentSize));
                const QString sectionNameValue = imgfsName(raw, tr, sd.mid(0x0c, 12), nameMap);
                const QByteArray bytes = imgfsIndexData(tr, qU32(sd, 0x1c), qU32(sd, 0x20),
                                                        qU32(sd, 0x18));
                if (!sectionNameValue.isEmpty() && !bytes.isNull())
                    sections[sectionNameValue] = bytes;
                ++j;
            }
            const QByteArray pe = reconstructImgfsPe(header, sections, machine);
            if (!pe.isEmpty()) { appendEmbedded(*entries, name, pe); ++count; }
            i = j;
            continue;
        }
        ++i;
    }
    return count;
}

void uniqueAndSort(QVector<ResourceEntry>* entries)
{
    if (!entries) return;
    std::sort(entries->begin(), entries->end(), [](const ResourceEntry& a, const ResourceEntry& b) {
        const int ci = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return ci != 0 ? ci < 0 : a.name < b.name;
    });
    QSet<QString> used;
    for (ResourceEntry& entry : *entries) {
        QString base = entry.name;
        QString candidate = base;
        int n = 2;
        while (used.contains(candidate.toLower())) {
            const QFileInfo fi(base);
            candidate = fi.suffix().isEmpty()
                ? QStringLiteral("%1.%2").arg(base).arg(n++)
                : QStringLiteral("%1.%2.%3").arg(fi.completeBaseName()).arg(n++).arg(fi.suffix());
        }
        entry.name = candidate;
        used.insert(candidate.toLower());
    }
}

void addWindowsDirectory(QVector<ResourceEntry>* resources, const fs::ByteStorePtr& source)
{
    ResourceEntry directory;
    directory.type = QStringLiteral("WINCE_DIR");
    directory.name = QStringLiteral("Windows");
    directory.language = QStringLiteral("neutral");
    directory.format = ModuleFormat::WINCE_ROM;
    directory.isDirectory = true;
    directory.containerSubPath = QStringLiteral("Windows");
    directory.content = source;
    resources->push_back(std::move(directory));
}

struct ParsedArchive {
    std::vector<std::uint8_t> flat;
    std::uint32_t base = 0;
    QString description;
    std::vector<Region> classic;
    std::vector<Ce1Region> ce1;
    QVector<ResourceEntry> entries;
    int modules = 0;
    int files = 0;
    int imgfs = 0;
    bool imgfsFtl = false;
};

bool parseArchive(const std::vector<std::uint8_t>& input, ParsedArchive* parsed)
{
    if (!parsed) return false;
    const bool b000ff = (input.size() >= 8 && std::memcmp(input.data(), "B000FF\r\n", 8) == 0) ||
        (input.size() >= 7 && std::memcmp(input.data(), "B000FF\n", 7) == 0) ||
        (input.size() >= 14 && std::memcmp(input.data(), "B000FF", 6) == 0 &&
         u32(input, 6) != 0 && u32(input, 10) != 0);
    if (b000ff) {
        if (!parseB000ff(input, &parsed->flat, &parsed->base)) return false;
        parsed->description = QStringLiteral("Windows CE B000FF ROM");
    } else if (unwrapNosaj(input, &parsed->flat, &parsed->base, &parsed->description) ||
               unwrapArnold(input, &parsed->flat, &parsed->base, &parsed->description) ||
               unwrapIpaqNbf(input, &parsed->flat, &parsed->base, &parsed->description)) {
    } else {
        parsed->flat = input;
        parsed->description = QStringLiteral("Windows CE ROM / IMGFS");
    }

    parsed->classic = findRegions(parsed->flat, parsed->base);
    if (parsed->classic.empty()) {
        Region structural;
        if (findStructuralRegion(parsed->flat, &structural)) parsed->classic.push_back(structural);
    }
    if (parsed->classic.empty()) parsed->ce1 = findCe1Regions(parsed->flat);

    const auto flatStore = std::make_shared<fs::MemoryStore>(parsed->flat);
    if (!parsed->classic.empty())
        extractClassicXip(flatStore, parsed->flat, parsed->classic, &parsed->entries,
                          &parsed->modules, &parsed->files);
    else if (!parsed->ce1.empty())
        extractCe1(parsed->flat, parsed->ce1, &parsed->entries,
                   &parsed->modules, &parsed->files);

    std::uint16_t cpu = 0;
    if (!parsed->classic.empty()) cpu = parsed->classic.front().header.cpuType;
    else if (!parsed->ce1.empty()) cpu = parsed->ce1.front().header.cpuType;
    parsed->imgfs = extractImgfs(input, cpu, &parsed->entries, &parsed->imgfsFtl);
    uniqueAndSort(&parsed->entries);
    if (parsed->entries.isEmpty()) return false;

    if (!parsed->classic.empty()) {
        parsed->description += QStringLiteral(" — %1 XIP region(s), %2")
            .arg(parsed->classic.size()).arg(cpuName(cpu));
    } else if (!parsed->ce1.empty()) {
        parsed->description += QStringLiteral(" — Windows CE 1.x, %1 region(s), %2")
            .arg(parsed->ce1.size()).arg(cpuName(cpu));
    }
    if (parsed->imgfs)
        parsed->description += QStringLiteral(" — IMGFS %1 (%2 entries)")
            .arg(parsed->imgfsFtl ? QStringLiteral("FTL") : QStringLiteral("direct"))
            .arg(parsed->imgfs);
    parsed->description += QStringLiteral(" — Windows: %1 modules, %2 files")
        .arg(parsed->modules).arg(parsed->files + parsed->imgfs);
    return true;
}

bool parseB000ffArchive(const fs::ByteStorePtr& source, ParsedArchive* parsed)
{
    if (!source || !parsed) return false;
    std::vector<BinRecord> records;
    std::uint32_t base = 0;
    std::uint64_t end = 0;
    if (!parseB000ffRecords(source, &records, &base, &end)) return false;

    const auto logical = std::make_shared<B000ffStore>(source, records, base, end);
    const std::vector<Region> regions = findB000ffRegions(*logical);
    std::uint16_t cpu = 0;
    int extractedRegions = 0;
    for (const Region& region : regions) {
        const std::uint64_t minimumEnd = region.romHdrOffset + kRomHdrSize +
            std::uint64_t(region.header.numMods) * kTocEntrySize +
            std::uint64_t(region.header.numFiles) * kFileEntrySize;
        const std::uint64_t physicalSpan = region.header.physLast >= region.header.physFirst
            ? std::uint64_t(region.header.physLast) - region.header.physFirst : 0;

        // A ROM may use a virtual alias in ECEC/PTOC while the BIN records are
        // addressed physically.  Derive several affine mappings and validate
        // them by actual extraction instead of trusting the first ROMHDR match.
        std::set<std::pair<std::uint64_t, std::uint32_t> > mappings;
        if (region.ptoc >= region.header.physFirst) {
            const std::uint64_t delta = std::uint64_t(region.ptoc) - region.header.physFirst;
            if (region.romHdrOffset >= delta) {
                const std::uint64_t physicalStart = region.romHdrOffset - delta;
                const std::uint64_t load64 = std::uint64_t(region.header.physFirst) - physicalStart;
                if (load64 <= 0xffffffffULL)
                    mappings.insert(std::make_pair(physicalStart, std::uint32_t(load64)));
            }
        }
        mappings.insert(std::make_pair(std::min(region.ececOffset, region.romHdrOffset),
                                       region.loadOffset));
        mappings.insert(std::make_pair(std::min(region.ececOffset, region.romHdrOffset), base));
        mappings.insert(std::make_pair(std::min(region.ececOffset, region.romHdrOffset),
                                       base | 0x80000000U));

        bool extracted = false;
        for (const auto& mapping : mappings) {
            std::uint64_t startLogical = mapping.first;
            const std::uint32_t globalLoad = mapping.second;
            if (startLogical > region.ececOffset || startLogical > region.romHdrOffset)
                continue;
            std::uint64_t regionEnd = minimumEnd;
            if (physicalSpan && physicalSpan <= kMaxMaterialisedImage &&
                startLogical <= std::uint64_t(logical->capacity()) -
                    std::min<std::uint64_t>(physicalSpan, logical->capacity()))
                regionEnd = std::max(regionEnd, startLogical + physicalSpan);
            if (regionEnd <= startLogical || regionEnd > std::uint64_t(logical->capacity()) ||
                regionEnd - startLogical > kMaxMaterialisedImage)
                continue;

            std::vector<std::uint8_t> flat = logical->readRange(
                std::int64_t(startLogical), std::int64_t(regionEnd - startLogical));
            if (flat.size() != std::size_t(regionEnd - startLogical)) continue;
            Region local = region;
            local.ececOffset = region.ececOffset - startLogical;
            local.romHdrOffset = region.romHdrOffset - startLogical;
            const std::uint64_t localLoad = std::uint64_t(globalLoad) + startLogical;
            if (localLoad > std::numeric_limits<std::uint32_t>::max()) continue;
            local.loadOffset = std::uint32_t(localLoad);

            const auto flatStore = std::make_shared<fs::MemoryStore>(flat);
            const int before = parsed->entries.size();
            extractClassicXip(flatStore, flat, std::vector<Region>(1, local),
                              &parsed->entries, &parsed->modules, &parsed->files);
            if (parsed->entries.size() > before) {
                extracted = true;
                if (!cpu) cpu = local.header.cpuType;
                break;
            }
        }
        if (extracted) ++extractedRegions;
    }

    // IMGFS and structural CE1/CE2 fallbacks still require a contiguous logical
    // image. Keep that path for ordinary-sized images; huge sparse B000FF files
    // remain cheap and extract their XIP regions independently above.
    if (std::uint64_t(logical->capacity()) <= kMaxMaterialisedImage) {
        const std::vector<std::uint8_t> raw = logical->readAll();
        if (parsed->entries.isEmpty()) {
            ParsedArchive fallback;
            if (parseArchive(raw, &fallback)) {
                *parsed = std::move(fallback);
                parsed->description = QStringLiteral("Windows CE B000FF ROM — ") +
                    parsed->description;
                return true;
            }
        }
        parsed->imgfs = extractImgfs(raw, cpu, &parsed->entries, &parsed->imgfsFtl);
    }

    uniqueAndSort(&parsed->entries);
    if (parsed->entries.isEmpty()) return false;
    parsed->description = QStringLiteral("Windows CE B000FF ROM");
    if (extractedRegions)
        parsed->description += QStringLiteral(" — %1 XIP region(s), %2")
            .arg(extractedRegions).arg(cpuName(cpu));
    if (parsed->imgfs)
        parsed->description += QStringLiteral(" — IMGFS %1 (%2 entries)")
            .arg(parsed->imgfsFtl ? QStringLiteral("FTL") : QStringLiteral("direct"))
            .arg(parsed->imgfs);
    parsed->description += QStringLiteral(" — Windows: %1 modules, %2 files")
        .arg(parsed->modules).arg(parsed->files + parsed->imgfs);
    return true;
}

} // namespace

ModulePtr WinceRomModule::open(const QString& filePath, const QString& subPath)
{
    return open(storeForFile(filePath), filePath, subPath);
}

ModulePtr WinceRomModule::open(const QByteArray& data, const QString& logicalName,
                               const QString& subPath)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName, subPath);
}

ModulePtr WinceRomModule::open(const fs::ByteStorePtr& file, const QString& sourceName,
                               const QString& subPath)
{
    auto module = peare::makeUnique<WinceRomModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::WINCE_ROM;
    if (!file || file->capacity() <= 0) {
        module->info_.error = QStringLiteral("Invalid or unsupported Windows CE image");
        return ModulePtr(std::move(module));
    }
    module->source_ = file;

    QString normalized = subPath;
    while (normalized.startsWith(QLatin1Char('/')) || normalized.startsWith(QLatin1Char('\\')))
        normalized.remove(0, 1);
    if (normalized.isEmpty()) {
        module->info_.description = QStringLiteral("Windows CE ROM / IMGFS");
        addWindowsDirectory(&module->resources_, file);
        return ModulePtr(std::move(module));
    }
    if (normalized.compare(QStringLiteral("Windows"), Qt::CaseInsensitive) != 0) {
        module->info_.error = QStringLiteral("Windows CE directory not found: %1").arg(subPath);
        return ModulePtr(std::move(module));
    }

    ParsedArchive parsed;
    bool parsedOk = false;
    if (b000ffSignatureLength(*file)) {
        parsedOk = parseB000ffArchive(file, &parsed);
    } else if (std::uint64_t(file->capacity()) <= kMaxMaterialisedImage) {
        parsedOk = parseArchive(file->readAll(), &parsed);
    }
    if (!parsedOk) {
        module->info_.error = QStringLiteral("No extractable Windows CE files found");
        return ModulePtr(std::move(module));
    }
    module->info_.description = parsed.description;
    if (!parsed.flat.empty())
        module->flat_ = std::make_shared<fs::MemoryStore>(std::move(parsed.flat));
    module->resources_ = std::move(parsed.entries);
    return ModulePtr(std::move(module));
}

} // namespace peare
