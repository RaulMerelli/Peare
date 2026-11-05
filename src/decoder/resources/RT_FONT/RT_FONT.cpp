#include "RT_FONT.h"
#include "../../font/AfmFont.h"
#include "../../font/PfmFont.h"
#include "../../font/AmigaFont.h"
#include "../../font/MacFondFont.h"
#include <QSharedPointer>
#include <QMutexLocker>
#include <QMutex>
#include <QHash>
#include <QSet>
#include <QCryptographicHash>
#include "OS2_RT_FONT.h"
#include "../../font/BitmapFont.h"
#include "../../font/SfntFont.h"
#include "../../font/SvgFont.h"
#include "../../font/SfnFont.h"
#include "../../font/SnfFont.h"
#include "../../font/ShpFont.h"
#include "../../font/Type1Font.h"
#include "../../font/Type3Font.h"
#include "../../font/DwfFont.h"
#include "../../font/WinGdiSfntRenderer.h"

#include <QImage>
#include <QPainter>
#include <QFont>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QRawFont>
#include <QGlyphRun>
#include <QPen>
#include <QPainterPath>
#include <QStringList>
#include <QTextCodec>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <limits>
#include <memory>
#include <mutex>

namespace peare {
namespace resources {
namespace {

QGuiApplication* ensureFontGuiApplication()
{
    if (QCoreApplication::instance())
        return qobject_cast<QGuiApplication*>(QCoreApplication::instance());

    static std::mutex mutex;
    static std::unique_ptr<QGuiApplication> application;
    std::lock_guard<std::mutex> lock(mutex);
    if (!application && !QCoreApplication::instance()) {
        static int argc = 1;
        static char applicationName[] = "PeareDecoder";
        static char* argv[] = {applicationName, nullptr};
        application.reset(new QGuiApplication(argc, argv));
    }
    return application.get();
}

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
            // Windows 1.x raster FNT uses dfWidthBytes as the scanline stride.
            // Proportional glyph widths and bit offsets come from dfCharOffset[].
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

QImage renderBitmapFont(const font::BitmapFont& fontData, const QString& text, int scale,
                        int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || text.size() > 4096) {
        if (error) *error = QStringLiteral("Bitmap font render limits exceeded.");
        return {};
    }
    QVector<quint32> characters;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            characters.push_back(QChar::surrogateToUcs4(ch, text.at(++i)));
        else characters.push_back(ch.unicode());
    }
    QHash<quint32, const font::Glyph*> glyphs;
    int left = 0;
    int right = 0;
    for (const font::Glyph& glyph : fontData.glyphs) {
        if (glyph.codepoint != 0xffffffffu && !glyphs.contains(glyph.codepoint))
            glyphs.insert(glyph.codepoint, &glyph);
        left = qMax(left, -glyph.bearingX);
        right = qMax(right, glyph.bearingX + glyph.width - glyph.advance);
    }
    int widest = 0;
    int width = 0;
    int lineCount = 1;
    quint32 previous = 0xffffffffu;
    for (quint32 codepoint : characters) {
        if (codepoint == '\n') {
            widest = qMax(widest, width);
            width = 0;
            ++lineCount;
            previous = 0xffffffffu;
            continue;
        }
        const font::Glyph* glyph = glyphs.value(codepoint, glyphs.value('?'));
        for (const font::KerningPair& pair : fontData.kernings)
            if (pair.left == previous && pair.right == codepoint) width += pair.adjustment;
        width += glyph ? glyph->advance : fontData.width;
        previous = codepoint;
    }
    widest = qMax(widest, width);
    const int ascent = fontData.ascent > 0 ? fontData.ascent : fontData.height;
    const int lineHeight = qMax(1, qMax(fontData.height, fontData.ascent + fontData.descent));
    const qint64 imageWidth = qint64(widest + left + right + 2LL * padding) * scale;
    const qint64 imageHeight = qint64(lineCount * lineHeight + 2LL * padding) * scale;
    if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > 16384 || imageHeight > 16384 ||
        imageWidth * imageHeight > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("Bitmap font render dimensions exceed limits.");
        return {};
    }
    QImage image(int(imageWidth), int(imageHeight), QImage::Format_ARGB32);
    image.fill(background);
    int penX = padding + left;
    int baseline = padding + ascent;
    quint32 previousCodepoint = 0xffffffffu;
    for (quint32 codepoint : characters) {
        if (codepoint == '\n') {
            penX = padding + left;
            baseline += lineHeight;
            previousCodepoint = 0xffffffffu;
            continue;
        }
        const font::Glyph* glyph = glyphs.value(codepoint, glyphs.value('?'));
        if (!glyph) { penX += fontData.width; previousCodepoint = codepoint; continue; }
        for (const font::KerningPair& pair : fontData.kernings)
            if (pair.left == previousCodepoint && pair.right == codepoint)
                penX += pair.adjustment;
        const int rowBytes = (glyph->width + 7) / 8;
        const int originX = (penX + glyph->bearingX) * scale;
        const int originY = (baseline - glyph->bearingY) * scale;
        for (int y = 0; y < glyph->height; ++y) {
            for (int x = 0; x < glyph->width; ++x) {
                if (!glyph->mask.isEmpty() &&
                    !(quint8(glyph->mask.at(y * ((glyph->width + 7) / 8) + x / 8)) &
                      (0x80u >> (x & 7)))) continue;
                QRgb pixel = foreground;
                if (!glyph->pixels.isEmpty()) {
                    const quint8 value = quint8(glyph->pixels.at(y * glyph->width + x));
                    const bool amigaColor = fontData.format == font::Format::AmigaColor ||
                                            fontData.format == font::Format::AmigaAnim;
                    const QVector<quint32>& palette = glyph->palette.isEmpty()
                        ? fontData.palette : glyph->palette;
                    if (!value && !amigaColor) continue;
                    if (fontData.format == font::Format::AmigaColor && value == 0) continue;
                    int paletteIndex = amigaColor ? value : value - 1;
                    int alpha = 255;
                    if (amigaColor) {
                        alpha = 255;
                    } else if (fontData.alphaBits == 8) {
                        paletteIndex = 0;
                        alpha = value;
                    } else if (fontData.alphaBits > 0) {
                        const int indexBits = 8 - fontData.alphaBits;
                        const int alphaMaximum = (1 << fontData.alphaBits) - 1;
                        paletteIndex = value & ((1 << indexBits) - 1);
                        alpha = (value >> indexBits) * 255 / alphaMaximum;
                    }
                    if (paletteIndex < 0 || paletteIndex >= palette.size()) continue;
                    const quint32 color = palette.at(paletteIndex);
                    if (amigaColor && paletteIndex == fontData.foregroundPaletteIndex) {
                        pixel = foreground;
                    } else if (fontData.format == font::Format::Bmf &&
                               fontData.alphaBits == 8) {
                        pixel = qRgba(qRed(foreground), qGreen(foreground),
                                      qBlue(foreground), alpha);
                    } else {
                        pixel = qRgba((color >> 16) & 0xff, (color >> 8) & 0xff,
                                      color & 0xff, alpha);
                    }
                } else {
                    const int offset = y * rowBytes + x / 8;
                    if (offset >= glyph->bitmap.size() ||
                        !(quint8(glyph->bitmap.at(offset)) & (0x80u >> (x & 7)))) continue;
                }
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx) {
                        const int px = originX + x * scale + sx;
                        const int py = originY + y * scale + sy;
                        if (px >= 0 && px < image.width() && py >= 0 && py < image.height())
                            image.setPixel(px, py, pixel);
                    }
            }
        }
        penX += glyph->advance;
        previousCodepoint = codepoint;
    }
    return image;
}

