#include "XuizModule.h"
#include "Compat.h"

#include <QFile>
#include <QtEndian>
#include <QFileInfo>
#include <QStringList>
#include <utility>

namespace peare {
namespace xuiz {
struct Entry { QString path; quint32 offset = 0; quint32 size = 0; QByteArray data; };
struct Archive { bool valid = false; quint32 flags = 0; quint32 declaredSize = 0; quint32 unknown = 0; quint32 dataPointer = 0; QVector<Entry> entries; QString error; };

namespace {
quint16 xuizBe16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + o));
}
quint32 xuizBe32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + o));
}
QString normalizeXuizPath(QByteArray raw)
{
    const int nul = raw.indexOf('\0');
    if (nul >= 0) raw.truncate(nul);
    QString path = QString::fromLatin1(raw).trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList safe;
    for (const QString& part : parts) {
        if (part == QStringLiteral(".") || part.isEmpty()) continue;
        safe.push_back(part == QStringLiteral("..") ? QStringLiteral("_") : part);
    }
    return safe.join(QLatin1Char('/'));
}
}

Archive parse(const QByteArray& data)
{
    Archive out;
    if (data.size() < 22 || data.left(4) != QByteArrayLiteral("XUIZ")) {
        out.error = QStringLiteral("Invalid XUIZ signature or truncated header");
        return out;
    }
    out.flags = xuizBe32(data, 4);
    out.declaredSize = xuizBe32(data, 8);
    out.unknown = xuizBe32(data, 12);
    out.dataPointer = xuizBe32(data, 16);
    const quint16 count = xuizBe16(data, 20);
    if (out.declaredSize != quint32(data.size())) { out.error = QStringLiteral("XUIZ size mismatch"); return out; }
    const quint64 payloadBase = 22u + out.dataPointer;
    if (payloadBase > quint64(data.size())) { out.error = QStringLiteral("XUIZ data pointer outside input"); return out; }
    qsizetype pos = 22;
    out.entries.reserve(count);
    for (quint16 index = 0; index < count; ++index) {
        if (pos + 9 > data.size() || quint64(pos + 9) > payloadBase) { out.error = QStringLiteral("Truncated XUIZ directory entry %1").arg(index); out.entries.clear(); return out; }
        const quint32 size = xuizBe32(data, pos);
        const quint32 relativeOffset = xuizBe32(data, pos + 4);
        const quint8 nameLength = quint8(data.at(pos + 8));
        pos += 9;
        if (nameLength == 0 || pos + nameLength > data.size() || quint64(pos + nameLength) > payloadBase) { out.error = QStringLiteral("Invalid XUIZ name in entry %1").arg(index); out.entries.clear(); return out; }
        const QString path = normalizeXuizPath(data.mid(pos, nameLength));
        pos += nameLength;
        const quint64 absoluteOffset = payloadBase + relativeOffset;
        if (path.isEmpty() || absoluteOffset + size > quint64(data.size())) { out.error = QStringLiteral("XUIZ entry %1 lies outside the archive").arg(index); out.entries.clear(); return out; }
        Entry entry; entry.path = path; entry.offset = quint32(absoluteOffset); entry.size = size; entry.data = data.mid(qsizetype(absoluteOffset), int(size));
        out.entries.push_back(std::move(entry));
    }
    out.valid = true;
    return out;
}
} // namespace xuiz

namespace {
QStringList parentPath(const QString& path)
{
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}
QString leafName(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}
}

std::unique_ptr<XuizModule> XuizModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<XuizModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::XUIZ;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<XuizModule> XuizModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<XuizModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::XUIZ;
    module->info_.description = QStringLiteral("Xbox 360 XUIZ archive");
    const xuiz::Archive archive = xuiz::parse(data);
    if (!archive.valid) {
        module->info_.error = archive.error;
        return module;
    }
    for (const xuiz::Entry& source : archive.entries) {
        ResourceEntry entry;
        entry.type = QStringLiteral("XUIZ_FILE");
        entry.name = leafName(source.path);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = source.offset;
        entry.dataSize = source.size;
        entry.format = ModuleFormat::XUIZ;
        entry.hierarchyPath = parentPath(source.path);
        entry.hierarchyPath.push_back(entry.name);
        entry.data = source.data;
        module->resources_.push_back(std::move(entry));
    }
    return module;
}

} // namespace peare
