#include "RT_GROUP_CURSOR.h"

#include "../RT_CURSOR/RT_CURSOR.h"

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

ResourcePreview RT_GROUP_CURSOR::preview(const ResourceEntry& entry, const IResourceResolver& resolver)
{
    ResourcePreview preview;

    if (entry.data.size() < 6)
    {
        preview.error = QStringLiteral("Invalid data");
        return preview;
    }

    QStringList lines;

    // CURSORDIR header
    const quint16 idReserved = ReadUInt16(entry.data, 0);
    const quint16 idType = ReadUInt16(entry.data, 2);
    const quint16 idCount = ReadUInt16(entry.data, 4);

    lines.append(QStringLiteral("RT_GROUP_CURSOR"));
    lines.append(QStringLiteral("{"));
    lines.append(QStringLiteral("\tReserved: %1").arg(idReserved));
    lines.append(QStringLiteral("\tType: %1 (2 = Cursor)").arg(idType));
    lines.append(QStringLiteral("\tCount: %1").arg(idCount));

    int offset = 6;

    for (int i = 0; i < idCount; ++i)
    {
        if (offset + 14 > entry.data.size())
        {
            lines.append(QStringLiteral("\tInvalid entry (truncated data)"));
            break;
        }

        // This is not what is documented, but it's made to get a result similar to Resource Hacker.
        const quint8 size = static_cast<quint8>(entry.data.at(offset));
        const quint8 reserved1 = static_cast<quint8>(entry.data.at(offset + 1));
        const quint8 colorCount = static_cast<quint8>(entry.data.at(offset + 2));
        const quint8 reserved2 = static_cast<quint8>(entry.data.at(offset + 3));
        const quint16 hotspotX = ReadUInt16(entry.data, offset + 4);
        const quint16 hotspotY = ReadUInt16(entry.data, offset + 6);
        const quint32 bytesInRes = ReadUInt32(entry.data, offset + 8);
        const quint16 nID = ReadUInt16(entry.data, offset + 12);

        lines.append(QStringLiteral("\tRT_CURSOR #%1").arg(nID));
        lines.append(QStringLiteral("\t{"));
        lines.append(QStringLiteral("\t\tWidth: %1").arg(size));
        lines.append(QStringLiteral("\t\tHeight: %1").arg(size));
        lines.append(QStringLiteral("\t\tReserved1: %1").arg(reserved1));
        lines.append(QStringLiteral("\t\tColorCount: %1").arg(size == 0 ? 0 : colorCount / size));
        lines.append(QStringLiteral("\t\tReserved2: %1").arg(reserved2));
        lines.append(QStringLiteral("\t\tHotspotX: %1").arg(hotspotX));
        lines.append(QStringLiteral("\t\tHotspotY: %1").arg(hotspotY));
        lines.append(QStringLiteral("\t\tBytesInRes: %1").arg(bytesInRes));
        lines.append(QStringLiteral("\t}"));

        if (const ResourceEntry* child = resolver.find(
                QStringLiteral("RT_CURSOR"),
                QStringLiteral("#%1").arg(nID),
                entry.language))
        {
            preview.images += RT_CURSOR::decode(child->data);
        }

        offset += 14;
    }

    lines.append(QStringLiteral("}"));
    preview.text = lines.join(QStringLiteral("\r\n"));
    return preview;
}

} // namespace resources
} // namespace peare
