#include "LvmModule.h"
#include "Compat.h"

#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kSector = 512;
const std::uint32_t kInitialCrc = 0xf597a6cfU;

struct Area {
    Area() : offset(0), length(0) {}
    Area(std::uint64_t valueOffset, std::uint64_t valueLength)
        : offset(valueOffset), length(valueLength) {}

    std::uint64_t offset;
    std::uint64_t length;
};

struct PvInfo {
    QString uuid;
    Area dataArea;
    Area metadataArea;
};

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) |
           (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

void writeUuidPart(QString* out, const std::uint8_t* p, int n) {
    out->append(QString::fromLatin1(reinterpret_cast<const char*>(p), n));
}

QString readPvUuid(const std::uint8_t* p) {
    QString out;
    writeUuidPart(&out, p, 6); out += QLatin1Char('-');
    writeUuidPart(&out, p + 6, 4); out += QLatin1Char('-');
    writeUuidPart(&out, p + 10, 4); out += QLatin1Char('-');
    writeUuidPart(&out, p + 14, 4); out += QLatin1Char('-');
    writeUuidPart(&out, p + 18, 4); out += QLatin1Char('-');
    writeUuidPart(&out, p + 22, 4); out += QLatin1Char('-');
    writeUuidPart(&out, p + 26, 6);
    return out;
}

std::uint32_t lvmCrc(const std::uint8_t* p, std::size_t n) {
    static const std::uint32_t tab[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU
    };
    std::uint32_t crc = kInitialCrc;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ tab[crc & 0xfU];
        crc = (crc >> 4) ^ tab[crc & 0xfU];
    }
    return crc;
}

bool parsePv(const fs::ByteStorePtr& file, PvInfo* pv, QString* error) {
    for (int sector = 0; sector < 4; ++sector) {
        std::vector<std::uint8_t> label = file->readRange(std::int64_t(sector) * kSector, kSector);
        if (label.size() < kSector)
            break;
        if (std::memcmp(label.data(), "LABELONE", 8) != 0)
            continue;
        if (le64(label.data() + 8) != static_cast<std::uint64_t>(sector) ||
            std::memcmp(label.data() + 0x18, "LVM2 001", 8) != 0 ||
            le32(label.data() + 0x10) != lvmCrc(label.data() + 0x14, kSector - 0x14)) {
            if (error) *error = QStringLiteral("Invalid LVM physical volume label");
            return false;
        }
        const std::uint32_t headerOffset = le32(label.data() + 0x14);
        if (headerOffset >= kSector) {
            if (error) *error = QStringLiteral("Invalid LVM PV header offset");
            return false;
        }
        std::vector<std::uint8_t> sectorBytes = file->readRange(std::int64_t(sector) * kSector, kSector);
        const std::uint8_t* h = sectorBytes.data() + headerOffset;
        pv->uuid = readPvUuid(h);
        int off = 0x28;
        std::vector<Area> dataAreas;
        while (off + 16 <= kSector) {
            Area a{le64(h + off), le64(h + off + 8)};
            off += 16;
            if (a.offset == 0 && a.length == 0) break;
            dataAreas.push_back(a);
        }
        std::vector<Area> metaAreas;
        while (off + 16 <= kSector) {
            Area a{le64(h + off), le64(h + off + 8)};
            off += 16;
            if (a.offset == 0 && a.length == 0) break;
            metaAreas.push_back(a);
        }
        if (dataAreas.size() != 1 || metaAreas.empty()) {
            if (error) *error = QStringLiteral("Unsupported LVM PV area layout");
            return false;
        }
        pv->dataArea = dataAreas[0];
        pv->metadataArea = metaAreas[0];
        return true;
    }
    if (error) *error = QStringLiteral("LVM physical volume label not found");
    return false;
}

