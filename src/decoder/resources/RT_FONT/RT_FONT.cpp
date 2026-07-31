#include "RT_FONT.h"
#include <QSharedPointer>
#include <QMutexLocker>
#include <QMutex>
#include <QHash>
#include <QCryptographicHash>
#include "OS2_RT_FONT.h"

#include <QImage>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QPen>
#include <QTextCodec>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <limits>

namespace peare {
namespace resources {
namespace {

quint16 u16(const QByteArray& d, int o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 u32(const QByteArray& d, int o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString readNullTerminatedAnsi(const QByteArray& data, int offset)
{
    if (offset < 0 || offset >= data.size())
        return {};
    int end = offset;
    while (end < data.size() && data[end] != '\0')
        ++end;
    return QTextCodec::codecForLocale()->toUnicode(data.constData() + offset, end - offset);
}

struct Header
{
    quint16 version = 0;
    quint32 size = 0;
    quint16 type = 0;
    quint16 points = 0;
    quint16 ascent = 0;
    quint16 externalLeading = 0;
    quint16 pixWidth = 0;
    quint16 pixHeight = 0;
    quint16 avgWidth = 0;
    quint16 maxWidth = 0;
    quint8 pitchAndFamily = 0;
    quint8 firstChar = 0;
    quint8 lastChar = 0;
    quint8 defaultChar = 0;
    quint8 breakChar = 0;
    quint8 charSet = 0;
    quint16 bSpace = 0;
    quint16 widthBytes = 0;
    quint32 face = 0;
    quint32 bitsOffset = 0;
};

bool parseHeader(const QByteArray& data, Header& h)
{
    if (data.size() < 118)
        return false;
    h.version = u16(data, 0);
    h.size = u32(data, 2);
    h.type = u16(data, 66);
    h.points = u16(data, 68);
    h.ascent = u16(data, 74);
    h.externalLeading = u16(data, 80);
    h.pixWidth = u16(data, 86);
    h.pixHeight = u16(data, 88);
    h.charSet = quint8(data[85]);
    h.pitchAndFamily = quint8(data[90]);
    h.avgWidth = u16(data, 91);
    h.maxWidth = u16(data, 93);
    h.firstChar = quint8(data[95]);
    h.lastChar = quint8(data[96]);
    h.defaultChar = quint8(data[97]);
    h.breakChar = quint8(data[98]);
    h.widthBytes = u16(data, 99);
    h.face = u32(data, 105);
    h.bitsOffset = u32(data, 113);
    if (h.version >= 0x0300 && data.size() >= 148)
        h.bSpace = u16(data, 123);

    return (h.version == 0x0100 || h.version == 0x0200 || h.version == 0x0300) &&
           h.lastChar >= h.firstChar && h.pixHeight > 0 && h.pixHeight <= 4096 &&
           h.bitsOffset < quint32(data.size());
}

int headerSizeForVersion(quint16 version)
{
    // Special thanks to RubyTuesday from BetaArchive for those magic numbers:
    // https://www.betaarchive.com/forum/viewtopic.php?t=33486
    if (version < 0x0200)
        return 117;  // version 1.x
    if (version < 0x0300)
        return 118;  // version 2.x
    return 148;      // version 3.x and later
}


struct VectorSegment
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

QVector<int> readGlyphWidths(const QByteArray& data, const Header& h, int characterCount, bool isVectorFont)
{
    QVector<int> widths(characterCount, 0);
    QVector<bool> widthWasRead(characterCount, false);
    const int version = h.version;
    const int headerSize = headerSizeForVersion(h.version);
    const bool isMonospace = (h.pitchAndFamily & 1) == 0;
    const bool preserveExplicitZeroWidth = version == 0x0100 && !isVectorFont && !isMonospace;

    if (version >= 0x0300)
    {
        for (int i = 0; i < characterCount; ++i)
        {
            const int entryOffset = headerSize + i * 6;
            if (entryOffset + 6 > data.size()) break;
            widths[i] = u16(data, entryOffset);
            widthWasRead[i] = true;
        }
    }
    else if (isMonospace)
    {
        int width = h.pixWidth;
        if (width <= 0) width = 8;
        widths.fill(width);
        widthWasRead.fill(true);
        return widths;
    }
    else if (version == 0x0100)
    {
        if (isVectorFont)
        {
            for (int i = 0; i < characterCount; ++i)
            {
                const int entryOffset = headerSize + i * 4;
                if (entryOffset + 4 > data.size()) break;
                widths[i] = u16(data, entryOffset + 2);
                widthWasRead[i] = true;
            }
        }
        else
        {
            QVector<quint16> offsets(characterCount + 1, 0);
            for (int i = 0; i <= characterCount; ++i)
            {
                const int entryOffset = headerSize + i * 2;
                if (entryOffset + 2 > data.size()) break;
                offsets[i] = u16(data, entryOffset);
            }
            for (int i = 0; i < characterCount; ++i)
            {
                widths[i] = qMax(0, int(offsets[i + 1]) - int(offsets[i]));
                widthWasRead[i] = true;
            }
        }
    }
    else
    {
        for (int i = 0; i < characterCount; ++i)
        {
            const int entryOffset = headerSize + i * 4;
            if (entryOffset + 4 > data.size()) break;
            widths[i] = u16(data, entryOffset);
            widthWasRead[i] = true;
        }
    }

    int fallback = h.pixWidth;
    if (fallback <= 0 && version >= 0x0300) fallback = h.bSpace;
    if (fallback <= 0) fallback = h.avgWidth;
    if (fallback <= 0) fallback = 8;
    for (int i = 0; i < widths.size(); ++i)
        if (!widthWasRead[i] || (!preserveExplicitZeroWidth && widths[i] <= 0))
            widths[i] = fallback;
    return widths;
}

QVector<int> readVectorGlyphOffsets(const QByteArray& data, const Header& h, int characterCount)
{
    QVector<int> offsets(characterCount, 0);
    const int headerSize = headerSizeForVersion(h.version);
    for (int i = 0; i < characterCount; ++i)
    {
        if (h.version >= 0x0300)
        {
            const int entryOffset = headerSize + i * 6;
            if (entryOffset + 6 > data.size()) break;
            const quint32 value = u32(data, entryOffset + 2);
            offsets[i] = value <= quint32(INT_MAX) ? int(value) : 0;
        }
        else
        {
            const int entryOffset = headerSize + i * 4;
            if (entryOffset + 4 > data.size()) break;
            offsets[i] = u16(data, entryOffset);
        }
    }
    return offsets;
}

QVector<VectorSegment> parseVectorSegments(const QByteArray& data, int start, int length,
                                           bool coords2Byte, int yOffset)
{
    QVector<VectorSegment> segments;
    if (length <= 0 || start < 0 || start >= data.size() || start + length > data.size())
        return segments;

    int x = 0;
    int y = 0;
    int position = 0;
    bool penDown = false;
    int lastX = 0;
    int lastY = 0;
    while (position < length)
    {
        if (coords2Byte)
        {
            if (position + 2 > length) break;
            const qint16 marker = qint16(u16(data, start + position));
            if (marker == std::numeric_limits<qint16>::min())
            {
                penDown = false;
                position += 2;
                continue;
            }
        }
        else
        {
            const qint8 marker = qint8(data[start + position]);
            if (marker == std::numeric_limits<qint8>::min())
            {
                penDown = false;
                position += 1;
                continue;
            }
        }

        int dx;
        int dy;
        if (coords2Byte)
        {
            if (position + 4 > length) break;
            dx = qint16(u16(data, start + position));
            dy = qint16(u16(data, start + position + 2));
            position += 4;
        }
        else
        {
            if (position + 2 > length) break;
            dx = qint8(data[start + position]);
            dy = qint8(data[start + position + 1]);
            position += 2;
        }

        x += dx;
        y += dy;
        const int currentY = y + yOffset;
        if (penDown)
        {
            VectorSegment segment;
            segment.x1 = lastX;
            segment.y1 = lastY;
            segment.x2 = x;
            segment.y2 = currentY;
            segments.push_back(segment);
        }
        lastX = x;
        lastY = currentY;
        penDown = true;
    }
    return segments;
}

QImage renderVectorGlyph(const QVector<VectorSegment>& segments, int advance, int glyphHeight)
{
    if (segments.isEmpty())
    {
        QImage empty(qMax(1, advance), qMax(1, glyphHeight), QImage::Format_ARGB32);
        empty.fill(Qt::transparent);
        return empty;
    }

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const VectorSegment& segment : segments)
    {
        minX = qMin(minX, qMin(segment.x1, segment.x2));
        minY = qMin(minY, qMin(segment.y1, segment.y2));
        maxX = qMax(maxX, qMax(segment.x1, segment.x2));
        maxY = qMax(maxY, qMax(segment.y1, segment.y2));
    }
    const int margin = 1;
    const int left = minX - margin;
    const int top = minY - margin;
    QImage image(qMax(1, maxX - minX + margin * 2 + 1),
                 qMax(1, maxY - minY + margin * 2 + 1), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(Qt::black, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    for (const VectorSegment& segment : segments)
        painter.drawLine(segment.x1 - left, segment.y1 - top,
                         segment.x2 - left, segment.y2 - top);
    return image;
}

QImage decodeVector(const QByteArray& data, const Header& h, QString& error)
{
    const int characterCount = int(h.lastChar) - int(h.firstChar) + 1;
    if (characterCount <= 0 || characterCount > 512)
    {
        error = QStringLiteral("Invalid Windows FNT character range.");
        return {};
    }

    const QVector<int> widths = readGlyphWidths(data, h, characterCount, true);
    const QVector<int> offsets = readVectorGlyphOffsets(data, h, characterCount);
    const bool coords2Byte = h.pixHeight > 128 || h.maxWidth > 128;
    const int yOffset = h.pixHeight - h.ascent;
    const int dataLength = h.bitsOffset < quint32(data.size()) ? data.size() - int(h.bitsOffset) : 0;
    const int glyphHeight = qMax(1, int(h.pixHeight) + int(h.ascent));

    QVector<QImage> glyphs;
    glyphs.reserve(characterCount);
    int atlasWidth = 1;
    int atlasHeight = 0;
    for (int i = 0; i < characterCount; ++i)
    {
        int advance = widths[i];
        if (advance <= 0) advance = h.pixWidth > 0 ? h.pixWidth : h.avgWidth;
        if (advance <= 0) advance = 8;
        const int currentOffset = offsets[i];
        const int nextOffset = i + 1 < characterCount ? offsets[i + 1] : dataLength;
        const int strokeDataStart = int(h.bitsOffset) + currentOffset;
        const int strokeLength = nextOffset - currentOffset;
        QImage glyph = renderVectorGlyph(parseVectorSegments(data, strokeDataStart, strokeLength,
                                                              coords2Byte, yOffset), advance, glyphHeight);
        atlasWidth = qMax(atlasWidth, glyph.width());
        atlasHeight += glyph.height();
        glyphs.push_back(glyph);
    }
    if (atlasWidth > 4096 || atlasHeight <= 0 || atlasHeight > 65535)
    {
        error = QStringLiteral("Invalid Windows vector FNT dimensions.");
        return {};
    }
    QImage atlas(atlasWidth, atlasHeight, QImage::Format_ARGB32);
    atlas.fill(Qt::transparent);
    QPainter painter(&atlas);
    int y = 0;
    for (const QImage& glyph : glyphs)
    {
        painter.drawImage(0, y, glyph);
        y += glyph.height();
    }
    return atlas;
}

QImage decodeRaster(const QByteArray& data, const Header& h, QString& error)
{
    const int characterCount = int(h.lastChar) - int(h.firstChar) + 1;
    if (characterCount <= 0 || characterCount > 512)
    {
        error = QStringLiteral("Invalid Windows FNT character range.");
        return {};
    }

    const int headerSize = headerSizeForVersion(h.version);
    QVector<int> widths = readGlyphWidths(data, h, characterCount, false);
    QVector<quint32> offsets(characterCount, 0);
    const bool isMonospace = (h.pitchAndFamily & 1) == 0;

    if (h.version >= 0x0300)
    {
        // Version 3 always uses GLYPHENTRY30: WORD width + DWORD absolute offset.
        // The table is present for fixed-width fonts as well as proportional fonts.
        for (int i = 0; i < characterCount; ++i)
        {
            const int pos = headerSize + i * 6;
            if (pos + 6 > data.size()) break;
            const int width = u16(data, pos);
            widths[i] = width > 0 ? width : widths[i];
            offsets[i] = u32(data, pos + 2);
        }
    }
    else if (isMonospace)
    {
        // Faithful to C#: for Windows FNT 1.x/2.x fixed-width fonts,
        // the glyph width comes from dfPixWidth (fallback 8), not dfAvgWidth
        // and not the per-character table.
        const int width = h.pixWidth > 0 ? h.pixWidth : 8;
        if (h.version > 0x0100)
        {
            const int bytesPerGlyph = ((width + 7) / 8) * h.pixHeight;
            for (int i = 0; i < characterCount; ++i)
            {
                widths[i] = width;
                offsets[i] = h.bitsOffset + quint32(i * bytesPerGlyph);
            }
        }
        else
        {
            quint32 bitOffset = h.bitsOffset * 8u;
            for (int i = 0; i < characterCount; ++i)
            {
                widths[i] = width;
                offsets[i] = bitOffset;
                bitOffset += quint32(width);
            }
        }
    }
    else if (h.version == 0x0100)
    {
        // We expect to have only the offset and no width in raster fonts ver. 1.
        QVector<quint16> bitOffsets(characterCount + 1, 0);
        for (int i = 0; i <= characterCount; ++i)
        {
            const int pos = headerSize + i * 2;
            if (pos + 2 > data.size()) break;
            bitOffsets[i] = u16(data, pos);
        }
        for (int i = 0; i < characterCount; ++i)
        {
            widths[i] = qMax(1, int(bitOffsets[i + 1]) - int(bitOffsets[i]));
            offsets[i] = h.bitsOffset * 8u + bitOffsets[i]; // bit offset
        }
    }
    else
    {
        // Windows 2.x dfCharTable: WORD width + WORD absolute offset.
        for (int i = 0; i < characterCount; ++i)
        {
            const int pos = headerSize + i * 4;
            if (pos + 4 > data.size()) break;
            // Faithful to C# Get(): preserve an explicit zero width in the
            // 2.x raster table. Decode() later applies its advance fallback,
            // while the bitmap row itself remains empty.
            widths[i] = u16(data, pos);
            offsets[i] = u16(data, pos + 2);
        }
    }

    int atlasWidth = 0;
    for (int width : widths)
        atlasWidth = qMax(atlasWidth, qMax(1, width));
    const int atlasHeight = characterCount * int(h.pixHeight);
    if (atlasWidth <= 0 || atlasWidth > 4096 || atlasHeight <= 0 || atlasHeight > 65535)
    {
        error = QStringLiteral("Invalid Windows FNT bitmap dimensions.");
        return {};
    }

    QImage image(atlasWidth, atlasHeight, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    for (int glyph = 0; glyph < characterCount; ++glyph)
    {
        const int rawWidth = widths[glyph];
        const int width = qMin(atlasWidth, qMax(1, rawWidth));
        const int top = glyph * int(h.pixHeight);

        // C# preserves explicit zero-width raster entries as empty bitmap rows.
        if (rawWidth <= 0)
            continue;

        if (h.version == 0x0100)
        {
            // Faithful to C#: every Windows 1.x raster FNT uses dfWidthBytes
            // as the shared scanline stride. Fixed-width fonts take width and
            // sequential bit offsets from the header; proportional fonts take
            // width and bit offsets from dfCharOffset[].
            const quint32 bitBase = offsets[glyph];
            const quint32 scanlineBits = quint32(h.widthBytes) * 8u;
            if (scanlineBits == 0)
                continue;
            for (int y = 0; y < h.pixHeight; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const quint32 bit = bitBase + quint32(y) * scanlineBits + quint32(x);
                    const quint32 byteOffset = bit / 8;
                    if (byteOffset >= quint32(data.size())) continue;
                    if ((quint8(data[int(byteOffset)]) & (0x80u >> (bit & 7))) != 0)
                        image.setPixelColor(x, top + y, Qt::black);
                }
            }
            continue;
        }

        const quint32 start = offsets[glyph];
        const int bytesPerRow = (width + 7) / 8;
        for (int y = 0; y < h.pixHeight; ++y)
        {
            // Windows FNT raster data is stored column-major by byte columns.
            for (int x = 0; x < width; ++x)
            {
                const int byteColumn = x / 8;
                const quint32 pos = start + quint32(byteColumn * h.pixHeight + y);
                if (pos >= quint32(data.size())) continue;
                if ((quint8(data[int(pos)]) & (0x80u >> (x & 7))) != 0)
                    image.setPixelColor(x, top + y, Qt::black);
            }
        }
        Q_UNUSED(bytesPerRow);
    }

    return image;
}


struct PreviewGlyph
{
    int characterCode = 0;
    int advanceX = 0;
    int offsetX = 0;
    int offsetY = 0;
    QImage bitmap;
    QVector<VectorSegment> vectorSegments;
};

struct DecodedWindowsFont
{
    QString faceName;
    QString formatName;
    bool isVector = false;
    int firstCharacter = 0;
    int lastCharacter = 0;
    int defaultCharacter = 0;
    int pixelHeight = 0;
    int lineHeight = 0;
    int characterSet = 0;
    QVector<PreviewGlyph> glyphs;
};

DecodedWindowsFont decodeWindowsFontForPreview(const QByteArray& data, const Header& h, QString& error)
{
    DecodedWindowsFont font;
    font.faceName = readNullTerminatedAnsi(data, int(h.face));
    if (font.faceName.isEmpty()) font.faceName = QStringLiteral("Windows FNT");
    font.formatName = QStringLiteral("Windows FNT %1.%2")
        .arg(h.version / 256).arg(h.version & 0xFF, 2, 10, QLatin1Char('0'));
    font.isVector = (h.type & 0x0001) != 0;
    font.firstCharacter = h.firstChar;
    font.lastCharacter = h.lastChar;
    font.defaultCharacter = int(h.firstChar) + int(h.defaultChar);
    font.pixelHeight = h.pixHeight;
    font.lineHeight = qMax(1, int(h.pixHeight) + int(h.externalLeading));
    font.characterSet = h.charSet;

    const int count = int(h.lastChar) - int(h.firstChar) + 1;
    const QVector<int> widths = readGlyphWidths(data, h, count, font.isVector);
    if (font.isVector)
    {
        const QVector<int> offsets = readVectorGlyphOffsets(data, h, count);
        const bool coords2Byte = h.pixHeight > 128 || h.maxWidth > 128;
        const int yOffset = h.pixHeight - h.ascent;
        const int dataLength = h.bitsOffset < quint32(data.size()) ? data.size() - int(h.bitsOffset) : 0;
        const int glyphHeight = qMax(1, int(h.pixHeight) + int(h.ascent));
        for (int i = 0; i < count; ++i)
        {
            int advance = widths.value(i, h.avgWidth > 0 ? h.avgWidth : 8);
            if (advance <= 0) advance = h.avgWidth > 0 ? h.avgWidth : 8;
            const int currentOffset = offsets.value(i);
            const int nextOffset = i + 1 < count ? offsets.value(i + 1) : dataLength;
            const int start = int(h.bitsOffset) + currentOffset;
            const int length = nextOffset - currentOffset;
            PreviewGlyph glyph;
            glyph.characterCode = int(h.firstChar) + i;
            glyph.advanceX = qMax(1, advance);
            glyph.vectorSegments = parseVectorSegments(data, start, length, coords2Byte, yOffset);
            glyph.bitmap = renderVectorGlyph(glyph.vectorSegments, glyph.advanceX, glyphHeight);
            if (!glyph.vectorSegments.isEmpty())
            {
                int minX = INT_MAX, minY = INT_MAX;
                for (const VectorSegment& segment : glyph.vectorSegments)
                {
                    minX = qMin(minX, qMin(segment.x1, segment.x2));
                    minY = qMin(minY, qMin(segment.y1, segment.y2));
                }
                glyph.offsetX = minX - 1;
                glyph.offsetY = minY - 1;
            }
            font.glyphs.push_back(glyph);
        }
    }
    else
    {
        QImage atlas = decodeRaster(data, h, error);
        if (atlas.isNull()) return font;
        for (int i = 0; i < count; ++i)
        {
            PreviewGlyph glyph;
            glyph.characterCode = int(h.firstChar) + i;
            glyph.advanceX = qMax(1, widths.value(i, h.avgWidth > 0 ? h.avgWidth : 8));
            const int width = qMin(atlas.width(), glyph.advanceX);
            glyph.bitmap = atlas.copy(0, i * int(h.pixHeight), qMax(1, width), qMax(1, int(h.pixHeight)));
            font.glyphs.push_back(glyph);
        }
    }
    return font;
}

const PreviewGlyph* resolveGlyph(const DecodedWindowsFont& font, int code)
{
    auto find = [&font](int value) -> const PreviewGlyph* {
        const int index = value - font.firstCharacter;
        return index >= 0 && index < font.glyphs.size() ? &font.glyphs[index] : nullptr;
    };
    if (const PreviewGlyph* glyph = find(code)) return glyph;
    if (const PreviewGlyph* glyph = find(font.defaultCharacter)) return glyph;
    if (const PreviewGlyph* glyph = find('?')) return glyph;
    return find(' ');
}

QImage scaledGlyph(const PreviewGlyph& glyph, int scale, bool vector)
{
    scale = qMax(1, scale);
    if (!vector || glyph.vectorSegments.isEmpty())
    {
        if (scale <= 1) return glyph.bitmap;
        return glyph.bitmap.scaled(qMax(1, glyph.bitmap.width() * scale),
                                   qMax(1, glyph.bitmap.height() * scale),
                                   Qt::IgnoreAspectRatio,
                                   Qt::FastTransformation);
    }

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const VectorSegment& segment : glyph.vectorSegments)
    {
        minX = qMin(minX, qMin(segment.x1, segment.x2));
        minY = qMin(minY, qMin(segment.y1, segment.y2));
        maxX = qMax(maxX, qMax(segment.x1, segment.x2));
        maxY = qMax(maxY, qMax(segment.y1, segment.y2));
    }
    const int margin = scale;
    const int left = minX * scale - margin;
    const int top = minY * scale - margin;
    QImage image(qMax(1, (maxX - minX) * scale + margin * 2 + 1),
                 qMax(1, (maxY - minY) * scale + margin * 2 + 1),
                 QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(Qt::black, qMax(1.0, double(scale)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    for (const VectorSegment& segment : glyph.vectorSegments)
        painter.drawLine(QPointF(segment.x1 * scale - left, segment.y1 * scale - top),
                         QPointF(segment.x2 * scale - left, segment.y2 * scale - top));
    return image;
}

QImage tintGlyph(const QImage& source, QRgb foregroundRgba)
{
    if (source.isNull()) return {};
    QImage tinted(source.size(), QImage::Format_ARGB32);
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawImage(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), QColor::fromRgba(foregroundRgba));
    return tinted;
}

QImage renderFontText(const DecodedWindowsFont& font, const QString& text, int scale,
                      int padding = 8, QRgb foregroundRgba = qRgba(0, 0, 0, 255),
                      QRgb backgroundRgba = qRgba(255, 255, 255, 255))
{
    if (font.glyphs.isEmpty() || text.isEmpty()) return {};
    scale = qMax(1, scale);
    struct Prepared { QImage image; int x = 0; int y = 0; };
    QVector<Prepared> prepared;
    int penX = 0;
    int minimumX = 0;
    int minimumY = 0;
    int maximumX = 0;
    int maximumY = qMax(1, font.lineHeight * scale);
    for (QChar value : text)
    {
        const PreviewGlyph* glyph = resolveGlyph(font, value.unicode());
        if (!glyph)
        {
            penX += qMax(1, font.pixelHeight / 2) * scale;
            continue;
        }
        Prepared item;
        item.image = tintGlyph(scaledGlyph(*glyph, scale, font.isVector), foregroundRgba);
        item.x = penX + glyph->offsetX * scale;
        item.y = glyph->offsetY * scale;
        minimumX = qMin(minimumX, item.x);
        minimumY = qMin(minimumY, item.y);
        maximumX = qMax(maximumX, item.x + item.image.width());
        maximumY = qMax(maximumY, item.y + item.image.height());
        prepared.push_back(item);
        penX += qMax(1, glyph->advanceX) * scale;
        maximumX = qMax(maximumX, penX);
    }
    const int margin = qMax(0, padding);
    QImage result(qMax(1, maximumX - minimumX + margin * 2),
                  qMax(1, maximumY - minimumY + margin * 2),
                  QImage::Format_ARGB32);
    result.fill(backgroundRgba);
    QPainter painter(&result);
    for (const Prepared& item : prepared)
        painter.drawImage(margin + item.x - minimumX,
                          margin + item.y - minimumY,
                          item.image);
    return result;
}

int robustDimension(QVector<int> values, int fallback)
{
    if (values.isEmpty()) return qMax(1, fallback);
    std::sort(values.begin(), values.end());
    int index = int(qFloor((values.size() - 1) * 0.90));
    return qMax(1, values[qBound(0, index, values.size() - 1)]);
}

QImage renderGlyphMap(const DecodedWindowsFont& font)
{
    if (font.glyphs.isEmpty()) return {};
    const int scale = font.isVector ? 1 : (font.pixelHeight <= 16 ? 2 : 1);
    const int columns = 16;
    QVector<QImage> prepared;
    QVector<int> widths, heights;
    for (const PreviewGlyph& glyph : font.glyphs)
    {
        QImage image = scaledGlyph(glyph, scale, font.isVector);
        prepared.push_back(image);
        if (image.width() > 0) widths.push_back(image.width());
        if (image.height() > 0) heights.push_back(image.height());
    }
    int imageAreaWidth = qMin(96, qMax(robustDimension(widths, qMax(8, font.pixelHeight * scale)),
                                      qMax(8, qMin(font.pixelHeight * 2 * scale, 96))));
    int imageAreaHeight = qMin(96, qMax(robustDimension(heights, qMax(8, font.lineHeight * scale)),
                                       qMax(8, qMin(font.lineHeight * scale, 96))));
    const int cellWidth = imageAreaWidth + 12;
    const int cellHeight = imageAreaHeight + 18;
    const int rows = (font.glyphs.size() + columns - 1) / columns;
    QImage result(qMax(1, columns * cellWidth + 1), qMax(1, rows * cellHeight + 1), QImage::Format_ARGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, font.isVector);
    painter.setPen(QColor(Qt::lightGray));
    QFont codeFont(QStringLiteral("Monospace"), 7);
    codeFont.setStyleHint(QFont::TypeWriter);
    for (int i = 0; i < font.glyphs.size(); ++i)
    {
        const int column = i % columns, row = i / columns;
        const int cellX = column * cellWidth, cellY = row * cellHeight;
        painter.drawRect(cellX, cellY, cellWidth, cellHeight);
        QImage image = prepared[i];
        if (image.width() > imageAreaWidth || image.height() > imageAreaHeight)
            image = image.scaled(imageAreaWidth, imageAreaHeight, Qt::KeepAspectRatio,
                                 font.isVector ? Qt::SmoothTransformation : Qt::FastTransformation);
        const int imageX = cellX + 6 + qMax(0, (imageAreaWidth - image.width()) / 2);
        const int imageY = cellY + 2 + qMax(0, (imageAreaHeight - image.height()) / 2);
        painter.drawImage(imageX, imageY, image);
        painter.setFont(codeFont);
        painter.setPen(QColor(Qt::darkGray));
        const int code = font.glyphs[i].characterCode;
        painter.drawText(cellX + 2, cellY + imageAreaHeight + 3, cellWidth - 4, 14,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(code, 16).toUpper().rightJustified(code <= 0xFF ? 2 : 4, QLatin1Char('0')));
        painter.setPen(QColor(Qt::lightGray));
    }
    return result;
}

QSharedPointer<DecodedWindowsFont> cachedWindowsFont(const QByteArray& data, const Header& header, QString& error)
{
    static QMutex mutex;
    static QHash<QByteArray,QSharedPointer<DecodedWindowsFont>> cache;
    const QByteArray key=QCryptographicHash::hash(data,QCryptographicHash::Sha256);
    { QMutexLocker lock(&mutex); const auto it=cache.constFind(key); if(it!=cache.constEnd()) return it.value(); }
    auto decoded=QSharedPointer<DecodedWindowsFont>::create(decodeWindowsFontForPreview(data,header,error));
    if(decoded->glyphs.isEmpty()) return decoded;
    QMutexLocker lock(&mutex);
    if(cache.size()>=8) cache.erase(cache.begin());
    cache.insert(key,decoded);
    return decoded;
}

} // namespace

bool RT_FONT::LooksLikeWindowsFnt(const QByteArray& resData)
{
    Header header;
    return parseHeader(resData, header);
}

bool RT_FONT::LooksLikeOs2Fnt(const QByteArray& resData)
{
    return OS2_RT_FONT::LooksLike(resData);
}

QImage RT_FONT::renderText(const ResourceEntry& entry, const QString& text, int scale,
                           int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                           QString* error)
{
    if (error) error->clear();
    if (entry.isOs2 || entry.format == ModuleFormat::LX ||
        (entry.format == ModuleFormat::LE && entry.isOs2))
        return OS2_RT_FONT::renderText(entry, text, scale, padding,
                                      foregroundRgba, backgroundRgba, error);

    Header header;
    if (!parseHeader(entry.data, header)) {
        if (error) *error = QStringLiteral("Invalid Windows FNT header.");
        return {};
    }
    QString decodeError;
    const QSharedPointer<DecodedWindowsFont> fontPtr = cachedWindowsFont(entry.data, header, decodeError);
    const DecodedWindowsFont& font = *fontPtr;
    if (font.glyphs.isEmpty()) {
        if (error) *error = decodeError.isEmpty()
            ? QStringLiteral("The Windows FNT contains no decodable glyphs.")
            : decodeError;
        return {};
    }
    const QImage image = renderFontText(font, text, scale, padding,
                                        foregroundRgba, backgroundRgba);
    if (image.isNull() && error)
        *error = QStringLiteral("Font text could not be rendered.");
    return image;
}

ResourcePreview RT_FONT::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;

    // Structure is different for OS/2. Do not parse it as FONTINFO16.
    if (entry.isOs2 || entry.format == ModuleFormat::LX ||
        (entry.format == ModuleFormat::LE && entry.isOs2))
    {
        return OS2_RT_FONT::preview(entry);
    }

    Header header;
    if (!parseHeader(entry.data, header))
    {
        preview.error = QStringLiteral("Invalid Windows FNT header.");
        return preview;
    }

    QString error;
    const QSharedPointer<DecodedWindowsFont> fontPtr = cachedWindowsFont(entry.data, header, error);
    const DecodedWindowsFont& font = *fontPtr;
    if (font.glyphs.isEmpty())
    {
        preview.error = error.isEmpty() ? QStringLiteral("The Windows FNT contains no decodable glyphs.") : error;
        return preview;
    }

    // Keep the same sample text and scale sequence used by ShowFontPreview in C#.
    const QString sampleText = QStringLiteral("The quick brown fox jumps over the lazy dog");
    for (int scale = 1; scale <= 4; ++scale)
    {
        const QImage sample = renderFontText(font, sampleText, scale);
        if (sample.isNull()) continue;
        preview.imageLabels.push_back(QStringLiteral("Sample \u2014 %1 px (%2\u00D7)")
            .arg(font.pixelHeight * scale).arg(scale));
        preview.images.push_back(sample);
    }

    // This is the details line built by ShowFontPreview in C#, followed by
    // the decoder metadata that is useful when comparing malformed fonts.
    preview.text = QStringLiteral("%1 \u2014 %2 \u2014 %3 \u2014 %4 glyphs \u2014 codes %5\u2013%6 \u2014 native height %7 px")
        .arg(font.faceName, font.formatName, font.isVector ? QStringLiteral("vector") : QStringLiteral("raster"))
        .arg(font.glyphs.size()).arg(font.firstCharacter).arg(font.lastCharacter).arg(font.pixelHeight);
    // Converted export operates on the original glyph images, matching
    // ResourceConversion.GetBitmaps(DecodedFont) in the C# implementation.
    for (const PreviewGlyph& glyph : font.glyphs)
    {
        if (glyph.bitmap.isNull()) continue;
        preview.conversionImages.push_back(glyph.bitmap);
        preview.conversionImageCodes.push_back(glyph.characterCode);
    }

    if (font.characterSet > 0)
        preview.text += QStringLiteral(" \u2014 charset %1").arg(font.characterSet);
    preview.text += QStringLiteral("\r\nDefault character: %1\r\nBreak character: %2\r\nAscent: %3")
        .arg(font.defaultCharacter)
        .arg(int(header.firstChar) + int(header.breakChar))
        .arg(header.ascent);

    return preview;
}

} // namespace resources
} // namespace peare
