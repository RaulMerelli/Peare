#include "PeModuleCommon.h"

#include <utility>

namespace peare {
namespace detail {

void appendPeResources(QVector<ResourceEntry>& destination,
                       const PeResourceResult& parsed)
{
    destination.reserve(destination.size() + parsed.entries.size());
    for (const PeResourceEntry& source : parsed.entries) {
        ResourceEntry target;
        target.type = source.type;
        target.name = source.name;
        target.language = source.language;
        target.dataOffset = source.dataRva;
        target.dataSize = source.size;
        target.codePage = source.codePage;
        target.format = ModuleFormat::PE;
        target.data = source.data;
        target.hierarchyPath = QStringList() << QStringLiteral(".rsrc");
        destination.push_back(std::move(target));
    }
}


namespace {
quint16 peU16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}
quint32 peU32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
QString peSectionName(const QByteArray& d, qsizetype o, int index)
{
    QByteArray n = d.mid(o, 8);
    const int zero = n.indexOf('\0');
    if (zero >= 0) n.truncate(zero);
    const QString name = QString::fromLatin1(n);
    return name.isEmpty() ? QStringLiteral("section_%1").arg(index) : name;
}
}

void appendPeStructure(QVector<ResourceEntry>& destination,
                       const QByteArray& data, PeStorageLayout layout)
{
    if (data.size() < 0x40 || peU16(data, 0) != 0x5a4d) return;
    const quint32 nt = peU32(data, 0x3c);
    if (nt + 24 > quint32(data.size()) || peU32(data, nt) != 0x00004550) return;
    const quint16 count = peU16(data, nt + 6);
    const quint16 optionalSize = peU16(data, nt + 20);
    const qsizetype sectionTable = qsizetype(nt) + 24 + optionalSize;

    ResourceEntry headers;
    headers.type = QStringLiteral("PE_HEADERS");
    headers.name = QStringLiteral("headers");
    headers.hierarchyPath = QStringList() << headers.name;
    headers.language = QStringLiteral("neutral");
    headers.dataOffset = 0;
    const quint32 declaredHeaders = peU32(data, qsizetype(nt) + 24 + 60);
    headers.dataSize = qMin<quint64>(declaredHeaders ? declaredHeaders : quint64(sectionTable + count * 40), quint64(data.size()));
    headers.format = ModuleFormat::PE;
    headers.data = data.left(qsizetype(headers.dataSize));
    destination.push_back(std::move(headers));

    for (quint16 i = 0; i < count; ++i) {
        const qsizetype sh = sectionTable + qsizetype(i) * 40;
        if (sh + 40 > data.size()) break;
        const quint32 virtualSize = peU32(data, sh + 8);
        const quint32 virtualAddress = peU32(data, sh + 12);
        const quint32 rawSize = peU32(data, sh + 16);
        const quint32 rawPointer = peU32(data, sh + 20);
        const quint64 offset = layout == PeStorageLayout::LoadedImage ? virtualAddress : rawPointer;
        const quint64 requested = layout == PeStorageLayout::LoadedImage
            ? qMax<quint32>(virtualSize, rawSize) : rawSize;
        if (offset >= quint64(data.size())) continue;
        const quint64 available = qMin<quint64>(requested, quint64(data.size()) - offset);
        ResourceEntry section;
        section.type = QStringLiteral("PE_SECTION");
        section.name = peSectionName(data, sh, i);
        section.hierarchyPath = QStringList() << section.name;
        section.language = QStringLiteral("neutral");
        section.dataOffset = offset;
        section.dataSize = available;
        section.format = ModuleFormat::PE;
        section.data = data.mid(qsizetype(offset), qsizetype(available));
        destination.push_back(std::move(section));
    }
}

} // namespace detail
} // namespace peare
