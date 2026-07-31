#include "RT_NAMETABLE.h"

#include <QHash>
#include <QStringList>

namespace peare {
namespace resources {
namespace {

#pragma pack(push, 1)
struct RTNameTableHeader
{
    quint16 lengthEntry;    // WORD (2 bytes)
    quint16 resourceType;   // WORD (2 bytes)
    quint16 resourceId;     // WORD (2 bytes)
    quint8 paddingZero;     // BYTE (1 byte)
                            // CHAR szName[]; follows immediately
};
#pragma pack(pop)

static_assert(sizeof(RTNameTableHeader) == 7,
              "RTNameTableHeader must match the packed C# structure");

QString hexValue(qulonglong value, int width)
{
    return QStringLiteral("%1").arg(value, width, 16, QLatin1Char('0')).toUpper();
}

quint16 readUInt16(const QByteArray& data, qsizetype offset)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(bytes[0]) | (quint16(bytes[1]) << 8);
}

// Windows resource types
// Based on Win1 and Win2 prgref and WINMOD.C for Win3 by Matt Pietrek, 1992
// Win1: https://www.os2museum.com/files/docs/win10sdk/windows-1.03-sdk-prgref-1986.pdf
// Win2: https://www.os2museum.com/files/docs/win20sdk/windows-2.0-sdk-prgref-1987.pdf
const QHash<quint16, QString> WindowsNeResourceTypes = {
    {0x01, QStringLiteral("RT_CURSOR")},
    {0x02, QStringLiteral("RT_BITMAP")},
    {0x03, QStringLiteral("RT_ICON")},
    {0x04, QStringLiteral("RT_MENU")},
    {0x05, QStringLiteral("RT_DIALOG")},
    {0x06, QStringLiteral("RT_STRING")},
    {0x07, QStringLiteral("RT_FONTDIR")}, // FONTDIR do not exist in Win1 prgref, but it works?! Was it undocumented?
    {0x08, QStringLiteral("RT_FONT")},
    {0x09, QStringLiteral("RT_ACCELERATOR")},
    {0x0A, QStringLiteral("RT_RCDATA")},
    {0x0B, QStringLiteral("RT_MESSAGETABLE")}, // In WinMod by Matt Pietrek, 1992, File: WINMOD.C this is ErrorTable
    {0x0C, QStringLiteral("RT_GROUP_CURSOR")},
    {0x0D, QStringLiteral("RT_UNKNOWN(13)")},
    {0x0E, QStringLiteral("RT_GROUP_ICON")},
    {0x0F, QStringLiteral("RT_NAMETABLE")},
    {0x10, QStringLiteral("RT_VERSION")},
    {257,  QStringLiteral("RT_DRV_RAW")} // Not found in any docs, this is an addition by me, as 257 is always found only in drv files
};

} // namespace

QString RT_NAMETABLE::Get(const QByteArray& data)
{
    if (data.isEmpty())
    {
        // We'll just return an empty string or a header for context.
        return QStringLiteral("RT_NAMETABLE\n{\n}");
    }

    QStringList resultBuilder;
    resultBuilder.append(QStringLiteral("RT_NAMETABLE"));
    resultBuilder.append(QStringLiteral("{"));

    qsizetype offset = 0;
    constexpr qsizetype headerSize = sizeof(RTNameTableHeader); // Should be 7 bytes

    while (offset < data.size())
    {
        if (offset + headerSize > data.size())
        {
            break;
        }

        quint16 lengthEntry = readUInt16(data, offset);
        const quint16 resourceType = readUInt16(data, offset + 2);
        const quint16 resourceIdRaw = readUInt16(data, offset + 4); // Keep the raw value for comment
        const quint8 paddingZero = quint8(data.at(offset + 6));

        // Validate the padding byte
        if (paddingZero != 0x00)
        {
            resultBuilder.append(
                QStringLiteral("// WARNING: Expected padding byte to be 0x00 at offset 0x%1, but found 0x%2.")
                    .arg(hexValue(qulonglong(offset + 6), 4))
                    .arg(hexValue(paddingZero, 2)));
        }

        if (lengthEntry < headerSize)
        {
            break;
        }

        if (offset + lengthEntry > data.size())
        {
            resultBuilder.append(
                QStringLiteral("// ERROR: Declared length 0x%1 (%2 bytes) at offset 0x%3 exceeds available data (%4 bytes remaining). Truncated entry detected. Parsing what's available.")
                    .arg(hexValue(lengthEntry, 4))
                    .arg(lengthEntry)
                    .arg(hexValue(qulonglong(offset), 4))
                    .arg(data.size() - offset));
            lengthEntry = quint16(data.size() - offset); // Adjust lengthEntry to parse what's available
        }

        const qsizetype stringStartIndex = offset + headerSize;
        qsizetype stringEndIndex = -1;

        for (qsizetype i = stringStartIndex; i < offset + lengthEntry; ++i)
        {
            if (data.at(i) == '\0')
            {
                stringEndIndex = i;
                break;
            }
        }

        QString decodedName;
        if (stringEndIndex != -1)
        {
            const qsizetype stringLength = stringEndIndex - stringStartIndex;
            if (stringLength >= 0)
            {
                decodedName = QString::fromLatin1(data.constData() + stringStartIndex,
                                                  int(stringLength));
            }
        }
        else
        {
            if (stringStartIndex < offset + lengthEntry)
            {
                const qsizetype stringLength = (offset + lengthEntry) - stringStartIndex;
                if (stringLength > 0)
                {
                    decodedName = QString::fromLatin1(data.constData() + stringStartIndex,
                                                      int(stringLength));
                    while (decodedName.endsWith(QChar(u'\0')))
                        decodedName.chop(1);
                }
            }
        }

        // The resourceId contains both the ordinal and the 0x8000 flag.
        // For naming, the ordinal likely refers to its position in the NAMETABLE itself,
        // not necessarily the final resource ID.
        const quint16 ordinalId = quint16(resourceIdRaw & ~quint16(0x8000)); // Clear the 0x8000 bit

        // Determine a human-readable resource type from the constant
        const QString typeDescription = WindowsNeResourceTypes.value(
            resourceType, QStringLiteral("#%1").arg(resourceType));

        resultBuilder.append(
            QStringLiteral("  %1 #%2 = \"%3\"")
                .arg(typeDescription)
                .arg(ordinalId)
                .arg(decodedName));

        offset += lengthEntry;

    }

    resultBuilder.append(QStringLiteral("}"));
    return resultBuilder.join(QLatin1Char('\n'));
}

ResourcePreview RT_NAMETABLE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data);
    return preview;
}

} // namespace resources
} // namespace peare
