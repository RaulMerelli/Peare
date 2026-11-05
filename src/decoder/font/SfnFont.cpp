#include "SfnFont.h"

#include <QTextCodec>
#include <QtGlobal>
#include <cstring>

namespace peare {
namespace font {
namespace {

const quint32 kUnicodeLimit = 0x110000;
const int kMaximumFontBytes = 64 * 1024 * 1024;
const int kMaximumGlyphs = 262144;
const int kMaximumLayers = 1000000;
const int kMaximumCollectionFonts = 1024;

quint16 readLe16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) | (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 readLe24(const QByteArray& data, int offset)
{
    return quint32(quint8(data.at(offset))) | (quint32(quint8(data.at(offset + 1))) << 8) |
        (quint32(quint8(data.at(offset + 2))) << 16);
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return quint32(quint8(data.at(offset))) | (quint32(quint8(data.at(offset + 1))) << 8) |
        (quint32(quint8(data.at(offset + 2))) << 16) | (quint32(quint8(data.at(offset + 3))) << 24);
}

bool readString(const QByteArray& data, int* position, int limit, QString* value)
{
    if (*position >= limit)
        return false;
    const int start = *position;
    while (*position < limit && data.at(*position) != '\0') {
        if (quint8(data.at(*position)) < 0x20 || *position - start >= 255)
            return false;
        ++*position;
    }
    if (*position >= limit)
        return false;
    const QByteArray utf8 = data.mid(start, *position - start);
    ++*position;
    *value = QString::fromUtf8(utf8);
    return value->toUtf8() == utf8;
}

bool parseFragment(const QByteArray& data, quint32 offset, quint32 limit,
                   quint8 fontWidth, quint8 fontHeight, SfnFragment* fragment)
{
    if (offset >= limit || limit > quint32(data.size()))
        return false;
    int position = int(offset);
    const quint8 first = quint8(data.at(position++));
    SfnFragment parsed;
    if ((first & 0x80) == 0) {
        parsed.type = SfnFragmentType::Contour;
        quint32 commandCount = (first & 0x3f) + 1;
        if (first & 0x40) {
            if (position >= int(limit)) return false;
            commandCount = (((first & 0x3f) << 8) | quint8(data.at(position++))) + 1;
        }
        if (commandCount > 16384)
            return false;
        const quint32 commandBytes = (commandCount + 3) / 4;
        if (quint64(position) + commandBytes > limit)
            return false;
        const int commandOffset = position;
        position += int(commandBytes);
        parsed.commands.reserve(int(commandCount));
        for (quint32 i = 0; i < commandCount; ++i) {
            const quint8 type = (quint8(data.at(commandOffset + int(i / 4))) >> ((i % 4) * 2)) & 3;
            if (i && type == 0)
                break;
            const int argumentBytes = type < 2 ? 2 : type == 2 ? 4 : 6;
            if (quint64(position) + argumentBytes > limit)
                return false;
            SfnContourCommand command;
            command.type = type;
            command.end.x = quint8(data.at(position));
            command.end.y = quint8(data.at(position + 1));
            if (type >= 2) {
                command.control1.x = quint8(data.at(position + 2));
                command.control1.y = quint8(data.at(position + 3));
            }
            if (type == 3) {
                command.control2.x = quint8(data.at(position + 4));
                command.control2.y = quint8(data.at(position + 5));
            }
            if (command.end.x > fontWidth || command.end.y > fontHeight ||
                (type >= 2 && (command.control1.x > fontWidth || command.control1.y > fontHeight)) ||
                (type == 3 && (command.control2.x > fontWidth || command.control2.y > fontHeight)))
                return false;
            parsed.commands.push_back(command);
            position += argumentBytes;
        }
        if (parsed.commands.isEmpty() || parsed.commands.first().type != 0)
            return false;
    } else if ((first & 0xe0) == 0x80) {
        parsed.type = SfnFragmentType::Bitmap;
        parsed.width = quint16(((first & 0x1f) + 1) * 8);
        if (position >= int(limit)) return false;
        parsed.height = quint16(quint8(data.at(position++)) + 1);
        const quint32 bytes = quint32(parsed.width / 8) * parsed.height;
        if (quint64(position) + bytes > limit)
            return false;
        parsed.pixels = data.mid(position, int(bytes));
    } else if ((first & 0xe0) == 0xa0) {
        parsed.type = SfnFragmentType::Pixmap;
        if (position + 3 > int(limit)) return false;
        const quint32 compressedSize = (((first & 0x1f) << 8) | quint8(data.at(position++))) + 1;
        parsed.width = quint16(quint8(data.at(position++)) + 1);
        parsed.height = quint16(quint8(data.at(position++)) + 1);
        const quint64 pixelCount = quint64(parsed.width) * parsed.height;
        if (compressedSize > 8192 || quint64(position) + compressedSize > limit || pixelCount > 65536)
            return false;
        const int end = position + int(compressedSize);
        parsed.pixels.reserve(int(pixelCount));
        while (position < end && parsed.pixels.size() < int(pixelCount)) {
            const quint8 packet = quint8(data.at(position++));
            const int count = (packet & 0x7f) + 1;
            if (parsed.pixels.size() + count > int(pixelCount)) return false;
            if (packet & 0x80) {
                if (position >= end) return false;
                parsed.pixels.append(QByteArray(count, data.at(position++)));
            } else {
                if (position + count > end) return false;
                parsed.pixels.append(data.constData() + position, count);
                position += count;
            }
        }
        if (position != end || parsed.pixels.size() != int(pixelCount))
            return false;
    } else if ((first & 0xe0) == 0xc0) {
        parsed.type = SfnFragmentType::Other;
        if (position >= int(limit)) return false;
        const quint32 count = (((first & 0x03) << 8) | quint8(data.at(position++))) + 1;
        if (quint64(position) + quint64(count) * 8 > limit) return false;
        position += int(count * 8);
    } else {
        parsed.type = SfnFragmentType::Other;
        const quint32 count = first & 0x1f;
        if (quint64(position) + count > limit) return false;
        position += int(count);
    }
    *fragment = parsed;
    return true;
}

} // namespace

bool hasSfnHeader(const QByteArray& data)
{
    return (data.size() >= 32 && data.startsWith("SFN2")) ||
        (data.size() >= 8 && data.startsWith("SFNC"));
}

bool parseSfnCollection(const QByteArray& data, QVector<QByteArray>* fonts, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!fonts || data.size() < 8 || data.size() > kMaximumFontBytes ||
        !data.startsWith("SFNC") || readLe32(data, 4) != quint32(data.size()))
        return fail(QStringLiteral("Invalid SFN collection header or size"));