QImage renderBitmapGlyph(const font::BitmapFont& fontData, quint32 glyphIndex, int scale,
                         int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 ||
        glyphIndex >= quint32(fontData.glyphs.size())) {
        if (error) *error = QStringLiteral("Bitmap glyph render limits exceeded.");
        return {};
    }
    const font::Glyph& glyph = fontData.glyphs.at(int(glyphIndex));
    const int left = qMax(0, -glyph.bearingX);
    const int right = qMax(0, glyph.bearingX + glyph.width - glyph.advance);
    const int above = qMax(0, glyph.bearingY - glyph.height);
    const int below = qMax(0, fontData.height - glyph.bearingY);
    const qint64 imageWidth = qMax<qint64>(1, qint64(left + glyph.width + right + 2 * padding) * scale);
    const qint64 imageHeight = qMax<qint64>(1, qint64(above + glyph.height + below + 2 * padding) * scale);
    if (imageWidth > 16384 || imageHeight > 16384 ||
        imageWidth * imageHeight > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("Bitmap glyph image exceeds render limits.");
        return {};
    }
    QImage image(int(imageWidth), int(imageHeight), QImage::Format_ARGB32);
    image.fill(background);
    const int originX = (padding + left) * scale;
    const int originY = (padding + above) * scale;
    const int rowBytes = (glyph.width + 7) / 8;
    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            if (!glyph.mask.isEmpty() &&
                !(quint8(glyph.mask.at(y * ((glyph.width + 7) / 8) + x / 8)) &
                  (0x80u >> (x & 7)))) continue;
            QRgb pixel = foreground;
            if (!glyph.pixels.isEmpty()) {
                const quint8 value = quint8(glyph.pixels.at(y * glyph.width + x));
                const bool amigaColor = fontData.format == font::Format::AmigaColor ||
                                        fontData.format == font::Format::AmigaAnim;
                const QVector<quint32>& palette = glyph.palette.isEmpty()
                    ? fontData.palette : glyph.palette;
                if (!value && !amigaColor) continue;
                if (fontData.format == font::Format::AmigaColor && value == 0) continue;
                int paletteIndex = amigaColor ? value : value - 1;
                int alpha = 255;
                if (amigaColor) {
                    alpha = 255;
                } else if (fontData.alphaBits == 8) {
                    paletteIndex = 0;
                    alpha = value;
                } else if (fontData.alphaBits > 0) {
                    const int indexBits = 8 - fontData.alphaBits;
                    const int alphaMaximum = (1 << fontData.alphaBits) - 1;
                    paletteIndex = value & ((1 << indexBits) - 1);
                    alpha = (value >> indexBits) * 255 / alphaMaximum;
                }
                if (paletteIndex < 0 || paletteIndex >= palette.size()) continue;
                const quint32 color = palette.at(paletteIndex);
                if (amigaColor && paletteIndex == fontData.foregroundPaletteIndex) {
                    pixel = foreground;
                } else if (fontData.format == font::Format::Bmf &&
                           fontData.alphaBits == 8) {
                    pixel = qRgba(qRed(foreground), qGreen(foreground),
                                  qBlue(foreground), alpha);
                } else {
                    pixel = qRgba((color >> 16) & 0xff, (color >> 8) & 0xff,
                                  color & 0xff, alpha);
                }
            } else {
                const int offset = y * rowBytes + x / 8;
                if (offset >= glyph.bitmap.size() ||
                    !(quint8(glyph.bitmap.at(offset)) & (0x80u >> (x & 7)))) continue;
            }
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                    image.setPixel(originX + x * scale + sx, originY + y * scale + sy,
                                   pixel);
        }
    }
    return image;
}

QImage renderOutlineFont(const font::SfntFont& fontData, const QString& text, int scale,
                         int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("Outline font render limits exceeded.");
        return {};
    }
    if (fontData.hasCff2) {
        if (error) *error = QStringLiteral("CFF2 rendering is not supported by the Qt outline backend.");
        return {};
    }
#ifdef Q_OS_WIN
    if (fontData.format == font::SfntFormat::TrueType && fontData.hasTrueTypeOutlines)
        return font::renderSfntTextWithGdi(fontData, text, scale, padding,
                                            foreground, background, error);
#endif
    if (!ensureFontGuiApplication()) {
        if (error) *error = QStringLiteral("Outline font rendering requires a Qt GUI application.");
        return {};
    }
    QRawFont raw(fontData.sfntBytes, 16 * scale, QFont::PreferDefaultHinting);
    if (!raw.isValid()) {
        if (error) *error = QStringLiteral("Qt could not load the outline font.");
        return {};
    }
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QVector<QVector<quint32>> glyphLines;
    QVector<QVector<QPointF>> advanceLines;
    qreal widest = 0;
    for (const QString& line : lines) {
        const QVector<quint32> glyphs = raw.glyphIndexesForString(line);
        const QVector<QPointF> advances = raw.advancesForGlyphIndexes(glyphs);
        qreal width = 0;
        for (const QPointF& advance : advances) width += advance.x();
        widest = qMax(widest, width);
        glyphLines.push_back(glyphs);
        advanceLines.push_back(advances);
    }
    const qreal lineHeight = qMax<qreal>(1, raw.ascent() + raw.descent() + raw.leading());
    const qreal imageWidth = qCeil(widest) + 2.0 * padding;
    const qreal imageHeight = qCeil(lineHeight * lines.size()) + 2.0 * padding;
    if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > 16384 || imageHeight > 16384 ||
        imageWidth * imageHeight > 64.0 * 1024 * 1024) {
        if (error) *error = QStringLiteral("Outline font render dimensions exceed limits.");
        return {};
    }
    QImage image(int(imageWidth), int(imageHeight), QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor::fromRgba(foreground));
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        QGlyphRun run;
        run.setRawFont(raw);
        run.setGlyphIndexes(glyphLines.at(lineIndex));
        QVector<QPointF> positions;
        qreal x = 0;
        for (const QPointF& advance : advanceLines.at(lineIndex)) {
            positions.push_back(QPointF(x, 0));
            x += advance.x();
        }
        run.setPositions(positions);
        const qreal baseline = padding + raw.ascent() + lineIndex * lineHeight;
        painter.drawGlyphRun(QPointF(padding, baseline), run);
    }
    return image;
}

QImage renderOutlineGlyph(const font::SfntFont& fontData, const QRawFont& raw,
                          quint32 glyphIndex, int scale, int padding, QRgb foreground,
                          QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 ||
        glyphIndex >= fontData.glyphCount) {
        if (error) *error = QStringLiteral("Outline glyph render limits exceeded.");
        return {};
    }
    if (!raw.isValid()) {
        if (error) *error = QStringLiteral("Qt could not load the requested outline glyph.");
        return {};
    }
    const QRectF bounds = raw.boundingRect(glyphIndex);
    if (!qIsFinite(bounds.x()) || !qIsFinite(bounds.y()) || !qIsFinite(bounds.width()) ||
        !qIsFinite(bounds.height()) || qAbs(bounds.x()) > 16384 || qAbs(bounds.y()) > 16384 ||
        bounds.width() > 16384 || bounds.height() > 16384) {
        if (error) *error = QStringLiteral("Outline glyph bounds exceed render limits.");
        return {};
    }
    const int width = qMax(1, qCeil(bounds.width()) + 2 * padding);
    const int height = qMax(1, qCeil(bounds.height()) + 2 * padding);
    if (quint64(width) * quint64(height) > 64u * 1024u * 1024u) {
        if (error) *error = QStringLiteral("Outline glyph image exceeds render limits.");
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    QVector<quint32> indexes{glyphIndex};
    QVector<QPointF> positions{QPointF(0, 0)};
    QGlyphRun run;
    run.setRawFont(raw);
    run.setGlyphIndexes(indexes);
    run.setPositions(positions);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor::fromRgba(foreground));
    painter.drawGlyphRun(QPointF(padding - bounds.left(), padding - bounds.top()), run);
    return image;
}

QImage renderOutlineGlyph(const font::SfntFont& fontData, quint32 glyphIndex, int scale,
                          int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16) {
        if (error) *error = QStringLiteral("Outline glyph render limits exceeded.");
        return {};
    }
    if (fontData.hasCff2) {
        if (error) *error = QStringLiteral("CFF2 rendering is not supported by the Qt outline backend.");
        return {};
    }
#ifdef Q_OS_WIN
    if (fontData.format == font::SfntFormat::TrueType && fontData.hasTrueTypeOutlines)
        return font::renderSfntGlyphWithGdi(fontData, glyphIndex, scale, padding,
                                             foreground, background, error);
#endif
    if (!ensureFontGuiApplication()) {
        if (error) *error = QStringLiteral("Outline font rendering requires a Qt GUI application.");
        return {};
    }
    const QRawFont raw(fontData.sfntBytes, 16 * scale, QFont::PreferDefaultHinting);
    return renderOutlineGlyph(fontData, raw, glyphIndex, scale, padding, foreground, background,
                              error);
}

bool validShpPosition(const QPointF& point)
{
    return qIsFinite(point.x()) && qIsFinite(point.y()) &&
        qAbs(point.x()) <= 1000000 && qAbs(point.y()) <= 1000000;
}

bool appendShpArc(const QPointF& start, const QPointF& center, double sweep,
                  QPointF* position, bool penDown, QPainterPath* path)
{
    const double pi = 3.14159265358979323846;
    if (!position || !path || !validShpPosition(start) || !validShpPosition(center) ||
        !qIsFinite(sweep) || qAbs(sweep) > 2.0 * pi + 1.0e-6)
        return false;
    const double radius = qSqrt(qPow(start.x() - center.x(), 2) +
                                qPow(start.y() - center.y(), 2));
    if (!qIsFinite(radius) || radius <= 0 || radius > 1000000) return false;
    const double startAngle = qAtan2(start.y() - center.y(), start.x() - center.x());
    const int steps = qMax(1, qMin(128, qCeil(qAbs(sweep) * radius / 1.5)));
    for (int step = 1; step <= steps; ++step) {
        const double angle = startAngle + sweep * double(step) / steps;
        const QPointF point(center.x() + radius * qCos(angle),
                            center.y() + radius * qSin(angle));
        if (!validShpPosition(point)) return false;
        if (penDown) path->lineTo(point); else path->moveTo(point);
    }
    *position = center + QPointF(radius * qCos(startAngle + sweep),
                                 radius * qSin(startAngle + sweep));
    return validShpPosition(*position);
}