QString readMetadataText(const fs::ByteStorePtr& file, const Area& area, QString* error) {
    if (area.offset + area.length > static_cast<std::uint64_t>(file->capacity()) || area.length < kSector) {
        if (error) *error = QStringLiteral("Invalid LVM metadata area");
        return {};
    }
    std::vector<std::uint8_t> areaBytes = file->readRange(static_cast<std::int64_t>(area.offset),
                                                          static_cast<std::int64_t>(area.length));
    if (areaBytes.size() < area.length) {
        if (error) *error = QStringLiteral("Truncated LVM metadata area");
        return {};
    }
    if (std::memcmp(areaBytes.data() + 4, " LVM2 x[5A%r0N*>", 16) != 0) {
        if (error) *error = QStringLiteral("Invalid LVM metadata header");
        return {};
    }
    std::uint32_t locOff = 0x28;
    while (locOff + 0x18 <= areaBytes.size()) {
        const std::uint64_t off = le64(areaBytes.data() + locOff);
        const std::uint64_t len = le64(areaBytes.data() + locOff + 8);
        const std::uint32_t checksum = le32(areaBytes.data() + locOff + 0x10);
        const std::uint32_t flags = le32(areaBytes.data() + locOff + 0x14);
        locOff += 0x18;
        if (off == 0 && len == 0 && checksum == 0 && flags == 0)
            break;
        if ((flags & 1U) != 0)
            continue;
        if (off + len > areaBytes.size()) {
            if (error) *error = QStringLiteral("Invalid LVM raw metadata location");
            return {};
        }
        if (checksum != lvmCrc(areaBytes.data() + off, static_cast<std::size_t>(len))) {
            if (error) *error = QStringLiteral("Invalid LVM metadata checksum");
            return {};
        }
        return QString::fromLatin1(reinterpret_cast<const char*>(areaBytes.data() + off),
                                   static_cast<int>(len));
    }
    if (error) *error = QStringLiteral("LVM metadata text not found");
    return {};
}

QString removeComments(QString text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        int p = line.indexOf(QLatin1Char('#'));
        if (p >= 0) line.truncate(p);
    }
    return lines.join(QLatin1Char('\n'));
}

int sectionOpenEnd(const QString& text, int openBrace) {
    int depth = 0;
    for (int i = openBrace; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('{')) ++depth;
        else if (text[i] == QLatin1Char('}')) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return -1;
}

QString namedSection(const QString& text, const QString& name) {
    QRegularExpression re(QStringLiteral("(^|\\n)\\s*%1\\s*\\{").arg(QRegularExpression::escape(name)));
    QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return {};
    const int open = text.indexOf(QLatin1Char('{'), m.capturedStart());
    const int close = sectionOpenEnd(text, open);
    return close < 0 ? QString() : text.mid(open + 1, close - open - 1);
}

QString firstTopSectionName(const QString& text) {
    QRegularExpression re(QStringLiteral("(^|\\n)\\s*([A-Za-z0-9_+.-]+)\\s*\\{"));
    QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(2) : QString();
}

QString scalar(const QString& text, const QString& key) {
    QRegularExpression re(QStringLiteral("(^|\\n)\\s*%1\\s*=\\s*(.+)").arg(QRegularExpression::escape(key)));
    QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return {};
    QString value = m.captured(2).trimmed();
    if (value.endsWith(QLatin1Char(','))) value.chop(1);
    if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
        value = value.mid(1, value.size() - 2);
    return value.trimmed();
}

std::vector<QString> childSectionNames(const QString& text) {
    std::vector<QString> names;
    int pos = 0;
    QRegularExpression re(QStringLiteral("(^|\\n)\\s*([A-Za-z0-9_+.-]+)\\s*\\{"));
    while (pos < text.size()) {
        QRegularExpressionMatch m = re.match(text, pos);
        if (!m.hasMatch()) break;
        names.push_back(m.captured(2));
        const int open = text.indexOf(QLatin1Char('{'), m.capturedStart());
        const int close = sectionOpenEnd(text, open);
        if (close < 0) break;
        pos = close + 1;
    }
    return names;
}

struct SegmentInfo {
    std::uint64_t startExtent = 0;
    std::uint64_t extentCount = 0;
    QString pvName;
    std::uint64_t pvStartExtent = 0;
};

struct LvInfo {
    QString name;
    std::vector<SegmentInfo> segments;
};