    QVector<QByteArray> parsed;
    int position = 8;
    while (position < data.size()) {
        if (parsed.size() >= kMaximumCollectionFonts || data.size() - position < 8 ||
            data.mid(position, 4) != QByteArrayLiteral("SFN2"))
            return fail(QStringLiteral("Invalid or excessive SFN collection member"));
        const quint32 memberSize = readLe32(data, position + 4);
        if (memberSize < 36 || memberSize > quint32(data.size() - position))
            return fail(QStringLiteral("Truncated SFN collection member"));
        const QByteArray member = data.mid(position, int(memberSize));
        SfnFont font;
        if (!parseSfnFont(member, &font, nullptr))
            return fail(QStringLiteral("Invalid SFN collection member"));
        parsed.push_back(member);
        position += int(memberSize);
    }
    if (parsed.isEmpty())
        return fail(QStringLiteral("SFN collection has no fonts"));
    *fonts = parsed;
    if (error) error->clear();
    return true;
}

bool parseSfnFont(const QByteArray& data, SfnFont* font, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!font || data.size() < 36 || data.size() > kMaximumFontBytes || !data.startsWith("SFN2"))
        return fail(QStringLiteral("Invalid SFN2 header or input size"));
    const quint32 fontSize = readLe32(data, 4);
    const quint8 type = quint8(data.at(8));
    const quint8 revision = quint8(data.at(9));
    const quint8 width = quint8(data.at(10));
    const quint8 height = quint8(data.at(11));
    const quint8 baseline = quint8(data.at(12));
    const quint8 underline = quint8(data.at(13));
    const quint32 fragmentsOffset = readLe16(data, 14);
    const quint32 charactersOffset = readLe32(data, 16);
    const quint32 ligatureOffset = readLe32(data, 20);
    const quint32 kerningOffset = readLe32(data, 24);
    const quint32 colorMapOffset = readLe32(data, 28);
    const quint32 dataEnd = fontSize >= 4 ? fontSize - 4 : 0;
    if (fontSize != quint32(data.size()) || revision != 0 || (type & 0x0f) > 4 ||
        width == 0 || height == 0 || baseline > height || fragmentsOffset < 32 ||
        charactersOffset <= fragmentsOffset || charactersOffset >= dataEnd ||
        std::memcmp(data.constData() + dataEnd, "2NFS", 4) != 0)
        return fail(QStringLiteral("Invalid SFN2 dimensions, offsets, revision, or end marker"));
    const quint32 optionalOffsets[] = {ligatureOffset, kerningOffset, colorMapOffset};
    for (quint32 offset : optionalOffsets)
        if (offset && (offset < charactersOffset || offset >= dataEnd))
            return fail(QStringLiteral("Invalid SFN2 optional table offset"));

