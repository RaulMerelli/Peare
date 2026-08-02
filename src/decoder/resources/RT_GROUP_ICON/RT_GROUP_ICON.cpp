#include "RT_GROUP_ICON.h"

#include "../RT_ICON/RT_ICON.h"

#include <QDebug>
#include <QStringList>
#include <QtEndian>

namespace peare {
namespace resources {
namespace {

quint16 ReadUInt16(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 ReadUInt32(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

} // namespace

ResourcePreview RT_GROUP_ICON::preview(const ResourceEntry& entry, const IResourceResolver& resolver)
{
    ResourcePreview preview;

    if (entry.data.size() < 6)
    {
        preview.error = QStringLiteral("Invalid data");
        return preview;
    }

    QStringList lines;

    // ICONDIR
    const quint16 idReserved = ReadUInt16(entry.data, 0); // 0x00
    const quint16 idType = ReadUInt16(entry.data, 2);     // 0x01
    const quint16 idCount = ReadUInt16(entry.data, 4);    // 0x02

    lines.append(QStringLiteral("RT_GROUP_ICON"));
    lines.append(QStringLiteral("{"));
    lines.append(QStringLiteral("\tReserved: %1").arg(idReserved));
    lines.append(QStringLiteral("\tType: %1 (1 = Icon)").arg(idType));
    lines.append(QStringLiteral("\tCount: %1").arg(idCount));

    int offset = 6;

    for (int i = 0; i < idCount; ++i)
    {
        if (offset + 14 > entry.data.size())
        {
            lines.append(QStringLiteral("  [!] GRPICONDIRENTRY %1 incomplete.").arg(i));
            break;
        }

        const quint8 bWidth = static_cast<quint8>(entry.data.at(offset));
        const quint8 bHeight = static_cast<quint8>(entry.data.at(offset + 1));
        const quint8 bColorCount = static_cast<quint8>(entry.data.at(offset + 2));
        const quint8 bReserved = static_cast<quint8>(entry.data.at(offset + 3));
        Q_UNUSED(bReserved);
        const quint16 wPlanes = ReadUInt16(entry.data, offset + 4);
        const quint16 wBitCount = ReadUInt16(entry.data, offset + 6);
        const quint32 dwBytesInRes = ReadUInt32(entry.data, offset + 8);
        const quint16 nID = ReadUInt16(entry.data, offset + 12);

        lines.append(QStringLiteral("\tRT_ICON #%1").arg(nID));
        lines.append(QStringLiteral("\t{"));
        lines.append(QStringLiteral("\t\tSize: %1x%2 px").arg(bWidth).arg(bHeight));
        lines.append(QStringLiteral("\t\tColors: %1")
                         .arg(bColorCount == 0 ? QStringLiteral(">8bpp") : QString::number(bColorCount)));
        lines.append(QStringLiteral("\t\tPlanes: %1").arg(wPlanes));
        lines.append(QStringLiteral("\t\tBitCount: %1").arg(wBitCount));
        lines.append(QStringLiteral("\t\tBytes in Resource: %1").arg(dwBytesInRes));
        lines.append(QStringLiteral("\t}"));

        if (const ResourceEntry* child = resolver.find(
                QStringLiteral("RT_ICON"),
                QStringLiteral("#%1").arg(nID),
                entry.language))
        {
            preview.images += RT_ICON::decode(child->data);
        }

        offset += 14;
    }

    lines.append(QStringLiteral("}"));
    preview.text = lines.join(QStringLiteral("\r\n"));
    return preview;
}

} // namespace resources
} // namespace peare