bool appendShpBulgeArc(QPointF* position, qint8 dx, qint8 dy, qint8 bulge,
                       double scale, bool penDown, QPainterPath* path)
{
    if (!position || !path || !validShpPosition(*position) ||
        !qIsFinite(scale) || scale <= 0 || qAbs(int(bulge)) > 127)
        return false;
    const QPointF start = *position;
    const QPointF end = start + QPointF(dx * scale, dy * scale);
    if (!validShpPosition(end)) return false;
    if (bulge == 0) {
        if (penDown) path->lineTo(end); else path->moveTo(end);
        *position = end;
        return true;
    }
    const double chord = qSqrt(qPow(end.x() - start.x(), 2) +
                               qPow(end.y() - start.y(), 2));
    if (chord <= 0) return false;
    const double ratio = double(bulge) / 127.0;
    const double sweep = 4.0 * qAtan(ratio);
    const QPointF midpoint = (start + end) * 0.5;
    const QPointF normal(-(end.y() - start.y()) / chord,
                          (end.x() - start.x()) / chord);
    const QPointF center = midpoint + normal * (chord * (1.0 - ratio * ratio) /
                                                  (4.0 * ratio));
    if (!appendShpArc(start, center, sweep, position, penDown, path)) return false;
    *position = end;
    return true;
}

bool skipShpInstruction(const QVector<quint16>& instructions, int* index,
                        bool bigFont)
{
    if (!index || *index >= instructions.size()) return false;
    const quint16 code = instructions.at((*index)++);
    if (code >= 0x10) return true;
    int operands = 0;
    switch (code) {
    case 1: case 2: case 5: case 6: case 14: return true;
    case 3: case 4: operands = 1; break;
    case 7:
        operands = bigFont && *index + 5 < instructions.size() &&
                           instructions.at(*index) == 0 ? 6 : 1;
        break;
    case 8: case 10: operands = 2; break;
    case 11: operands = 5; break;
    case 12: operands = 3; break;
    case 9:
        while (*index + 1 < instructions.size()) {
            const quint16 dx = instructions.at((*index)++);
            const quint16 dy = instructions.at((*index)++);
            if (dx == 0 && dy == 0) return true;
        }
        return false;
    case 13:
        while (*index + 1 < instructions.size()) {
            const quint16 dx = instructions.at((*index)++);
            const quint16 dy = instructions.at((*index)++);
            if (dx == 0 && dy == 0) return true;
            if (*index >= instructions.size()) return false;
            ++*index;
        }
        return false;
    default: return false;
    }
    if (*index + operands > instructions.size()) return false;
    *index += operands;
    return true;
}

bool drawShpShape(const font::ShpGlyph& glyph,
                  const QHash<quint16, const font::ShpGlyph*>& glyphs,
                  bool bigFont,
                  QPointF* position, bool* penDown, double* scale,
                  QVector<QPointF>* stack, QSet<quint16>* active,
                  QPainterPath* path, int depth)
{
    if (!position || !penDown || !scale || !stack || !active || !path || depth > 16 ||
        active->contains(glyph.code))
        return false;
    active->insert(glyph.code);
    const double diagonal = 0.4142135623730951;
    const double xUnits[16] = {1, 1, 1, diagonal, 0, -diagonal, -1, -1,
                               -1, -1, -1, -diagonal, 0, diagonal, 1, 1};
    const double yUnits[16] = {0, diagonal, 1, 1, 1, 1, 1, diagonal,
                               0, -diagonal, -1, -1, -1, -1, -1, -diagonal};
    for (int i = 0; i < glyph.instructions.size();) {
        if (path->elementCount() > 65536) return false;
        const quint8 code = quint8(glyph.instructions.at(i++));
        if (!code)
            break;
        if (code >= 0x10) {
            const int direction = code & 0x0f;
            const double length = (code >> 4) * *scale;
            *position += QPointF(xUnits[direction] * length, yUnits[direction] * length);
            if (!validShpPosition(*position)) return false;
            if (*penDown) path->lineTo(*position); else path->moveTo(*position);
            continue;
        }
        switch (code) {
        case 1: *penDown = true; path->moveTo(*position); break;
        case 2: *penDown = false; path->moveTo(*position); break;
        case 3:
            *scale /= glyph.instructions.at(i++);
            if (!qIsFinite(*scale) || *scale < 1.0e-6 || *scale > 1.0e6) return false;
            break;
        case 4:
            *scale *= glyph.instructions.at(i++);
            if (!qIsFinite(*scale) || *scale < 1.0e-6 || *scale > 1.0e6) return false;
            break;
        case 5:
            if (stack->size() >= 4) return false;
            stack->append(*position);
            break;
        case 6:
            if (!stack->isEmpty()) {
                *position = stack->takeLast();
                if (*penDown) path->lineTo(*position); else path->moveTo(*position);
            }
            break;
        case 7: {
            if (bigFont && i + 5 < glyph.instructions.size() &&
                glyph.instructions.at(i) == 0) {
                ++i;
                const quint16 childCode = glyph.instructions.at(i++);
                const qint8 baseX = qint8(glyph.instructions.at(i++));
                const qint8 baseY = qint8(glyph.instructions.at(i++));
                const double width = glyph.instructions.at(i++);
                const double height = glyph.instructions.at(i++);
                const font::ShpGlyph* child = glyphs.value(childCode, nullptr);
                QPointF childPosition;
                bool childPenDown = true;
                double childScale = 1.0;
                QVector<QPointF> childStack;
                QPainterPath childPath;
                if (child && !drawShpShape(*child, glyphs, bigFont, &childPosition,
                                           &childPenDown, &childScale, &childStack,
                                           active, &childPath, depth + 1)) {
                    active->remove(glyph.code);
                    return false;
                }
                const QPointF origin = *position +
                    QPointF(baseX * *scale, baseY * *scale);
                const double xScale = *scale * width / 16.0;
                const double yScale = *scale * height / 16.0;
                if (!qIsFinite(xScale) || !qIsFinite(yScale) ||
                    xScale <= 0 || yScale <= 0 || xScale > 1.0e6 || yScale > 1.0e6) {
                    active->remove(glyph.code);
                    return false;
                }
                if (child) {
                    const QTransform transform(xScale, 0, 0, yScale, origin.x(), origin.y());
                    path->addPath(transform.map(childPath));
                    *position = transform.map(childPosition);
                }
            } else {
                const font::ShpGlyph* child = glyphs.value(glyph.instructions.at(i++), nullptr);
                if ((!child && !bigFont) ||
                    (child && !drawShpShape(*child, glyphs, bigFont, position, penDown,
                                            scale, stack, active, path, depth + 1))) {
                    active->remove(glyph.code);
                    return false;
                }
            }
            if (!validShpPosition(*position)) {
                active->remove(glyph.code);
                return false;
            }
            break;
        }
        case 8: {
            const qint8 dx = qint8(glyph.instructions.at(i++));
            const qint8 dy = qint8(glyph.instructions.at(i++));
            *position += QPointF(dx * *scale, dy * *scale);
            if (!validShpPosition(*position)) return false;
            if (*penDown) path->lineTo(*position); else path->moveTo(*position);
            break;
        }
        case 9:
            while (i + 1 < glyph.instructions.size()) {
                const qint8 dx = qint8(glyph.instructions.at(i++));
                const qint8 dy = qint8(glyph.instructions.at(i++));
                if (dx == 0 && dy == 0) break;
                *position += QPointF(dx * *scale, dy * *scale);
                if (!validShpPosition(*position)) return false;
                if (*penDown) path->lineTo(*position); else path->moveTo(*position);
            }
            break;
        case 10:
        case 11: {
            double startOffset = 0, endOffset = 0;
            quint32 radius = 0;
            int packed = 0;
            if (code == 10) {
                radius = glyph.instructions.at(i++);
                packed = qAbs(int(qint8(glyph.instructions.at(i++))));
            } else {
                startOffset = glyph.instructions.at(i++);
                endOffset = glyph.instructions.at(i++);
                radius = quint32(glyph.instructions.at(i++)) * 256u;
                radius += glyph.instructions.at(i++);
                packed = qAbs(int(qint8(glyph.instructions.at(i++))));
            }
            const int direction = qint8(glyph.instructions.at(i - 1)) < 0 ? -1 : 1;
            const int startOctant = (packed >> 4) & 0x07;
            int octants = packed & 0x0f;
            if (octants == 0) octants = 8;
            const double pi = 3.14159265358979323846;
            const double degreesPerUnit = pi / (4.0 * 256.0);
            const double startAngle = startOctant * pi / 4.0 +
                direction * startOffset * degreesPerUnit;
            const double sweep = direction *
                (octants * pi / 4.0 + endOffset * degreesPerUnit);
            const QPointF start = *position;
            const double scaledRadius = radius * *scale;
            const QPointF center(start.x() - scaledRadius * qCos(startAngle),
                                 start.y() - scaledRadius * qSin(startAngle));
            if (!appendShpArc(start, center, sweep, position, *penDown, path)) return false;
            break;
        }
        case 12: {
            const qint8 dx = qint8(glyph.instructions.at(i++));
            const qint8 dy = qint8(glyph.instructions.at(i++));
            const qint8 bulge = qint8(glyph.instructions.at(i++));
            if (!appendShpBulgeArc(position, dx, dy, bulge, *scale, *penDown, path))
                return false;
            break;
        }
        case 13:
            while (i + 1 < glyph.instructions.size()) {
                const qint8 dx = qint8(glyph.instructions.at(i++));
                const qint8 dy = qint8(glyph.instructions.at(i++));
                if (dx == 0 && dy == 0) break;
                const qint8 bulge = qint8(glyph.instructions.at(i++));
                if (!appendShpBulgeArc(position, dx, dy, bulge, *scale,
                                       *penDown, path)) return false;
            }
            break;
        case 14:
            if (!skipShpInstruction(glyph.instructions, &i, bigFont)) {
                active->remove(glyph.code);
                return false;
            }
            break;
        default:
            active->remove(glyph.code);
            return false;
        }
    }
    active->remove(glyph.code);
    return true;
}