std::vector<LvInfo> parseLogicalVolumes(const QString& metadata, std::uint64_t* extentSize) {
    std::vector<LvInfo> out;
    const QString vgName = firstTopSectionName(metadata);
    const QString vg = namedSection(metadata, vgName);
    bool ok = false;
    *extentSize = scalar(vg, QStringLiteral("extent_size")).toULongLong(&ok);
    if (!ok || *extentSize == 0) return out;
    const QString lvs = namedSection(vg, QStringLiteral("logical_volumes"));
    for (const QString& lvName : childSectionNames(lvs)) {
        const QString lvSec = namedSection(lvs, lvName);
        LvInfo lv;
        lv.name = vgName + QLatin1Char('/') + lvName;
        for (const QString& segName : childSectionNames(lvSec)) {
            const QString segSec = namedSection(lvSec, segName);
            if (scalar(segSec, QStringLiteral("type")) != QStringLiteral("striped"))
                continue;
            if (scalar(segSec, QStringLiteral("stripe_count")) != QStringLiteral("1"))
                continue;
            QRegularExpression stripeRe(QStringLiteral("stripes\\s*=\\s*\\[\\s*\"([^\"]+)\"\\s*,\\s*([0-9]+)\\s*\\]"));
            QRegularExpressionMatch sm = stripeRe.match(segSec);
            if (!sm.hasMatch())
                continue;
            SegmentInfo s;
            s.startExtent = scalar(segSec, QStringLiteral("start_extent")).toULongLong(&ok);
            if (!ok) continue;
            s.extentCount = scalar(segSec, QStringLiteral("extent_count")).toULongLong(&ok);
            if (!ok || s.extentCount == 0) continue;
            s.pvName = sm.captured(1);
            s.pvStartExtent = sm.captured(2).toULongLong(&ok);
            if (!ok) continue;
            lv.segments.push_back(s);
        }
        std::sort(lv.segments.begin(), lv.segments.end(),
                  [](const SegmentInfo& a, const SegmentInfo& b) { return a.startExtent < b.startExtent; });
        if (!lv.segments.empty())
            out.push_back(lv);
    }
    return out;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

}  // namespace

ModulePtr LvmModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<LvmModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::LVM;
    module->info_.description = QStringLiteral("Linux LVM2 physical volume");
    module->file_ = file;

    QString error;
    PvInfo pv;
    if (!parsePv(file, &pv, &error)) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }
    QString metadata = removeComments(readMetadataText(file, pv.metadataArea, &error));
    if (!error.isEmpty()) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }
    std::uint64_t extentSize = 0;
    const std::vector<LvInfo> lvs = parseLogicalVolumes(metadata, &extentSize);
    if (lvs.empty()) {
        module->info_.error = QStringLiteral("No supported LVM logical volumes found");
        return ModulePtr(std::move(module));
    }
    const std::uint64_t extentBytes = extentSize * kSector;
    for (const LvInfo& lv : lvs) {
        std::vector<fs::ByteStorePtr> parts;
        std::uint64_t next = 0;
        bool ok = true;
        std::uint64_t totalExtents = 0;
        for (const SegmentInfo& seg : lv.segments) {
            if (seg.startExtent != next || seg.pvStartExtent * extentBytes + seg.extentCount * extentBytes > pv.dataArea.length) {
                ok = false;
                break;
            }
            const std::uint64_t off = pv.dataArea.offset + seg.pvStartExtent * extentBytes;
            const std::uint64_t len = seg.extentCount * extentBytes;
            parts.push_back(std::make_shared<fs::SubStore>(file, static_cast<std::int64_t>(off),
                                                           static_cast<std::int64_t>(len)));
            next += seg.extentCount;
            totalExtents += seg.extentCount;
        }
        if (!ok || parts.empty())
            continue;
        ResourceEntry entry;
        entry.type = QStringLiteral("LVM_LOGICAL_VOLUME");
        entry.isEmbeddedFile = true;
        entry.name = lv.name;
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(totalExtents * extentBytes);
        entry.content = std::make_shared<fs::ConcatStore>(std::move(parts));
        module->resources_.push_back(std::move(entry));
    }
    if (module->resources_.empty())
        module->info_.error = QStringLiteral("No readable single-PV LVM logical volumes found");
    return ModulePtr(std::move(module));
}

ModulePtr LvmModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<LvmModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::LVM;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
