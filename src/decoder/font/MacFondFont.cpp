#include "MacFondFont.h"
#include "BitmapFont.h"
#include "AmigaAnimFont.h"

namespace peare {
namespace font {
namespace {

quint16 readBe16(const QByteArray& data, int offset)
{
    return (quint16(uchar(data.at(offset))) << 8) | quint16(uchar(data.at(offset + 1)));
}

quint32 readBe32(const QByteArray& data, int offset)
{
    return (quint32(uchar(data.at(offset))) << 24) |
           (quint32(uchar(data.at(offset + 1))) << 16) |
           (quint32(uchar(data.at(offset + 2))) << 8) |
           quint32(uchar(data.at(offset + 3)));
}

bool rangeFits(quint64 offset, quint64 length, quint64 size)
{
    return offset <= size && length <= size - offset;
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool tableOffsetFits(quint32 offset, quint64 minimum, int tableEnd, int size)
{
    return !offset || (offset >= quint32(tableEnd) &&
                       rangeFits(offset, minimum, quint64(size)));
}

bool parse(const QByteArray& data, MacFondFont* out, QString* error)
{
    const bool amigaContents = data.size() >= 2 && quint8(data.at(0)) == 0x0F &&
        (quint8(data.at(1)) == 0x00 || quint8(data.at(1)) == 0x02 ||
         quint8(data.at(1)) == 0x03);
    const bool amigaStrike = data.size() >= 4 && readBe32(data, 0) == 0x000003F3u;
    if (hasPsfHeader(data) || hasVfontHeader(data) || amigaContents || amigaStrike ||
        hasAmigaAnimFontHeader(data))
        return fail(error, QStringLiteral("Input is not a Macintosh FOND resource"));
    if (!out || data.size() < 54 || data.size() > 16 * 1024 * 1024)
        return fail(error, QStringLiteral("Truncated or oversized Macintosh FOND resource"));

    MacFondFont parsed;
    parsed.flags = readBe16(data, 0);
    parsed.familyId = readBe16(data, 2);
    parsed.firstCharacter = readBe16(data, 4);
    parsed.lastCharacter = readBe16(data, 6);
    const quint32 widthOffset = readBe32(data, 16);
    const quint32 kerningOffset = readBe32(data, 20);
    const quint32 styleOffset = readBe32(data, 24);
    parsed.version = readBe16(data, 50);

    const quint16 associationCountMinusOne = readBe16(data, 52);
    const quint32 associationCount = quint32(associationCountMinusOne) + 1u;
    const quint64 associationBytes = quint64(associationCount) * 6u;
    const quint64 associationEnd = 54u + associationBytes;
    if (parsed.familyId > 32767 || parsed.firstCharacter > parsed.lastCharacter ||
        associationCount > 4096 || !rangeFits(54, associationBytes, quint64(data.size())))
        return fail(error, QStringLiteral("Invalid Macintosh FOND family or association table"));

    parsed.associations.reserve(int(associationCount));
    for (quint32 i = 0; i < associationCount; ++i) {
        const int offset = 54 + int(i * 6u);
        MacFondAssociation association;
        association.pointSize = readBe16(data, offset);
        association.style = readBe16(data, offset + 2);
        association.resourceId = qint16(readBe16(data, offset + 4));
        parsed.associations.append(association);
    }

    if (!tableOffsetFits(widthOffset, 2, int(associationEnd), data.size()) ||
        !tableOffsetFits(kerningOffset, 4, int(associationEnd), data.size()) ||
        !tableOffsetFits(styleOffset, 58, int(associationEnd), data.size()))
        return fail(error, QStringLiteral("Invalid Macintosh FOND table offset"));

    if (widthOffset) {
        const quint32 widthCount = quint32(readBe16(data, int(widthOffset))) + 1u;
        const quint64 characterCount = quint64(parsed.lastCharacter) -
                                       parsed.firstCharacter + 1u;
        const quint64 widthEntryBytes = (characterCount + 4u) * 2u;
        const quint64 widthTableBytes = 2u + quint64(widthCount) * widthEntryBytes;
        if (widthCount > 4096 || !rangeFits(widthOffset, widthTableBytes,
                                            quint64(data.size())))
            return fail(error, QStringLiteral("Invalid Macintosh FOND width table"));
        parsed.widthTableCount = int(widthCount);
    }

    if (styleOffset) {
        const quint32 encodingOffset = readBe32(data, int(styleOffset) + 2);
        if (encodingOffset &&
            (encodingOffset < 58 || !rangeFits(quint64(styleOffset) + encodingOffset,
                                                2, quint64(data.size()))))
            return fail(error, QStringLiteral("Invalid Macintosh FOND encoding-table offset"));
    }

    *out = parsed;
    return true;
}

} // namespace

bool hasMacFondHeader(const QByteArray& data)
{
    MacFondFont font;
    return parse(data, &font, nullptr);
}

bool parseMacFond(const QByteArray& data, MacFondFont* font, QString* error)
{
    if (!parse(data, font, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Invalid or unsupported Macintosh FOND resource");
        return false;
    }
    return true;
}

} // namespace font
} // namespace peare