QImage renderShpFont(const font::ShpFont& fontData, const QString& text, int scale,
                     int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("SHP font render limits exceeded.");
        return {};
    }
    QHash<quint16, const font::ShpGlyph*> glyphs;
    for (const font::ShpGlyph& glyph : fontData.glyphs)
        glyphs.insert(glyph.code, &glyph);
    QVector<QPainterPath> positioned;
    QRectF bounds;
    const int lineHeight = fontData.above + fontData.below;
    double penX = 0;
    double widest = 0;
    int line = 0;
    for (QChar character : text) {
        if (character == QLatin1Char('\n')) {
            widest = qMax(widest, penX);
            penX = 0;
            ++line;
            continue;
        }
        const quint16 shapeCode = fontData.glyphByUnicode.value(character.unicode(),
                                                                 character.unicode());
        const font::ShpGlyph* glyph = glyphs.value(shapeCode, nullptr);
        if (!glyph) {
            penX += qMax(1, int(fontData.above) / 2);
            continue;
        }
        QPointF position;
        bool penDown = true;
        double vectorScale = 1;
        QVector<QPointF> stack;
        QSet<quint16> active;
        QPainterPath path;
        if (!drawShpShape(*glyph, glyphs, fontData.bigFont, &position, &penDown,
                          &vectorScale, &stack, &active, &path, 0)) {
            penX += qMax(1.0, double(fontData.above) * 0.35);
            continue;
        }
        const double advance = position.x() > 0 ? position.x()
            : qMax(double(fontData.above) * 0.35, path.boundingRect().right() + 1);
        if (!path.isEmpty()) {
            const QTransform transform(1, 0, 0, -1, penX, line * lineHeight + fontData.above);
            path = transform.map(path);
            bounds = bounds.isNull() ? path.boundingRect() : bounds.united(path.boundingRect());
            positioned.append(path);
        }
        penX += advance;
    }
    widest = qMax(widest, penX);
    const QRectF layout(0, 0, qMax(widest, double(fontData.above)),
                        qMax(1, line + 1) * lineHeight);
    bounds = bounds.isNull() ? layout : bounds.united(layout);
    const double factor = 16.0 * scale / qMax(1, lineHeight);
    const int width = qCeil(bounds.width() * factor) + padding * 2;
    const int height = qCeil(bounds.height() * factor) + padding * 2;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        qint64(width) * height > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("SHP font render dimensions exceed limits.");
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor::fromRgba(foreground), 1.0 / factor,
                        Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    painter.setBrush(Qt::NoBrush);
    painter.setWorldTransform(QTransform(factor, 0, 0, factor,
        padding - bounds.left() * factor, padding - bounds.top() * factor));
    for (const QPainterPath& path : positioned)
        painter.drawPath(path);
    return image;
}

QImage renderShpGlyph(const font::ShpFont& fontData,
                      const QHash<quint16, const font::ShpGlyph*>& glyphs,
                      quint32 glyphIndex, int scale, int padding, QRgb foreground,
                      QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 ||
        glyphIndex >= quint32(fontData.glyphs.size())) {
        if (error) *error = QStringLiteral("SHP glyph render limits exceeded.");
        return {};
    }
    QPointF position;
    bool penDown = true;
    double vectorScale = 1;
    QVector<QPointF> stack;
    QSet<quint16> active;
    QPainterPath path;
    const font::ShpGlyph& glyph = fontData.glyphs.at(int(glyphIndex));
    if (!drawShpShape(glyph, glyphs, fontData.bigFont, &position, &penDown,
                      &vectorScale, &stack, &active, &path, 0))
        path = QPainterPath();
    const double advance = position.x() > 0 ? position.x()
        : qMax(double(fontData.above) * 0.35, path.boundingRect().right() + 1);
    QVector<QPainterPath> positioned;
    QRectF bounds;
    if (!path.isEmpty()) {
        path = QTransform(1, 0, 0, -1, 0, fontData.above).map(path);
        bounds = path.boundingRect();
        positioned.append(path);
    }
    const int lineHeight = fontData.above + fontData.below;
    const QRectF layout(0, 0, qMax(advance, double(fontData.above)), lineHeight);
    bounds = bounds.isNull() ? layout : bounds.united(layout);
    const double factor = 16.0 * scale / qMax(1, lineHeight);
    const int width = qCeil(bounds.width() * factor) + padding * 2;
    const int height = qCeil(bounds.height() * factor) + padding * 2;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        qint64(width) * height > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("SHP glyph image exceeds render limits.");
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor::fromRgba(foreground), 1.0 / factor,
                        Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    painter.setBrush(Qt::NoBrush);
    painter.setWorldTransform(QTransform(factor, 0, 0, factor,
        padding - bounds.left() * factor, padding - bounds.top() * factor));
    for (const QPainterPath& glyphPath : positioned)
        painter.drawPath(glyphPath);
    return image;
}

QPainterPath svgPainterPath(const QVector<font::SvgPathCommand>& commands)
{
    QPainterPath path;
    for (const font::SvgPathCommand& command : commands) {
        switch (command.op) {
        case font::SvgPathOp::MoveTo: path.moveTo(command.end); break;
        case font::SvgPathOp::LineTo: path.lineTo(command.end); break;
        case font::SvgPathOp::CubicTo:
            path.cubicTo(command.control1, command.control2, command.end);
            break;
        case font::SvgPathOp::QuadTo: path.quadTo(command.control1, command.end); break;
        case font::SvgPathOp::Close: path.closeSubpath(); break;
        }
    }
    return path;
}

QPainterPath type1PainterPath(const font::Type1Glyph& glyph)
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const font::Type1PathCommand& command : glyph.path) {
        switch (command.kind) {
        case font::Type1PathCommand::Move: path.moveTo(command.x1, command.y1); break;
        case font::Type1PathCommand::Line: path.lineTo(command.x1, command.y1); break;
        case font::Type1PathCommand::Curve:
            path.cubicTo(command.x1, command.y1, command.x2, command.y2,
                         command.x3, command.y3);
            break;
        case font::Type1PathCommand::Close: path.closeSubpath(); break;
        }
    }
    return path;
}

QImage renderType1Font(const font::Type1Font& fontData, const QString& text, int scale,
                       int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("Type 1 font render limits exceeded.");
        return {};
    }
    QHash<quint32, const font::Type1Glyph*> glyphs;
    for (const font::Type1Glyph& glyph : fontData.glyphs)
        if (!glyphs.contains(glyph.codepoint)) glyphs.insert(glyph.codepoint, &glyph);
    QVector<QPainterPath> paths;
    QRectF bounds;
    double penX = 0;
    double widest = 0;
    int line = 0;
    bool hasPreviousGlyph = false;
    const double lineHeight = fontData.ascent - fontData.descent;
    for (int i = 0; i < text.size();) {
        if (text.at(i) == QLatin1Char('\n')) {
            widest = qMax(widest, penX);
            penX = 0;
            hasPreviousGlyph = false;
            ++line;
            ++i;
            continue;
        }
        quint32 codepoint;
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            codepoint = QChar::surrogateToUcs4(text.at(i), text.at(i + 1)), i += 2;
        else codepoint = text.at(i++).unicode();
        if (hasPreviousGlyph && fontData.paintType == 1)
            penX += fontData.strokeWidth;
        const font::Type1Glyph* glyph = glyphs.value(codepoint, nullptr);
        if (!glyph) glyph = glyphs.value(0, nullptr);
        if (!glyph) {
            penX += fontData.unitsPerEm * 0.5;
            hasPreviousGlyph = true;
            continue;
        }
        const QPainterPath positioned = QTransform(1, 0, 0, -1, penX,
            line * lineHeight + fontData.ascent).map(type1PainterPath(*glyph));
        if (!positioned.isEmpty()) {
            paths.push_back(positioned);
            bounds = bounds.isNull() ? positioned.boundingRect()
                                     : bounds.united(positioned.boundingRect());
        }
        penX += glyph->advance;
        hasPreviousGlyph = true;
    }
    widest = qMax(widest, penX);
    const double width = qMax(widest, fontData.unitsPerEm * 0.1);
    const QRectF layout(0, 0, width, qMax(1, line + 1) * lineHeight);
    bounds = bounds.isNull() ? layout : bounds.united(layout);
    const double factor = 16.0 * scale / fontData.unitsPerEm;
    const int imageWidth = qCeil(bounds.width() * factor) + padding * 2;
    const int imageHeight = qCeil(bounds.height() * factor) + padding * 2;
    if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > 16384 || imageHeight > 16384 ||
        qint64(imageWidth) * imageHeight > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("Type 1 font render dimensions exceed limits.");
        return {};
    }
    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (fontData.paintType == 1) {
        painter.setPen(QPen(QColor::fromRgba(foreground), fontData.strokeWidth, Qt::SolidLine,
                            Qt::FlatCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor::fromRgba(foreground));
    }
    painter.setWorldTransform(QTransform(factor, 0, 0, factor,
        padding - bounds.left() * factor, padding - bounds.top() * factor));
    for (const QPainterPath& path : paths) painter.drawPath(path);
    return image;
}

