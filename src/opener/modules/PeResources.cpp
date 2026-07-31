#include "PeResources.h"

#include <QByteArray>
#include <QFile>
#include <QHash>

#include <functional>

namespace peare {
namespace {

quint16 u16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 u32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString typeName(quint32 id)
{
    static const QHash<quint32, QString> names = {
        {1,"RT_CURSOR"},{2,"RT_BITMAP"},{3,"RT_ICON"},{4,"RT_MENU"},{5,"RT_DIALOG"},
        {6,"RT_STRING"},{7,"RT_FONTDIR"},{8,"RT_FONT"},{9,"RT_ACCELERATOR"},{10,"RT_RCDATA"},
        {11,"RT_MESSAGETABLE"},{12,"RT_GROUP_CURSOR"},{14,"RT_GROUP_ICON"},{16,"RT_VERSION"},
        {17,"RT_DLGINCLUDE"},{19,"RT_PLUGPLAY"},{20,"RT_VXD"},{21,"RT_ANICURSOR"},
        {22,"RT_ANIICON"},{23,"RT_HTML"},{24,"RT_MANIFEST"}
    };
    return names.value(id, QStringLiteral("#%1").arg(id));
}

struct Section { quint32 va=0, raw=0, rawSize=0, virtualSize=0; };

qsizetype rvaToOffset(quint32 rva, const QVector<Section>& sections, PeStorageLayout layout, qsizetype dataSize)
{
    if (layout == PeStorageLayout::LoadedImage)
        return quint64(rva) < quint64(dataSize) ? qsizetype(rva) : -1;
    for (const auto& s : sections) {
        const quint32 span = qMax(s.rawSize, s.virtualSize);
        if (rva >= s.va && rva < s.va + span)
            return qsizetype(s.raw) + qsizetype(rva - s.va);
    }
    return -1;
}

QString readName(const QByteArray& d, qsizetype base, quint32 raw)
{
    if ((raw & 0x80000000u) == 0) return QStringLiteral("#%1").arg(raw & 0xffffu);
    const qsizetype off = base + qsizetype(raw & 0x7fffffffu);
    const quint16 len = u16(d, off);
    if (off + 2 + qsizetype(len) * 2 > d.size()) return QStringLiteral("<invalid>");
    QString out;
    out.reserve(len);
    for (quint16 i = 0; i < len; ++i) out.append(QChar(u16(d, off + 2 + i * 2)));
    return out;
}

} // namespace

PeResourceResult PeResources::listFile(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return {{}, f.errorString()};
    return list(f.readAll(), PeStorageLayout::File);
}

PeResourceResult PeResources::listFile(const QByteArray& data)
{
    return list(data, PeStorageLayout::File);
}

PeResourceResult PeResources::listImage(const QByteArray& image)
{
    return list(image, PeStorageLayout::LoadedImage);
}

PeResourceResult PeResources::list(const QByteArray& d, PeStorageLayout layout)
{

    if (d.size() < 0x100 || u16(d,0) != 0x5a4d) return {{}, QStringLiteral("Invalid MZ header")};

    const quint32 pe = u32(d,0x3c);
    if (pe + 24 > quint32(d.size()) || u32(d,pe) != 0x00004550) return {{}, QStringLiteral("Not a PE file")};

    const quint16 sectionCount = u16(d, pe + 6);
    const quint16 optionalSize = u16(d, pe + 20);
    const qsizetype opt = pe + 24;
    const quint16 magic = u16(d,opt);
    const qsizetype dataDir = opt + (magic == 0x20b ? 112 : 96);
    if (magic != 0x10b && magic != 0x20b) return {{}, QStringLiteral("Unsupported PE optional header")};

    const quint32 resourceRva = u32(d, dataDir + 2 * 8);
    const quint32 resourceSize = u32(d, dataDir + 2 * 8 + 4);
    if (!resourceRva || !resourceSize) return {};

    QVector<Section> sections;
    const qsizetype secBase = opt + optionalSize;
    for (quint16 i=0; i<sectionCount; ++i) {
        const qsizetype o = secBase + i * 40;
        if (o + 40 > d.size()) return {{}, QStringLiteral("Truncated section table")};
        Section section;
        section.va = u32(d, o + 12);
        section.raw = u32(d, o + 20);
        section.rawSize = u32(d, o + 16);
        section.virtualSize = u32(d, o + 8);
        sections.push_back(section);
    }

    const qsizetype base = rvaToOffset(resourceRva, sections, layout, d.size());
    if (base < 0 || base + 16 > d.size()) return {{}, QStringLiteral("Invalid resource directory RVA")};

    PeResourceResult result;
    const quint32 limit = qMin<quint32>(resourceSize, quint32(d.size() - base));

    typedef std::function<void(qsizetype)> ResourceEntryCallback;
    const std::function<bool(quint32, const ResourceEntryCallback&)> readDir =
        [&](quint32 rel, const ResourceEntryCallback& callback) -> bool {
        if (rel + 16 > limit) return false;
        const qsizetype o = base + rel;
        const quint32 count = quint32(u16(d,o+12)) + quint32(u16(d,o+14));
        if (rel + 16 + count * 8 > limit) return false;
        for (quint32 i=0;i<count;++i) callback(o + 16 + i*8);
        return true;
    };

    const bool ok = readDir(0, [&](qsizetype typeEntry){
        const quint32 typeRaw = u32(d,typeEntry);
        const quint32 typeTarget = u32(d,typeEntry+4);
        if ((typeTarget & 0x80000000u) == 0) return;
        const QString type = (typeRaw & 0x80000000u) ? readName(d,base,typeRaw) : typeName(typeRaw & 0xffffu);
        readDir(typeTarget & 0x7fffffffu, [&](qsizetype nameEntry){
            const quint32 nameRaw = u32(d,nameEntry);
            const quint32 nameTarget = u32(d,nameEntry+4);
            if ((nameTarget & 0x80000000u) == 0) return;
            const QString name = readName(d,base,nameRaw);
            readDir(nameTarget & 0x7fffffffu, [&](qsizetype langEntry){
                const quint32 langRaw = u32(d,langEntry);
                const quint32 dataTarget = u32(d,langEntry+4);
                if (dataTarget & 0x80000000u) return;
                if (dataTarget + 16 > limit) return;
                const qsizetype de = base + dataTarget;
                const quint32 dataRva = u32(d,de);
                const quint32 size = u32(d,de+4);
                const qsizetype fileOffset = rvaToOffset(dataRva, sections, layout, d.size());
                QByteArray raw;
                if (fileOffset >= 0 && fileOffset + qsizetype(size) <= d.size())
                    raw = d.mid(fileOffset, size);
                PeResourceEntry entry;
                entry.type = type;
                entry.name = name;
                entry.language = QStringLiteral("#%1").arg(langRaw & 0xffffu);
                entry.dataRva = dataRva;
                entry.size = size;
                entry.codePage = u32(d, de + 8);
                entry.data = raw;
                result.entries.push_back(entry);
            });
        });
    });

    if (!ok) result.error = QStringLiteral("Corrupt PE resource directory");
    return result;
}

} // namespace peare