    SfnFont parsed;
    parsed.type = type;
    parsed.width = width;
    parsed.height = height;
    parsed.baseline = baseline;
    parsed.underline = underline;
    int stringPosition = 32;
    QString* strings[] = {&parsed.uniqueName, &parsed.familyName, &parsed.subfamilyName,
        &parsed.version, &parsed.designer, &parsed.license};
    for (QString* value : strings)
        if (!readString(data, &stringPosition, int(fragmentsOffset), value))
            return fail(QStringLiteral("Invalid SFN2 string table"));
    if (stringPosition > int(fragmentsOffset))
        return fail(QStringLiteral("SFN2 string table overlaps fragment data"));
    const QString faceName = parsed.familyName.isEmpty() ? parsed.uniqueName : parsed.familyName;
    parsed.familyName = faceName;

    if (colorMapOffset) {
        const quint32 colorBytes = dataEnd - colorMapOffset;
        if (colorBytes % 4 || colorBytes / 4 > 254)
            return fail(QStringLiteral("Invalid SFN2 color map size"));
        parsed.colors.reserve(int(colorBytes / 4));
        for (quint32 i = 0; i < colorBytes; i += 4) {
            SfnColor color;
            color.blue = quint8(data.at(colorMapOffset + i));
            color.green = quint8(data.at(colorMapOffset + i + 1));
            color.red = quint8(data.at(colorMapOffset + i + 2));
            color.alpha = quint8(data.at(colorMapOffset + i + 3));
            parsed.colors.push_back(color);
        }
    }