QImage renderType1Glyph(const font::Type1Font& fontData, quint32 glyphIndex, int scale,
                        int padding, QRgb foreground, QRgb background, QString* error)
{
    if (glyphIndex >= quint32(fontData.glyphs.size())) {
        if (error) *error = QStringLiteral("Type 1 glyph index is out of range.");
        return {};
    }
    const font::Type1Glyph& glyph = fontData.glyphs.at(int(glyphIndex));
    font::Type1Font singleGlyphFont;
    singleGlyphFont.unitsPerEm = fontData.unitsPerEm;
    singleGlyphFont.ascent = fontData.ascent;
    singleGlyphFont.descent = fontData.descent;
    singleGlyphFont.paintType = fontData.paintType;
    singleGlyphFont.strokeWidth = fontData.strokeWidth;
    singleGlyphFont.glyphs.push_back(glyph);
    singleGlyphFont.glyphs.first().codepoint = 'A';
    return renderType1Font(singleGlyphFont, QStringLiteral("A"), scale, padding,
                           foreground, background, error);
}

QImage renderSvgFont(const font::SvgFont& fontData, const QString& text, int scale,
                     int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("SVG font render limits exceeded.");
        return {};
    }
    QHash<QString, const font::SvgGlyph*> glyphs;
    int maxUnicodeLength = 1;
    for (const font::SvgGlyph& glyph : fontData.glyphs) {
        if (!glyphs.contains(glyph.unicode)) glyphs.insert(glyph.unicode, &glyph);
        maxUnicodeLength = qMax(maxUnicodeLength, glyph.unicode.size());
    }

    QVector<QPainterPath> positioned;
    QRectF bounds;
    const double lineHeight = fontData.ascent - fontData.descent;
    double penX = 0;
    double widest = 0;
    int line = 0;
    for (int i = 0; i < text.size();) {
        if (text.at(i) == QLatin1Char('\n')) {
            widest = qMax(widest, penX);
            penX = 0;
            ++line;
            ++i;
            continue;
        }
        QString key;
        const int limit = qMin(maxUnicodeLength, text.size() - i);
        for (int length = limit; length > 0; --length) {
            if (i + length < text.size() && text.at(i + length - 1).isHighSurrogate() &&
                text.at(i + length).isLowSurrogate())
                continue;
            const QString candidate = text.mid(i, length);
            if (glyphs.contains(candidate)) {
                key = candidate;
                break;
            }
        }
        if (key.isEmpty()) {
            const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size() &&
                text.at(i + 1).isLowSurrogate();
            key = pair ? text.mid(i, 2) : text.mid(i, 1);
        }
        i += key.size();
        const font::SvgGlyph* glyph = glyphs.value(key, nullptr);
        const double advance = glyph ? glyph->advance : fontData.missingAdvance;
        const QVector<font::SvgPathCommand>* source = glyph ? &glyph->path : &fontData.missingGlyph;
        if (!source->isEmpty()) {
            const QTransform transform(1, 0, 0, -1, penX, line * lineHeight + fontData.ascent);
            const QPainterPath path = transform.map(svgPainterPath(*source));
            positioned.push_back(path);
            bounds = bounds.isNull() ? path.boundingRect() : bounds.united(path.boundingRect());
        }
        penX += advance;
    }
    widest = qMax(widest, penX);
    const QRectF layoutBounds(0, 0, qMax(widest, fontData.unitsPerEm * 0.1),
        qMax(1, line + 1) * lineHeight);
    bounds = bounds.isNull() ? layoutBounds : bounds.united(layoutBounds);
    const double factor = 16.0 * scale / fontData.unitsPerEm;
    const int width = qCeil(bounds.width() * factor) + 2 * padding;
    const int height = qCeil(bounds.height() * factor) + 2 * padding;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        qint64(width) * height > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("SVG font render dimensions exceed limits.");
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgba(foreground));
    painter.setWorldTransform(QTransform(factor, 0, 0, factor,
        padding - bounds.left() * factor, padding - bounds.top() * factor));
    for (const QPainterPath& path : positioned)
        painter.drawPath(path);
    return image;
}

QPainterPath sfnContourPath(const font::SfnFragment& fragment)
{
    QPainterPath path;
    for (const font::SfnContourCommand& command : fragment.commands) {
        const QPointF end(command.end.x, command.end.y);
        if (command.type == 0)
            path.moveTo(end);
        else if (command.type == 1)
            path.lineTo(end);
        else if (command.type == 2)
            path.quadTo(QPointF(command.control1.x, command.control1.y), end);
        else
            path.cubicTo(QPointF(command.control1.x, command.control1.y),
                         QPointF(command.control2.x, command.control2.y), end);
    }
    path.closeSubpath();
    return path;
}

QImage sfnFragmentImage(const font::SfnFont& fontData, const font::SfnFragment& fragment,
                        int colorIndex, QRgb foreground)
{
    if (fragment.type != font::SfnFragmentType::Bitmap &&
        fragment.type != font::SfnFragmentType::Pixmap)
        return {};
    QImage image(fragment.width, fragment.height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < fragment.height; ++y) {
        for (int x = 0; x < fragment.width; ++x) {
            QRgb color = 0;
            if (fragment.type == font::SfnFragmentType::Bitmap) {
                const int offset = y * (fragment.width / 8) + x / 8;
                if (quint8(fragment.pixels.at(offset)) & (1 << (x & 7)))
                    color = colorIndex >= 0 ? [&]() {
                        const font::SfnColor& value = fontData.colors.at(colorIndex);
                        return qRgba(value.red, value.green, value.blue, value.alpha);
                    }() : foreground;
            } else {
                const quint8 index = quint8(fragment.pixels.at(y * fragment.width + x));
                if (index == 254) {
                    if (colorIndex >= 0) {
                        const font::SfnColor& value = fontData.colors.at(colorIndex);
                        color = qRgba(value.red, value.green, value.blue, value.alpha);
                    } else {
                        color = foreground;
                    }
                } else if (index < 254) {
                    const font::SfnColor& value = fontData.colors.at(index);
                    color = qRgba(value.red, value.green, value.blue, value.alpha);
                }
            }
            if (color)
                image.setPixel(x, y, color);
        }
    }
    return image;
}

QImage renderSfnFont(const font::SfnFont& fontData, const QString& text, int scale,
                     int padding, QRgb foreground, QRgb background, QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("SFN font render limits exceeded.");
        return {};
    }
    struct PaintLayer {
        QPainterPath path;
        QImage image;
        QPointF position;
        QRgb color = 0;
        bool isContour = false;
    };
    QVector<PaintLayer> layers;
    QRectF bounds;
    double penX = 0;
    double widest = 0;
    int line = 0;
    for (int i = 0; i < text.size();) {
        if (text.at(i) == QLatin1Char('\n')) {
            widest = qMax(widest, penX);
            penX = 0;
            ++line;
            continue;
        }
        quint32 codepoint;
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            codepoint = QChar::surrogateToUcs4(text.at(i), text.at(i + 1)), i += 2;
        else
            codepoint = text.at(i++).unicode();
        int glyphIndex = fontData.glyphByCodepoint.value(codepoint, -1);
        if (glyphIndex < 0)
            glyphIndex = fontData.glyphByCodepoint.value(0, -1);
        if (glyphIndex < 0) {
            penX += fontData.width;
            continue;
        }
        const font::SfnGlyph& glyph = fontData.glyphs.at(glyphIndex);
        QPainterPath contourPath;
        int contourColorIndex = -2;
        QRgb contourColor = foreground;
        const auto flushContour = [&]() {
            if (contourPath.isEmpty())
                return;
            contourPath.setFillRule(Qt::OddEvenFill);
            PaintLayer paint;
            paint.path = contourPath;
            paint.color = contourColor;
            paint.isContour = true;
            layers.push_back(paint);
            contourPath = QPainterPath();
            contourColorIndex = -2;
        };
        for (const font::SfnLayer& layer : glyph.layers) {
            const font::SfnFragment& fragment = fontData.fragments.at(int(layer.fragment));
            const QPointF position(penX + layer.x, line * fontData.height + layer.y);
            if (fragment.type == font::SfnFragmentType::Contour) {
                const int colorIndex = layer.color;
                if (!contourPath.isEmpty() && colorIndex != contourColorIndex)
                    flushContour();
                if (contourPath.isEmpty()) {
                    contourColorIndex = colorIndex;
                    if (colorIndex >= 0) {
                        const font::SfnColor& color = fontData.colors.at(colorIndex);
                        contourColor = qRgba(color.red, color.green, color.blue, color.alpha);
                    } else {
                        contourColor = foreground;
                    }
                }
                const QPainterPath path = QTransform::fromTranslate(position.x(), position.y()).map(
                    sfnContourPath(fragment));
                if (!path.isEmpty()) {
                    contourPath.addPath(path);
                    bounds = bounds.isNull() ? path.boundingRect() : bounds.united(path.boundingRect());
                }
            } else if (fragment.type == font::SfnFragmentType::Bitmap ||
                       fragment.type == font::SfnFragmentType::Pixmap) {
                flushContour();
                PaintLayer paint;
                paint.image = sfnFragmentImage(fontData, fragment, layer.color, foreground);
                paint.position = position;
                bounds = bounds.isNull() ? QRectF(position, paint.image.size())
                                         : bounds.united(QRectF(position, paint.image.size()));
                layers.push_back(paint);
            } else {
                flushContour();
            }
        }
        flushContour();
        penX += glyph.advanceX ? glyph.advanceX : fontData.width;
    }
    widest = qMax(widest, penX);
    const QRectF layoutBounds(0, 0, qMax(widest, double(fontData.width)),
        qMax(1, line + 1) * double(fontData.height));
    bounds = bounds.isNull() ? layoutBounds : bounds.united(layoutBounds);
    const double factor = 16.0 * scale / fontData.height;
    const int width = qCeil(bounds.width() * factor) + 2 * padding;
    const int height = qCeil(bounds.height() * factor) + 2 * padding;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        qint64(width) * height > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("SFN font render dimensions exceed limits.");
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgba(foreground));
    painter.setWorldTransform(QTransform(factor, 0, 0, factor,
        padding - bounds.left() * factor, padding - bounds.top() * factor));
    for (const PaintLayer& layer : layers) {
        if (layer.isContour) {
            painter.setBrush(QColor::fromRgba(layer.color));
            painter.drawPath(layer.path);
        } else {
            painter.drawImage(layer.position, layer.image);
        }
    }
    return image;
}

} // namespace

bool RT_FONT::LooksLikeWindowsFnt(const QByteArray& resData)
{
    font::AfmFont metrics;
    if (font::parsePfm(resData, &metrics, nullptr)) return false;
    Header header;
    return parseHeader(resData, header);
}

bool RT_FONT::LooksLikeOs2Fnt(const QByteArray& resData)
{
    return OS2_RT_FONT::LooksLike(resData);
}

bool RT_FONT::LooksLikeBitmapFont(const QByteArray& resData)
{
    font::BitmapFont parsed;
    return font::parse(resData, &parsed, nullptr) && parsed.format != font::Format::MacNfnt;
}

bool RT_FONT::LooksLikeOutlineFont(const QByteArray& resData)
{
    font::SfntFont parsed;
    return font::parseSfnt(resData, &parsed, nullptr);
}

bool RT_FONT::LooksLikeMetricsFont(const QByteArray& resData)
{
    font::AfmFont parsed;
    return font::parseAfm(resData, &parsed, nullptr) ||
           font::parsePfm(resData, &parsed, nullptr);
}

bool RT_FONT::LooksLikeSvgFont(const QByteArray& resData)
{
    font::SvgFont parsed;
    return font::parseSvgFont(resData, &parsed, nullptr);
}

bool RT_FONT::LooksLikeSfnFont(const QByteArray& resData)
{
    font::SfnFont parsed;
    return font::parseSfnFont(resData, &parsed, nullptr);
}

bool RT_FONT::LooksLikeShpFont(const QByteArray& resData)
{
    font::ShpFont parsed;
    return font::parseShpFont(resData, &parsed, nullptr);
}

bool RT_FONT::LooksLikeFont(const QByteArray& resData)
{
    font::AmigaFontContents amigaFont;
    font::MacFondFont macFond;
    font::BitmapFont macBitmap;
    return LooksLikeOs2Fnt(resData) || LooksLikeWindowsFnt(resData) ||
        LooksLikeBitmapFont(resData) || LooksLikeMetricsFont(resData) ||
        LooksLikeSvgFont(resData) || LooksLikeSfnFont(resData) ||
        LooksLikeShpFont(resData) || LooksLikeOutlineFont(resData) ||
        font::hasDwfHeader(resData) ||
        font::hasType1Header(resData) || font::hasType3Header(resData) ||
        font::hasSnfHeader(resData) ||
        font::parseMacFond(resData, &macFond, nullptr) ||
        (font::parse(resData, &macBitmap, nullptr) &&
         macBitmap.format == font::Format::MacNfnt) ||
        font::parseAmigaFontContents(resData, &amigaFont, nullptr);
}