    quint32 characterEnd = dataEnd;
    for (quint32 offset : optionalOffsets)
        if (offset && offset > charactersOffset)
            characterEnd = qMin(characterEnd, offset);
    QHash<quint32, quint32> fragmentIndexes;
    quint32 codepoint = 0;
    int position = int(charactersOffset);
    quint64 totalLayers = 0;
    while (codepoint < kUnicodeLimit) {
        if (position >= int(characterEnd))
            return fail(QStringLiteral("Truncated SFN2 character table"));
        const quint8 attributes = quint8(data.at(position));
        if (attributes & 0x80) {
            quint32 skipped;
            if (attributes == 0xff) {
                skipped = 65536;
                ++position;
            } else if ((attributes & 0xc0) == 0xc0) {
                if (position + 1 >= int(characterEnd))
                    return fail(QStringLiteral("Truncated SFN2 Unicode skip run"));
                skipped = (((attributes & 0x3f) << 8) | quint8(data.at(position + 1))) + 1;
                position += 2;
            } else {
                skipped = (attributes & 0x3f) + 1;
                ++position;
            }
            if (codepoint + skipped == kUnicodeLimit + 1 && position == int(characterEnd)) {
                codepoint = kUnicodeLimit;
                break;
            }
            if (codepoint + skipped > kUnicodeLimit)
                return fail(QStringLiteral("SFN2 Unicode skip exceeds the codepoint range"));
            codepoint += skipped;
            continue;
        }
        if (position + 6 > int(characterEnd) || parsed.glyphs.size() >= kMaximumGlyphs)
            return fail(QStringLiteral("Invalid or excessive SFN2 glyph table"));
        SfnGlyph glyph;
        glyph.codepoint = codepoint++;
        glyph.overlap = attributes & 0x3f;
        const bool wideOffsets = attributes & 0x40;
        const quint8 layerCount = quint8(data.at(position + 1));
        glyph.width = quint8(data.at(position + 2));
        glyph.height = quint8(data.at(position + 3));
        glyph.advanceX = quint8(data.at(position + 4));
        glyph.advanceY = quint8(data.at(position + 5));
        position += 6;
        const int descriptorSize = wideOffsets ? 6 : 5;
        if (quint64(position) + quint64(layerCount) * descriptorSize > characterEnd ||
            totalLayers + layerCount > kMaximumLayers)
            return fail(QStringLiteral("Truncated or excessive SFN2 fragment descriptors"));
        totalLayers += layerCount;
        int currentColor = -1;
        for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
            const quint8 x = quint8(data.at(position));
            const quint8 y = quint8(data.at(position + 1));
            if (x == 0xff && y == 0xff) {
                const quint8 color = quint8(data.at(position + 2));
                const int paddingBytes = wideOffsets ? 3 : 2;
                if (color > 253 || color >= parsed.colors.size())
                    return fail(QStringLiteral("Invalid SFN2 color command"));
                for (int i = 0; i < paddingBytes; ++i)
                    if (data.at(position + 3 + i) != 0)
                        return fail(QStringLiteral("Invalid SFN2 color command padding"));
                currentColor = color;
                position += descriptorSize;
                continue;
            }
            const quint32 fragmentOffset = wideOffsets ? readLe32(data, position + 2)
                                                        : readLe24(data, position + 2);
            if (fragmentOffset < fragmentsOffset || fragmentOffset >= charactersOffset)
                return fail(QStringLiteral("SFN2 fragment offset is outside the fragment area"));
            quint32 fragmentIndex;
            const auto existing = fragmentIndexes.constFind(fragmentOffset);
            if (existing != fragmentIndexes.constEnd()) {
                fragmentIndex = existing.value();
            } else {
                SfnFragment fragment;
                if (!parseFragment(data, fragmentOffset, charactersOffset, width, height, &fragment))
                    return fail(QStringLiteral("Invalid SFN2 fragment"));
                fragmentIndex = quint32(parsed.fragments.size());
                if (fragmentIndex >= kMaximumLayers)
                    return fail(QStringLiteral("Excessive SFN2 fragment count"));
                parsed.fragments.push_back(fragment);
                fragmentIndexes.insert(fragmentOffset, fragmentIndex);
            }
            const SfnFragmentType fragmentType = parsed.fragments.at(int(fragmentIndex)).type;
            if (fragmentType == SfnFragmentType::Pixmap && !colorMapOffset)
                return fail(QStringLiteral("SFN2 pixmap has no color map"));
            if (fragmentType == SfnFragmentType::Pixmap) {
                const QByteArray& pixels = parsed.fragments.at(int(fragmentIndex)).pixels;
                for (char pixel : pixels)
                    if (quint8(pixel) < 254 && quint8(pixel) >= parsed.colors.size())
                        return fail(QStringLiteral("SFN2 pixmap references a missing color"));
            }
            SfnLayer layer;
            layer.x = x;
            layer.y = y;
            layer.fragment = fragmentIndex;
            layer.color = qint16(currentColor);
            glyph.layers.push_back(layer);
            position += descriptorSize;
        }
        const int glyphIndex = parsed.glyphs.size();
        parsed.glyphs.push_back(glyph);
        parsed.glyphByCodepoint.insert(glyph.codepoint, glyphIndex);
    }
    if (parsed.glyphs.isEmpty())
        return fail(QStringLiteral("SFN2 font has no glyphs"));
    *font = parsed;
    if (error) error->clear();
    return true;
}

} // namespace font
} // namespace peare