QImage RT_FONT::renderText(const ResourceEntry& entry, const QString& text, int scale,
                           int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                           QString* error)
{
    if (error) error->clear();
    font::SnfFont snfFont;
    if (font::parseSnf(entry.data, &snfFont, nullptr)) {
        if (error) *error = QStringLiteral("SNF glyph pixels depend on external X server bit and scanline settings.");
        return {};
    }
    font::ShpFont shpFont;
    if (font::parseShpFont(entry.data, &shpFont, nullptr)) {
        if (entry.codePage && shpFont.unicode && shpFont.encoding == 1)
            font::mapPackedShpUnicode(&shpFont, entry.codePage, nullptr);
        return renderShpFont(shpFont, text, scale, padding,
                             foregroundRgba, backgroundRgba, error);
    }
    font::Type1Font type1Font;
    if (font::parseType3Font(entry.data, &type1Font, nullptr) ||
        font::parseType1Font(entry.data, &type1Font, nullptr))
        return renderType1Font(type1Font, text, scale, padding,
                               foregroundRgba, backgroundRgba, error);
    font::SvgFont svgFont;
    if (font::parseSvgFont(entry.data, &svgFont, nullptr))
        return renderSvgFont(svgFont, text, scale, padding,
                             foregroundRgba, backgroundRgba, error);
    font::SfnFont sfnFont;
    if (font::parseSfnFont(entry.data, &sfnFont, nullptr))
        return renderSfnFont(sfnFont, text, scale, padding,
                             foregroundRgba, backgroundRgba, error);
    font::AfmFont metricsFont;
    if (font::parseAfm(entry.data, &metricsFont, nullptr)) {
        if (error) *error = QStringLiteral("AFM contains metrics but no glyph outlines.");
        return {};
    }
    if (font::parsePfm(entry.data, &metricsFont, nullptr)) {
        if (error) *error = QStringLiteral("PFM contains metrics but no glyph outlines.");
        return {};
    }
    font::BitmapFont bitmapFont;
    if (font::parse(entry.data, &bitmapFont, nullptr))
        return renderBitmapFont(bitmapFont, text, scale, padding,
                                foregroundRgba, backgroundRgba, error);
    font::AmigaFontContents amigaFont;
    if (font::parseAmigaFontContents(entry.data, &amigaFont, nullptr)) {
        if (error) *error = QStringLiteral("Amiga font contents list strikes but has no glyph data.");
        return {};
    }
    font::SfntFont outlineFont;
    if (font::parseSfnt(entry.data, &outlineFont, nullptr))
        return renderOutlineFont(outlineFont, text, scale, padding,
                                 foregroundRgba, backgroundRgba, error);
    font::MacFondFont fondFont;
    if (font::parseMacFond(entry.data, &fondFont, nullptr)) {
        if (error) *error = QStringLiteral("Macintosh FOND resources contain family metadata, not glyph data.");
        return {};
    }
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

QImage RT_FONT::renderGlyphIndex(const ResourceEntry& entry, quint32 glyphIndex, int scale,
                                 int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                                 QString* error)
{
    font::Type1Font type1Font;
    if (font::parseType3Font(entry.data, &type1Font, nullptr) ||
        font::parseType1Font(entry.data, &type1Font, nullptr))
        return renderType1Glyph(type1Font, glyphIndex, scale, padding, foregroundRgba,
                                backgroundRgba, error);
    font::SnfFont snfFont;
    if (font::parseSnf(entry.data, &snfFont, nullptr)) {
        if (error) *error = QStringLiteral("SNF glyph pixels depend on external X server bit and scanline settings.");
        return {};
    }
    font::SfntFont outlineFont;
    if (font::parseSfnt(entry.data, &outlineFont, nullptr))
        return renderOutlineGlyph(outlineFont, glyphIndex, scale, padding, foregroundRgba,
                                  backgroundRgba, error);
    font::SvgFont svgFont;
    if (font::parseSvgFont(entry.data, &svgFont, nullptr)) {
        if (glyphIndex >= quint32(svgFont.glyphs.size())) {
            if (error) *error = QStringLiteral("SVG font glyph index is out of range.");
            return {};
        }
        font::SvgFont singleGlyphFont;
        singleGlyphFont.unitsPerEm = svgFont.unitsPerEm;
        singleGlyphFont.ascent = svgFont.ascent;
        singleGlyphFont.descent = svgFont.descent;
        font::SvgGlyph glyph = svgFont.glyphs.at(int(glyphIndex));
        glyph.unicode = QStringLiteral("A");
        singleGlyphFont.glyphs.clear();
        singleGlyphFont.glyphs.push_back(glyph);
        return renderSvgFont(singleGlyphFont, QStringLiteral("A"), scale, padding,
                             foregroundRgba, backgroundRgba, error);
    }
    font::BitmapFont bitmapFont;
    if (font::parse(entry.data, &bitmapFont, nullptr))
        return renderBitmapGlyph(bitmapFont, glyphIndex, scale, padding, foregroundRgba,
                                 backgroundRgba, error);
    font::SfnFont sfnFont;
    if (font::parseSfnFont(entry.data, &sfnFont, nullptr)) {
        if (glyphIndex >= quint32(sfnFont.glyphs.size())) {
            if (error) *error = QStringLiteral("SFN glyph index is out of range.");
            return {};
        }
        const quint32 codepoint = sfnFont.glyphs.at(int(glyphIndex)).codepoint;
        return renderSfnFont(sfnFont, QString::fromUcs4(&codepoint, 1), scale, padding,
                             foregroundRgba, backgroundRgba, error);
    }
    font::ShpFont shpFont;
    if (font::parseShpFont(entry.data, &shpFont, nullptr)) {
        if (glyphIndex >= quint32(shpFont.glyphs.size())) {
            if (error) *error = QStringLiteral("SHP glyph index is out of range.");
            return {};
        }
        QHash<quint16, const font::ShpGlyph*> glyphs;
        for (const font::ShpGlyph& glyph : shpFont.glyphs)
            glyphs.insert(glyph.code, &glyph);
        return renderShpGlyph(shpFont, glyphs, glyphIndex, scale, padding,
                              foregroundRgba, backgroundRgba, error);
    }
    font::MacFondFont fondFont;
    if (font::parseMacFond(entry.data, &fondFont, nullptr)) {
        if (error) *error = QStringLiteral("Macintosh FOND resources contain family metadata, not glyph data.");
        return {};
    }
    if (error) *error = QStringLiteral("Glyph-index rendering needs a bitmap or SFNT font.");
    return {};
}

QVector<QImage> RT_FONT::renderGlyphIndexes(const ResourceEntry& entry,
                                           const QVector<quint32>& glyphIndexes, int scale,
                                           int padding, QRgb foregroundRgba,
                                           QRgb backgroundRgba, QString* error)
{
    if (glyphIndexes.size() > 8192) {
        if (error) *error = QStringLiteral("Font glyph batch exceeds the render limit.");
        return {};
    }
    font::SfntFont sfntFont;
    if (font::parseSfnt(entry.data, &sfntFont, nullptr)) {
        if (scale < 1 || scale > 16 || padding < 0 || padding > 4096) {
            if (error) *error = QStringLiteral("Outline glyph render limits exceeded.");
            return {};
        }
        if (sfntFont.hasCff2) {
            if (error) *error = QStringLiteral("CFF2 rendering is not supported by the Qt outline backend.");
            return {};
        }
#ifdef Q_OS_WIN
        if (sfntFont.format == font::SfntFormat::TrueType && sfntFont.hasTrueTypeOutlines)
            return font::renderSfntGlyphsWithGdi(sfntFont, glyphIndexes, scale, padding,
                                                  foregroundRgba, backgroundRgba, error);
#endif
        if (!ensureFontGuiApplication()) {
            if (error) *error = QStringLiteral("Outline font rendering requires a Qt GUI application.");
            return {};
        }
        const QRawFont raw(sfntFont.sfntBytes, 16 * scale, QFont::PreferDefaultHinting);
        if (!raw.isValid()) {
            if (error) *error = QStringLiteral("Qt could not load the requested outline glyph.");
            return {};
        }
        QVector<QImage> images;
        images.reserve(glyphIndexes.size());
        quint64 totalPixels = 0;
        for (quint32 index : glyphIndexes) {
            QImage image = renderOutlineGlyph(sfntFont, raw, index, scale, padding,
                                              foregroundRgba, backgroundRgba, error);
            if (image.isNull()) return {};
            totalPixels += quint64(image.width()) * quint64(image.height());
            if (totalPixels > 16u * 1024u * 1024u) {
                if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
                return {};
            }
            images.push_back(std::move(image));
        }
        return images;
    }
    font::Type1Font type1Font;
    if (font::parseType3Font(entry.data, &type1Font, nullptr) ||
        font::parseType1Font(entry.data, &type1Font, nullptr)) {
        QVector<QImage> images;
        images.reserve(glyphIndexes.size());
        quint64 totalPixels = 0;
        for (quint32 index : glyphIndexes) {
            if (index >= quint32(type1Font.glyphs.size())) {
                if (error) *error = QStringLiteral("Type 1 glyph index is out of range.");
                return {};
            }
            QImage image = renderType1Glyph(type1Font, index, scale, padding,
                                            foregroundRgba, backgroundRgba, error);
            if (image.isNull()) {
                if (error) error->clear();
                image = QImage(1, 1, QImage::Format_ARGB32);
                image.fill(backgroundRgba);
            }
            totalPixels += quint64(image.width()) * quint64(image.height());
            if (totalPixels > 16u * 1024u * 1024u) {
                if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
                return {};
            }
            images.push_back(std::move(image));
        }
        return images;
    }
    font::BitmapFont bitmapFont;
    if (font::parse(entry.data, &bitmapFont, nullptr)) {
        QVector<QImage> images;
        images.reserve(glyphIndexes.size());
        quint64 totalPixels = 0;
        for (quint32 index : glyphIndexes) {
            if (index >= quint32(bitmapFont.glyphs.size())) {
                if (error) *error = QStringLiteral("Bitmap glyph index is out of range.");
                return {};
            }
            QImage image = renderBitmapGlyph(bitmapFont, index, scale, padding,
                                             foregroundRgba, backgroundRgba, error);
            if (image.isNull()) return {};
            totalPixels += quint64(image.width()) * quint64(image.height());
            if (totalPixels > 16u * 1024u * 1024u) {
                if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
                return {};
            }
            images.push_back(std::move(image));
        }
        return images;
    }
    font::ShpFont shpFont;
    if (font::parseShpFont(entry.data, &shpFont, nullptr)) {
        QHash<quint16, const font::ShpGlyph*> glyphs;
        for (const font::ShpGlyph& glyph : shpFont.glyphs)
            glyphs.insert(glyph.code, &glyph);
        QVector<QImage> images;
        images.reserve(glyphIndexes.size());
        quint64 totalPixels = 0;
        for (quint32 index : glyphIndexes) {
            if (index >= quint32(shpFont.glyphs.size())) {
                if (error) *error = QStringLiteral("SHP glyph index is out of range.");
                return {};
            }
            QImage image = renderShpGlyph(shpFont, glyphs, index, scale, padding,
                                          foregroundRgba, backgroundRgba, error);
            if (image.isNull()) {
                if (error) error->clear();
                image = QImage(1, 1, QImage::Format_ARGB32);
                image.fill(backgroundRgba);
            }
            totalPixels += quint64(image.width()) * quint64(image.height());
            if (totalPixels > 16u * 1024u * 1024u) {
                if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
                return {};
            }
            images.push_back(std::move(image));
        }
        return images;
    }
    QVector<QImage> images;
    images.reserve(glyphIndexes.size());
    quint64 totalPixels = 0;
    for (quint32 index : glyphIndexes) {
        QImage image = renderGlyphIndex(entry, index, scale, padding, foregroundRgba,
                                        backgroundRgba, error);
        if (image.isNull()) return {};
        totalPixels += quint64(image.width()) * quint64(image.height());
        if (totalPixels > 16u * 1024u * 1024u) {
            if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
            return {};
        }
        images.push_back(std::move(image));
    }
    return images;
}

ResourcePreview RT_FONT::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;

    font::SnfFont snfFont;
    if (font::parseSnf(entry.data, &snfFont, nullptr)) {
        QStringList metrics;
        const int shownGlyphs = qMin(snfFont.glyphs.size(), 256);
        for (int index = 0; index < shownGlyphs; ++index) {
            const font::SnfGlyphMetric& glyph = snfFont.glyphs.at(index);
            metrics.append(QStringLiteral("%1: bearing %2..%3; advance %4; ascent %5; descent %6")
                .arg(glyph.code).arg(glyph.leftBearing).arg(glyph.rightBearing)
                .arg(glyph.advance).arg(glyph.ascent).arg(glyph.descent));
        }
        if (shownGlyphs < snfFont.glyphs.size())
            metrics.append(QStringLiteral("%1 more glyph metric records")
                .arg(snfFont.glyphs.size() - shownGlyphs));
        preview.text = QStringLiteral("X11 Server Normal Format - %1 - %2 defined slots of %3 - slot-code range %4-%5 - %6 properties - glyph pixels are server-dependent\n%7")
            .arg(snfFont.name.isEmpty() ? QStringLiteral("unnamed") : snfFont.name)
            .arg(snfFont.glyphs.size()).arg(snfFont.glyphSlotCount)
            .arg(snfFont.firstCode).arg(snfFont.lastCode).arg(snfFont.propertyCount)
            .arg(metrics.join(QLatin1Char('\n')));
        return preview;
    }

    font::Type1Font type1Font;
    if (font::parseType3Font(entry.data, &type1Font, nullptr) ||
        font::parseType1Font(entry.data, &type1Font, nullptr)) {
        const QString fontType = font::hasType3Header(entry.data)
            ? QStringLiteral("PostScript Type 3") : QStringLiteral("PostScript Type 1");
        preview.text = QStringLiteral("%1 - %2 - %3 glyphs - %4 units per em")
            .arg(type1Font.name, fontType).arg(type1Font.glyphs.size()).arg(type1Font.unitsPerEm);
        return preview;
    }

    font::AfmFont metricsFont;
    if (font::parseAfm(entry.data, &metricsFont, nullptr)) {
        QStringList metrics;
        const int shownGlyphs = qMin(metricsFont.glyphs.size(), 512);
        for (int index = 0; index < shownGlyphs; ++index) {
            const font::AfmGlyph& glyph = metricsFont.glyphs.at(index);
            const QString name = glyph.name.isEmpty()
                ? QStringLiteral("(unnamed)") : glyph.name;
            const QString code = glyph.code < 0
                ? QStringLiteral("unencoded") : QString::number(glyph.code);
            metrics.append(QStringLiteral("%1; code %2; advance %3")
                .arg(name, code).arg(glyph.advance));
        }
        if (shownGlyphs < metricsFont.glyphs.size())
            metrics.append(QStringLiteral("%1 additional glyph metrics")
                .arg(metricsFont.glyphs.size() - shownGlyphs));
        preview.text = QStringLiteral("%1 - Adobe Font Metrics - %2 glyph metrics - %3 x %4 font bounds - outlines not included\n%5")
            .arg(metricsFont.familyName).arg(metricsFont.glyphs.size())
            .arg(metricsFont.bboxWidth).arg(metricsFont.bboxHeight)
            .arg(metrics.join(QLatin1Char('\n')));
        return preview;
    }

    if (font::parsePfm(entry.data, &metricsFont, nullptr)) {
        QStringList metrics;
        const int shownGlyphs = qMin(metricsFont.glyphs.size(), 512);
        for (int index = 0; index < shownGlyphs; ++index) {
            const font::AfmGlyph& glyph = metricsFont.glyphs.at(index);
            metrics.append(QStringLiteral("character %1; advance %2")
                .arg(glyph.code).arg(glyph.advance));
        }
        if (shownGlyphs < metricsFont.glyphs.size())
            metrics.append(QStringLiteral("%1 additional character metrics")
                .arg(metricsFont.glyphs.size() - shownGlyphs));
        preview.text = QStringLiteral("%1 - Windows Printer Font Metrics - %2 character metrics - ascent %3 - descent %4 - outlines not included\n%5")
            .arg(metricsFont.familyName).arg(metricsFont.glyphs.size())
            .arg(metricsFont.ascender).arg(-metricsFont.descender)
            .arg(metrics.join(QLatin1Char('\n')));
        return preview;
    }

    font::ShpFont shpFont;
    if (font::parseShpFont(entry.data, &shpFont, nullptr)) {
        const QString variant = shpFont.bigFont
            ? (shpFont.compiled ? QStringLiteral("BigFont SHX") : QStringLiteral("BigFont SHP"))
            : shpFont.compiled ? QStringLiteral("SHX") : QStringLiteral("SHP");
        preview.text = QStringLiteral("%1 - AutoCAD %2 vector font - %3 glyphs - ascent %4 - descent %5")
            .arg(shpFont.name, variant)
            .arg(shpFont.glyphs.size()).arg(shpFont.above).arg(shpFont.below);
        return preview;
    }

    font::SfnFont sfnFont;
    if (font::parseSfnFont(entry.data, &sfnFont, nullptr)) {
        preview.text = QStringLiteral("%1 - Scalable Screen Font 2 - %2 glyphs - %3 x %4 grid")
            .arg(sfnFont.familyName).arg(sfnFont.glyphs.size())
            .arg(sfnFont.width).arg(sfnFont.height);
        return preview;
    }

    font::SvgFont svgFont;
    if (font::parseSvgFont(entry.data, &svgFont, nullptr)) {
        preview.text = QStringLiteral("%1 - SVG font - %2 glyphs - %3 units per em")
            .arg(svgFont.familyName).arg(svgFont.glyphs.size()).arg(svgFont.unitsPerEm);
        return preview;
    }

    font::SfntFont sfntFont;
    if (font::parseSfnt(entry.data, &sfntFont, nullptr)) {
        preview.text = QStringLiteral("%1 %2 - %3 glyphs - %4 units per em")
            .arg(sfntFont.familyName, sfntFont.styleName)
            .arg(sfntFont.glyphCount).arg(sfntFont.unitsPerEm);
        return preview;
    }

    font::BitmapFont bitmapFont;
    if (font::parse(entry.data, &bitmapFont, nullptr)) {
        QString formatName;
        switch (bitmapFont.format) {
        case font::Format::Bdf: formatName = QStringLiteral("BDF bitmap font"); break;
        case font::Format::Bmf: formatName = QStringLiteral("ByteMap Font"); break;
        case font::Format::Psf1: formatName = QStringLiteral("PC Screen Font 1"); break;
        case font::Format::Psf2: formatName = QStringLiteral("PC Screen Font 2"); break;
        case font::Format::Pcf: formatName = QStringLiteral("Portable Compiled Format font"); break;
        case font::Format::Fzx: formatName = QStringLiteral("ZX Spectrum FZX font"); break;
        case font::Format::Pk: formatName = QStringLiteral("TeX Packed Font"); break;
        case font::Format::MacNfnt: formatName = QStringLiteral("Macintosh FONT/NFNT bitmap font"); break;
        case font::Format::Amiga: formatName = QStringLiteral("Amiga bitmap font strike"); break;
        case font::Format::AmigaColor: formatName = QStringLiteral("Amiga ColorFont strike"); break;
        case font::Format::AmigaAnim: formatName = QStringLiteral("Amiga AnimFont"); break;
        case font::Format::Vfont: formatName = QStringLiteral("Sun VFONT bitmap font"); break;
        case font::Format::SunNews: formatName = QStringLiteral("Sun X11/NeWS bitmap font"); break;
        }
        preview.text = QStringLiteral("%1 - %2 - %3 glyphs - %4 x %5 pixels")
            .arg(bitmapFont.name.isEmpty() ? formatName : bitmapFont.name, formatName)
            .arg(bitmapFont.glyphs.size()).arg(bitmapFont.width).arg(bitmapFont.height);
        return preview;
    }
    if (font::hasDwfHeader(entry.data)) {
        QString error;
        font::parseDwf(entry.data, &bitmapFont, &error);
        preview.text = QStringLiteral("DECwindows Font - %1")
            .arg(error.isEmpty() ? QStringLiteral("unsupported layout") : error);
        return preview;
    }

    font::MacFondFont fondFont;
    if (font::parseMacFond(entry.data, &fondFont, nullptr)) {
        QStringList associations;
        for (const font::MacFondAssociation& association : fondFont.associations) {
            associations.append(QStringLiteral("%1 pt/style 0x%2 -> resource %3")
                .arg(association.pointSize)
                .arg(association.style, 4, 16, QLatin1Char('0'))
                .arg(association.resourceId));
        }
        const QString familyName = entry.name.isEmpty()
            ? QStringLiteral("Macintosh FOND family") : entry.name;
        preview.text = QStringLiteral("%1 - Macintosh FOND - family ID %2 - character codes %3-%4 - %5 associated fonts - width tables %6 - %7")
            .arg(familyName).arg(fondFont.familyId).arg(fondFont.firstCharacter)
            .arg(fondFont.lastCharacter).arg(fondFont.associations.size())
            .arg(fondFont.widthTableCount)
            .arg(associations.join(QStringLiteral("; ")));
        return preview;
    }
    font::AmigaFontContents amigaFont;
    if (font::parseAmigaFontContents(entry.data, &amigaFont, nullptr)) {
        QStringList strikes;
        for (const font::AmigaFontStrike& strike : amigaFont.strikes)
            strikes.append(QStringLiteral("%1 - %2 px - style 0x%3 - flags 0x%4")
                .arg(strike.fileName).arg(strike.pixelHeight)
                .arg(strike.style, 2, 16, QLatin1Char('0'))
                .arg(strike.flags, 2, 16, QLatin1Char('0')));
        preview.text = QStringLiteral("Amiga font contents - %1 strikes - strike files contain glyph data\n%2")
            .arg(amigaFont.strikes.size()).arg(strikes.join(QLatin1Char('\n')));
        return preview;
    }

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
