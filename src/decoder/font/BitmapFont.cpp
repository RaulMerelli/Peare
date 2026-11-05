#include "BitmapFont.h"
#include "AmigaFont.h"
#include "AmigaAnimFont.h"
#include "DwfFont.h"
#include "SunNewsFont.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstring>

namespace peare {
namespace font {
namespace {

const int kMaxGlyphs = 65536;
const int kMaxDimension = 4096;
const int kMaxBitmapBytes = 64 * 1024 * 1024;

quint16 le16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) | (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 le32(const QByteArray& data, int offset)
{
    return quint32(quint8(data.at(offset))) | (quint32(quint8(data.at(offset + 1))) << 8) |
           (quint32(quint8(data.at(offset + 2))) << 16) |
           (quint32(quint8(data.at(offset + 3))) << 24);
}

quint16 read16(const QByteArray& data, int offset, bool bigEndian)
{
    const quint16 first = quint8(data.at(offset));
    const quint16 second = quint8(data.at(offset + 1));
    return bigEndian ? quint16((first << 8) | second) : quint16(first | (second << 8));
}

quint32 read32(const QByteArray& data, int offset, bool bigEndian)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i) {
        const int shift = bigEndian ? (3 - i) * 8 : i * 8;
        value |= quint32(quint8(data.at(offset + i))) << shift;
    }
    return value;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool dimensionOk(int width, int height)
{
    return width > 0 && width <= kMaxDimension && height > 0 && height <= kMaxDimension &&
           qint64(width) * height <= kMaxBitmapBytes * 8LL;
}

bool rangeFits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

bool parseBdf(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!hasBdfHeader(data)) return fail(error, QStringLiteral("Invalid BDF header"));
    int lineCount = 0;
    for (char ch : data) {
        if (ch == '\n' && ++lineCount > 1000000)
            return fail(error, QStringLiteral("BDF line count limit exceeded"));
    }
    const QList<QByteArray> lines = data.split('\n');
    BitmapFont font;
    font.format = Format::Bdf;
    int expectedGlyphs = -1;
    int declaredGlyphs = 0;
    Glyph glyph;
    bool inGlyph = false;
    bool inBitmap = false;
    int bitmapRows = 0;
    int bitmapRowBytes = 0;
    qint64 bitmapTotal = 0;
    bool sawEnd = false;
    int endLine = -1;
    const QRegularExpression whitespace(QStringLiteral("\\s+"));

    for (int lineNo = 0; lineNo < lines.size(); ++lineNo) {
        QByteArray raw = lines.at(lineNo);
        if (raw.endsWith('\r')) raw.chop(1);
        if (raw.size() > 65536) return fail(error, QStringLiteral("BDF line is too long"));
        const QString line = QString::fromLatin1(raw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList fields = line.split(whitespace, Qt::SkipEmptyParts);
        if (fields.isEmpty()) continue;
        const QString key = fields.at(0);
        if (inBitmap) {
            if (key == QLatin1String("ENDCHAR")) {
                if (bitmapRows != glyph.height) return fail(error, QStringLiteral("BDF glyph bitmap row count mismatch"));
                inBitmap = false;
                inGlyph = false;
                font.glyphs.push_back(glyph);
                continue;
            }
            if (bitmapRows >= glyph.height || raw.size() != bitmapRowBytes * 2)
                return fail(error, QStringLiteral("Invalid BDF bitmap row"));
            for (char ch : raw) {
                if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
                      (ch >= 'a' && ch <= 'f')))
                    return fail(error, QStringLiteral("Invalid BDF bitmap hex"));
            }
            const QByteArray row = QByteArray::fromHex(raw);
            if (row.size() != bitmapRowBytes) return fail(error, QStringLiteral("Invalid BDF bitmap row"));
            glyph.bitmap.append(row);
            bitmapTotal += row.size();
            if (bitmapTotal > kMaxBitmapBytes) return fail(error, QStringLiteral("BDF bitmap data limit exceeded"));
            ++bitmapRows;
            continue;
        }
        if (key == QLatin1String("FONT") && fields.size() >= 2) {
            font.name = fields.mid(1).join(QLatin1Char(' '));
        } else if (key == QLatin1String("FONTBOUNDINGBOX") && fields.size() == 5) {
            bool ok[4] = {};
            const int w = fields.at(1).toInt(&ok[0]);
            const int h = fields.at(2).toInt(&ok[1]);
            const int x = fields.at(3).toInt(&ok[2]);
            const int y = fields.at(4).toInt(&ok[3]);
            if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || !dimensionOk(w, h) ||
                x < -kMaxDimension || x > kMaxDimension ||
                y < -kMaxDimension || y > kMaxDimension)
                return fail(error, QStringLiteral("Invalid BDF font bounding box"));
            font.width = w;
            font.height = h;
            font.ascent = qMax(0, h + y);
            font.descent = qMax(0, h - font.ascent);
            Q_UNUSED(x);
        } else if (key == QLatin1String("CHARS") && fields.size() == 2) {
            bool ok = false;
            expectedGlyphs = fields.at(1).toInt(&ok);
            if (!ok || expectedGlyphs <= 0 || expectedGlyphs > kMaxGlyphs)
                return fail(error, QStringLiteral("Invalid BDF glyph count"));
        } else if (key == QLatin1String("FONT_ASCENT") && fields.size() == 2) {
            bool ok = false;
            const int value = fields.at(1).toInt(&ok);
            if (!ok || value < 0 || value > kMaxDimension)
                return fail(error, QStringLiteral("Invalid BDF ascent"));
            font.ascent = value;
            font.descent = qMax(0, font.height - value);
        } else if (key == QLatin1String("STARTCHAR")) {
            if (inGlyph || expectedGlyphs < 0) return fail(error, QStringLiteral("Unexpected BDF STARTCHAR"));
            inGlyph = true;
            glyph = Glyph();
            glyph.advance = font.width;
            ++declaredGlyphs;
            if (declaredGlyphs > expectedGlyphs) return fail(error, QStringLiteral("BDF glyph count exceeds CHARS"));
        } else if (inGlyph && key == QLatin1String("ENCODING") &&
                   (fields.size() == 2 || fields.size() == 3)) {
            bool ok = false;
            int value = fields.at(1).toInt(&ok);
            if (ok && value == -1 && fields.size() == 3)
                value = fields.at(2).toInt(&ok);
            if (!ok || value < -1 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
                return fail(error, QStringLiteral("Invalid BDF character encoding"));
            glyph.codepoint = value < 0 ? 0xffffffffu : quint32(value);
        } else if (inGlyph && key == QLatin1String("DWIDTH") && fields.size() == 3) {
            bool ok = false;
            bool yOk = false;
            glyph.advance = fields.at(1).toInt(&ok);
            fields.at(2).toInt(&yOk);
            if (!ok || !yOk || glyph.advance < 0 || glyph.advance > kMaxDimension)
                return fail(error, QStringLiteral("Invalid BDF glyph advance"));
        } else if (inGlyph && key == QLatin1String("DWIDTH") && fields.size() == 2) {
            bool ok = false;
            glyph.advance = fields.at(1).toInt(&ok);
            if (!ok || glyph.advance < 0 || glyph.advance > kMaxDimension)
                return fail(error, QStringLiteral("Invalid BDF glyph advance"));
        } else if (inGlyph && key == QLatin1String("BBX") && fields.size() == 5) {
            bool ok[4] = {};
            glyph.width = fields.at(1).toInt(&ok[0]);
            glyph.height = fields.at(2).toInt(&ok[1]);
            glyph.bearingX = fields.at(3).toInt(&ok[2]);
            const int yOffset = fields.at(4).toInt(&ok[3]);
            glyph.bearingY = yOffset + glyph.height;
            if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || glyph.width < 0 ||
                glyph.width > kMaxDimension || glyph.height < 0 || glyph.height > kMaxDimension ||
                glyph.bearingX < -kMaxDimension || glyph.bearingX > kMaxDimension ||
                yOffset < -kMaxDimension || yOffset > kMaxDimension)
                return fail(error, QStringLiteral("Invalid BDF glyph bounding box"));
        } else if (inGlyph && key == QLatin1String("BITMAP")) {
            bitmapRows = 0;
            bitmapRowBytes = (glyph.width + 7) / 8;
            inBitmap = true;
        } else if (key == QLatin1String("ENDFONT")) {
            sawEnd = true;
            endLine = lineNo;
            break;
        }
    }
    if (!sawEnd || inGlyph || inBitmap || expectedGlyphs < 0 || declaredGlyphs != expectedGlyphs ||
        font.glyphs.size() != expectedGlyphs || font.width <= 0 || font.height <= 0 ||
        font.name.isEmpty())
        return fail(error, QStringLiteral("Incomplete BDF font"));
    for (int i = endLine + 1; i < lines.size(); ++i) {
        const QByteArray trailing = lines.at(i).trimmed();
        if (!trailing.isEmpty() && !trailing.startsWith('#'))
            return fail(error, QStringLiteral("Unexpected data after ENDFONT"));
    }
    font.fixedWidth = true;
    for (const Glyph& item : font.glyphs)
        if (item.advance != font.glyphs.first().advance) font.fixedWidth = false;
    *out = font;
    return true;
}

bool parseBmf(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!hasBmfHeader(data) || data.size() < 18)
        return fail(error, QStringLiteral("Invalid BMF header"));
    const quint8 version = quint8(data.at(4));
    if (version != 0x11 && version != 0x12)
        return fail(error, QStringLiteral("Unsupported BMF version"));
    const int alphaBits = version == 0x12 ? quint8(data.at(12)) : 0;
    if (alphaBits > 8)
        return fail(error, QStringLiteral("Invalid BMF alpha depth"));
    const int lineHeight = qint8(data.at(5));
    const int ascent = -qint8(data.at(6));
    const int descent = qint8(data.at(7));
    const int addSpace = qint8(data.at(8));
    const int innerHeight = -qint8(data.at(9));
    const int usedColors = quint8(data.at(10));
    const int highestUsedColor = quint8(data.at(11));
    const int paletteSize = quint8(data.at(16));
    if (lineHeight <= 0 || ascent < 0 || innerHeight < 0 ||
        paletteSize == 0 || usedColors > paletteSize + 1 ||
        highestUsedColor > paletteSize)
        return fail(error, QStringLiteral("Invalid BMF metrics or palette size"));
    quint64 cursor = 17;
    const quint64 paletteBytes = quint64(paletteSize) * 3;
    if (!rangeFits(cursor, paletteBytes + 1, quint64(data.size())))
        return fail(error, QStringLiteral("Truncated BMF palette"));
    BitmapFont font;
    font.format = Format::Bmf;
    for (int i = 0; i < paletteSize; ++i) {
        const int offset = int(cursor) + i * 3;
        const quint32 red = quint8(data.at(offset)) * 4u;
        const quint32 green = quint8(data.at(offset + 1)) * 4u;
        const quint32 blue = quint8(data.at(offset + 2)) * 4u;
        font.palette.push_back(0xff000000u | (red << 16) | (green << 8) | blue);
    }
    cursor += paletteBytes;
    const quint8 nameLength = quint8(data.at(int(cursor++)));
    if (!rangeFits(cursor, quint64(nameLength) + 2, quint64(data.size())))
        return fail(error, QStringLiteral("Truncated BMF name or glyph count"));
    font.name = QString::fromUtf8(data.constData() + int(cursor), nameLength).trimmed();
    if (font.name.isEmpty()) font.name = QStringLiteral("ByteMap Font");
    cursor += nameLength;
    font.width = 1;
    font.height = lineHeight;
    font.ascent = ascent;
    font.descent = descent;
    font.fixedWidth = true;
    font.alphaBits = alphaBits;
    font.addSpace = addSpace;
    QSet<quint32> codepoints;
    quint32 maximumWidth = 1;
    const auto readGlyph = [&](quint32 codepoint, BitmapFont* parsed, quint64* offset) -> bool {
        if (!rangeFits(*offset, 5, quint64(data.size())) || codepoints.contains(codepoint))
            return false;
        const int width = quint8(data.at(int(*offset)));
        const int height = quint8(data.at(int(*offset) + 1));
        const int bearingX = qint8(data.at(int(*offset) + 2));
        const int bearingBottom = qint8(data.at(int(*offset) + 3));
        const int shift = qint8(data.at(int(*offset) + 4));
        *offset += 5;
        if (shift + addSpace < 0) return false;
        const quint64 pixelCount = quint64(width) * quint64(height);
        if (!rangeFits(*offset, pixelCount, quint64(data.size()))) return false;
        Glyph glyph;
        glyph.codepoint = codepoint;
        glyph.width = width;
        glyph.height = height;
        glyph.bearingX = bearingX;
        glyph.bearingY = bearingBottom + height;
        glyph.advance = shift + addSpace;
        glyph.pixels.resize(int(pixelCount));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const quint8 value = quint8(data.at(int(*offset) + y * width + x));
                const int indexBits = 8 - alphaBits;
                const int paletteIndex = alphaBits == 0 ? value
                    : (indexBits == 0 ? 0 : value & ((1 << indexBits) - 1));
                if ((alphaBits == 0 && value > paletteSize) ||
                    (alphaBits != 0 && paletteIndex >= paletteSize))
                    return false;
                glyph.pixels[y * width + x] = char(value);
            }
        }
        *offset += pixelCount;
        parsed->width = qMax(parsed->width, glyph.advance);
        maximumWidth = qMax(maximumWidth, quint32(glyph.advance));
        if (!parsed->glyphs.isEmpty() && parsed->glyphs.first().advance != glyph.advance)
            parsed->fixedWidth = false;
        codepoints.insert(codepoint);
        parsed->glyphs.push_back(glyph);
        return true;
    };
    if (!rangeFits(cursor, 2, quint64(data.size())))
        return fail(error, QStringLiteral("Truncated BMF low-character count"));
    const quint16 lowCount = le16(data, int(cursor));
    cursor += 2;
    if (lowCount > 256) return fail(error, QStringLiteral("BMF low-character count exceeds limits"));
    for (quint16 i = 0; i < lowCount; ++i) {
        if (!rangeFits(cursor, 1, quint64(data.size())) ||
            !readGlyph(quint8(data.at(int(cursor++))), &font, &cursor))
            return fail(error, QStringLiteral("Invalid or truncated BMF low-character record"));
    }
    if (version == 0x12) {
        if (cursor == quint64(data.size())) {
            if (font.glyphs.isEmpty())
                return fail(error, QStringLiteral("Incomplete BMF font"));
            font.width = qMax(font.width, int(maximumWidth));
            *out = font;
            return true;
        }
        if (!rangeFits(cursor, 4, quint64(data.size())))
            return fail(error, QStringLiteral("Truncated BMF Unicode count"));
        const quint32 highCount = le32(data, int(cursor));
        cursor += 4;
        if (highCount > 65536) return fail(error, QStringLiteral("BMF Unicode count exceeds limits"));
        for (quint32 i = 0; i < highCount; ++i) {
            if (!rangeFits(cursor, 4, quint64(data.size())))
                return fail(error, QStringLiteral("Truncated BMF Unicode codepoint"));
            const quint32 codepoint = le32(data, int(cursor));
            cursor += 4;
            if (codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                !readGlyph(codepoint, &font, &cursor))
                return fail(error, QStringLiteral("Invalid or truncated BMF Unicode record"));
        }
        if (!rangeFits(cursor, 2, quint64(data.size())))
            return fail(error, QStringLiteral("Truncated BMF kerning count"));
        const quint16 kerningCount = le16(data, int(cursor));
        cursor += 2;
        if (!rangeFits(cursor, quint64(kerningCount) * 10, quint64(data.size())))
            return fail(error, QStringLiteral("Truncated BMF kerning table"));
        for (quint16 i = 0; i < kerningCount; ++i) {
            KerningPair pair;
            pair.left = le32(data, int(cursor));
            pair.right = le32(data, int(cursor + 4));
            pair.adjustment = qint16(le16(data, int(cursor + 8)));
            font.kernings.push_back(pair);
            cursor += 10;
        }
    }
    if (font.glyphs.isEmpty() || cursor != quint64(data.size()))
        return fail(error, QStringLiteral("Incomplete BMF font or unexpected trailing bytes"));
    font.width = qMax(font.width, int(maximumWidth));
    *out = font;
    return true;
}

bool parsePsfUnicode(const QByteArray& data, int offset, BitmapFont* font, bool psf1,
                     QString* error)
{
    int glyphIndex = 0;
    bool sequence = false;
    QByteArray utf8;
    for (int pos = offset; pos < data.size();) {
        if (psf1) {
            if (pos + 2 > data.size()) return fail(error, QStringLiteral("Truncated PSF1 Unicode table"));
            const quint16 value = le16(data, pos);
            pos += 2;
            if (value == 0xffff) {
                if (glyphIndex >= font->glyphs.size()) return fail(error, QStringLiteral("Excess PSF1 Unicode entries"));
                ++glyphIndex;
                sequence = false;
                continue;
            }
            if (value == 0xfffe) {
                if (glyphIndex >= font->glyphs.size()) return fail(error, QStringLiteral("Excess PSF1 Unicode entries"));
                sequence = true;
                continue;
            }
            if (glyphIndex >= font->glyphs.size())
                return fail(error, QStringLiteral("Excess PSF1 Unicode entries"));
            if (value >= 0xd800 && value <= 0xdfff)
                return fail(error, QStringLiteral("Invalid PSF1 Unicode mapping"));
            if (!sequence && (font->glyphs.at(glyphIndex).codepoint == 0xffffffffu ||
                              font->glyphs.at(glyphIndex).codepoint == quint32(glyphIndex)))
                font->glyphs[glyphIndex].codepoint = value;
        } else {
            const quint8 value = quint8(data.at(pos++));
            if (value == 0xff || value == 0xfe) {
                if (glyphIndex >= font->glyphs.size())
                    return fail(error, QStringLiteral("Excess PSF2 Unicode entries"));
                if (!utf8.isEmpty()) {
                    const QString decoded = QString::fromUtf8(utf8.constData(), utf8.size());
                    if (decoded.toUtf8() != utf8 || decoded.isEmpty())
                        return fail(error, QStringLiteral("Invalid PSF2 Unicode mapping"));
                    for (int i = 0; i < decoded.size(); ++i) {
                        if (decoded.at(i).isHighSurrogate()) {
                            if (i + 1 >= decoded.size() || !decoded.at(i + 1).isLowSurrogate())
                                return fail(error, QStringLiteral("Invalid PSF2 Unicode surrogate pair"));
                            ++i;
                        } else if (decoded.at(i).isLowSurrogate()) {
                            return fail(error, QStringLiteral("Unexpected PSF2 Unicode low surrogate"));
                        }
                    }
                    if (glyphIndex >= font->glyphs.size())
                        return fail(error, QStringLiteral("Excess PSF2 Unicode entries"));
                    if (!sequence &&
                        (font->glyphs.at(glyphIndex).codepoint == 0xffffffffu ||
                         font->glyphs.at(glyphIndex).codepoint == quint32(glyphIndex))) {
                        const uint codepoint = decoded.at(0).isHighSurrogate()
                            ? QChar::surrogateToUcs4(decoded.at(0), decoded.at(1))
                            : decoded.at(0).unicode();
                        font->glyphs[glyphIndex].codepoint = codepoint;
                    }
                    utf8.clear();
                }
                if (value == 0xff) {
                    if (glyphIndex >= font->glyphs.size()) return fail(error, QStringLiteral("Excess PSF2 Unicode entries"));
                    ++glyphIndex;
                    sequence = false;
                } else sequence = true;
            } else utf8.append(char(value));
        }
    }
    if (!psf1 && !utf8.isEmpty()) return fail(error, QStringLiteral("Unterminated PSF2 Unicode mapping"));
    if (glyphIndex != font->glyphs.size()) return fail(error, QStringLiteral("Incomplete PSF Unicode table"));
    return true;
}

bool parsePsf(const QByteArray& data, BitmapFont* out, QString* error)
{
    BitmapFont font;
    const bool psf1 = quint8(data.at(0)) == 0x36;
    if (psf1) {
        if (data.size() < 4) return fail(error, QStringLiteral("Truncated PSF1 header"));
        const quint8 mode = quint8(data.at(2));
        const int count = (mode & 1) ? 512 : 256;
        const int height = quint8(data.at(3));
        if ((mode & ~3) != 0 || height == 0 || height > kMaxDimension)
            return fail(error, QStringLiteral("Unsupported PSF1 mode or height"));
        const qint64 bitmapLength = qint64(count) * height;
        if (bitmapLength > data.size() - 4) return fail(error, QStringLiteral("Truncated PSF1 glyph data"));
        font.format = Format::Psf1;
        font.width = 8;
        font.height = height;
        font.ascent = height;
        font.fixedWidth = true;
        for (int i = 0; i < count; ++i) {
            Glyph glyph;
            glyph.codepoint = quint32(i);
            glyph.width = 8;
            glyph.height = height;
            glyph.bearingY = height;
            glyph.advance = 8;
            glyph.bitmap = data.mid(4 + i * height, height);
            font.glyphs.push_back(glyph);
        }
        if (mode & 2) {
            for (Glyph& glyph : font.glyphs) glyph.codepoint = 0xffffffffu;
            if (!parsePsfUnicode(data, int(4 + bitmapLength), &font, true, error)) return false;
        }
        if (!(mode & 2) && int(4 + bitmapLength) != data.size())
            return fail(error, QStringLiteral("Unexpected bytes after PSF1 glyph data"));
    } else {
        if (data.size() < 32 || le32(data, 0) != 0x864ab572u)
            return fail(error, QStringLiteral("Invalid PSF2 header"));
        const quint32 version = le32(data, 4);
        const quint32 headerSize = le32(data, 8);
        const quint32 flags = le32(data, 12);
        const quint32 count = le32(data, 16);
        const quint32 charSize = le32(data, 20);
        const quint32 height = le32(data, 24);
        const quint32 width = le32(data, 28);
        if (version != 0 || headerSize < 32 || headerSize > quint32(data.size()) ||
            (flags & ~1u) != 0 || count == 0 || count > quint32(kMaxGlyphs) ||
            !dimensionOk(int(width), int(height)) || charSize == 0 || charSize > kMaxBitmapBytes)
            return fail(error, QStringLiteral("Unsupported or invalid PSF2 header"));
        const quint32 rowBytes = (width + 7) / 8;
        if (qint64(rowBytes) * height > charSize || qint64(count) * charSize > data.size() - headerSize)
            return fail(error, QStringLiteral("Invalid PSF2 glyph data size"));
        font.format = Format::Psf2;
        font.width = int(width);
        font.height = int(height);
        font.ascent = int(height);
        font.fixedWidth = true;
        for (quint32 i = 0; i < count; ++i) {
            Glyph glyph;
            glyph.codepoint = i;
            glyph.width = int(width);
            glyph.height = int(height);
            glyph.bearingY = int(height);
            glyph.advance = int(width);
            glyph.bitmap = data.mid(int(headerSize + i * charSize), int(rowBytes * height));
            font.glyphs.push_back(glyph);
        }
        const int end = int(headerSize + count * charSize);
        if (flags & 1) {
            for (Glyph& glyph : font.glyphs) glyph.codepoint = 0xffffffffu;
            if (!parsePsfUnicode(data, end, &font, false, error)) return false;
        } else if (end != data.size()) return fail(error, QStringLiteral("Unexpected bytes after PSF2 glyph data"));
    }
    font.name = psf1 ? QStringLiteral("PC Screen Font v1") : QStringLiteral("PC Screen Font v2");
    *out = font;
    return true;
}

struct PcfTable {
    quint32 type = 0;
    quint32 format = 0;
    quint32 offset = 0;
    quint32 size = 0;
};

struct PcfMetric {
    qint16 left = 0;
    qint16 right = 0;
    qint16 advance = 0;
    qint16 ascent = 0;
    qint16 descent = 0;
};

bool parsePcf(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!hasPcfHeader(data) || data.size() < 8)
        return fail(error, QStringLiteral("Invalid PCF header"));
    const quint32 tableCount = le32(data, 4);
    if (tableCount == 0 || tableCount > 64 || 8LL + qint64(tableCount) * 16 > data.size())
        return fail(error, QStringLiteral("Invalid PCF table directory"));
    QVector<PcfTable> tables;
    QVector<QPair<quint32, quint32>> ranges;
    for (quint32 i = 0; i < tableCount; ++i) {
        const int record = 8 + int(i * 16);
        PcfTable table;
        table.type = le32(data, record);
        table.format = le32(data, record + 4);
        table.size = le32(data, record + 8);
        table.offset = le32(data, record + 12);
        if (table.offset < 8 + tableCount * 16 || (table.offset & 3u) ||
            table.offset > quint32(data.size()))
            return fail(error, QStringLiteral("Invalid PCF table bounds"));
        if (table.size > quint32(data.size()) - table.offset) {
            if (table.type != (1u << 8))
                return fail(error, QStringLiteral("Invalid PCF table bounds"));
            table.size = 0;
        }
        for (const PcfTable& prior : tables)
            if (prior.type == table.type &&
                (table.type != (1u << 1) || prior.format == table.format ||
                 std::count_if(tables.cbegin(), tables.cend(), [&table](const PcfTable& item) {
                     return item.type == table.type;
                 }) > 1))
                return fail(error, QStringLiteral("Duplicate PCF table"));
        if (table.size) ranges.push_back(qMakePair(table.offset, table.offset + table.size));
        tables.push_back(table);
    }
    std::sort(ranges.begin(), ranges.end(), [](const QPair<quint32, quint32>& a,
                                               const QPair<quint32, quint32>& b) {
        return a.first < b.first;
    });
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges.at(i).first < ranges.at(i - 1).second)
            return fail(error, QStringLiteral("Overlapping PCF tables"));

    auto findTable = [&tables](quint32 type) -> const PcfTable* {
        for (const PcfTable& table : tables) if (table.type == type) return &table;
        return nullptr;
    };
    // Older PCF writers store DEFAULT in the directory and the effective format in the table.
    auto tableFormat = [](const PcfTable* table, const QByteArray& bytes, quint32* format) {
        if (!table || bytes.size() < 4)
            return false;
        *format = le32(bytes, 0);
        return table->format == 0 || table->format == *format;
    };
    const PcfTable* metricsTable = findTable(1u << 2);
    const PcfTable* bitmapsTable = findTable(1u << 3);
    const PcfTable* encodingTable = findTable(1u << 5);
    if (!metricsTable || !bitmapsTable || !encodingTable)
        return fail(error, QStringLiteral("PCF metrics, bitmaps, or encoding table is missing"));

    const QByteArray metricsData = data.mid(int(metricsTable->offset), int(metricsTable->size));
    quint32 metricsFormat = 0;
    if (!tableFormat(metricsTable, metricsData, &metricsFormat))
        return fail(error, QStringLiteral("PCF metrics format mismatch"));
    const bool metricsBigEndian = (metricsFormat & 4u) != 0;
    if ((metricsFormat & 0xfffffec0u) != 0)
        return fail(error, QStringLiteral("Unsupported PCF metrics format"));
    const bool compressedMetrics = (metricsFormat & 0x100u) != 0;
    if (metricsData.size() < (compressedMetrics ? 6 : 8))
        return fail(error, QStringLiteral("Truncated PCF metrics table"));
    const quint32 metricCount = compressedMetrics ? read16(metricsData, 4, metricsBigEndian)
                                                  : read32(metricsData, 4, metricsBigEndian);
    const int metricSize = compressedMetrics ? 5 : 12;
    const int metricsStart = compressedMetrics ? 6 : 8;
    if (metricCount == 0 || metricCount > quint32(kMaxGlyphs) ||
        quint64(metricsStart) + quint64(metricCount) * metricSize > quint64(metricsData.size()))
        return fail(error, QStringLiteral("Invalid PCF metric array"));
    QVector<PcfMetric> metrics;
    metrics.reserve(int(metricCount));
    for (quint32 i = 0; i < metricCount; ++i) {
        const int offset = metricsStart + int(i * metricSize);
        PcfMetric metric;
        if (compressedMetrics) {
            metric.left = qint16(quint8(metricsData.at(offset))) - 0x80;
            metric.right = qint16(quint8(metricsData.at(offset + 1))) - 0x80;
            metric.advance = qint16(quint8(metricsData.at(offset + 2))) - 0x80;
            metric.ascent = qint16(quint8(metricsData.at(offset + 3))) - 0x80;
            metric.descent = qint16(quint8(metricsData.at(offset + 4))) - 0x80;
        } else {
            metric.left = qint16(read16(metricsData, offset, metricsBigEndian));
            metric.right = qint16(read16(metricsData, offset + 2, metricsBigEndian));
            metric.advance = qint16(read16(metricsData, offset + 4, metricsBigEndian));
            metric.ascent = qint16(read16(metricsData, offset + 6, metricsBigEndian));
            metric.descent = qint16(read16(metricsData, offset + 8, metricsBigEndian));
        }
        const int width = int(metric.right) - int(metric.left);
        const int height = int(metric.ascent) + int(metric.descent);
        if (width < 0 || width > kMaxDimension || height < 0 || height > kMaxDimension ||
            qint64(width) * height > kMaxBitmapBytes * 8LL)
            return fail(error, QStringLiteral("Invalid PCF glyph metrics"));
        metrics.push_back(metric);
    }

    const QByteArray bitmapData = data.mid(int(bitmapsTable->offset), int(bitmapsTable->size));
    quint32 bitmapFormat = 0;
    if (!tableFormat(bitmapsTable, bitmapData, &bitmapFormat))
        return fail(error, QStringLiteral("PCF bitmap format mismatch"));
    const bool bitmapBigEndian = (bitmapFormat & 4u) != 0;
    if (bitmapData.size() < 8 || (bitmapFormat & 0xffffff00u) != 0 ||
        ((bitmapFormat >> 4) & 3u) > 3)
        return fail(error, QStringLiteral("Unsupported PCF bitmap format"));
    const quint32 bitmapGlyphCount = read32(bitmapData, 4, bitmapBigEndian);
    if (bitmapGlyphCount != metricCount || 8LL + qint64(bitmapGlyphCount) * 4 + 16 > bitmapData.size())
        return fail(error, QStringLiteral("Invalid PCF bitmap glyph count"));
    QVector<quint32> bitmapOffsets;
    bitmapOffsets.reserve(int(bitmapGlyphCount));
    int cursor = 8;
    for (quint32 i = 0; i < bitmapGlyphCount; ++i, cursor += 4)
        bitmapOffsets.push_back(read32(bitmapData, cursor, bitmapBigEndian));
    quint32 bitmapSizes[4] = {};
    for (int i = 0; i < 4; ++i, cursor += 4)
        bitmapSizes[i] = read32(bitmapData, cursor, bitmapBigEndian);
    const quint32 glyphPad = 1u << (bitmapFormat & 3u);
    const quint32 scanBytes = 1u << ((bitmapFormat >> 4) & 3u);
    const quint32 bitmapLength = bitmapSizes[bitmapFormat & 3u];
    if (bitmapLength > quint32(bitmapData.size() - cursor))
        return fail(error, QStringLiteral("Truncated PCF bitmap array"));
    const int rawBitmapStart = cursor;
    const quint32 scanBits = scanBytes * 8;
    quint64 decodedBitmapBytes = 0;

    const QByteArray encodingData = data.mid(int(encodingTable->offset), int(encodingTable->size));
    quint32 encodingFormat = 0;
    if (!tableFormat(encodingTable, encodingData, &encodingFormat))
        return fail(error, QStringLiteral("PCF encoding format mismatch"));
    const bool encodingBigEndian = (encodingFormat & 4u) != 0;
    if (encodingData.size() < 14 || (encodingFormat & 0xffffff00u) != 0)
        return fail(error, QStringLiteral("Unsupported PCF encoding format"));
    const quint16 minChar = read16(encodingData, 4, encodingBigEndian);
    const quint16 maxChar = read16(encodingData, 6, encodingBigEndian);
    const quint16 minByte = read16(encodingData, 8, encodingBigEndian);
    const quint16 maxByte = read16(encodingData, 10, encodingBigEndian);
    if (minChar > maxChar || minByte > maxByte)
        return fail(error, QStringLiteral("Invalid PCF encoding range"));
    const quint64 encodingCount = quint64(maxChar - minChar + 1) * (maxByte - minByte + 1);
    if (encodingCount > 65536 || 14 + encodingCount * 2 > quint64(encodingData.size()))
        return fail(error, QStringLiteral("Invalid PCF encoding array"));

    QString fontName = QStringLiteral("PCF bitmap font");
    const PcfTable* propertiesTable = findTable(1u << 0);
    if (propertiesTable) {
        const QByteArray properties = data.mid(int(propertiesTable->offset), int(propertiesTable->size));
        quint32 propertiesFormat = 0;
        if (properties.size() < 12 || !tableFormat(propertiesTable, properties, &propertiesFormat))
            return fail(error, QStringLiteral("Invalid PCF properties table"));
        const bool propertiesBigEndian = (propertiesFormat & 4u) != 0;
        const quint32 propertyCount = read32(properties, 4, propertiesBigEndian);
        const quint64 recordsEnd = 8 + quint64(propertyCount) * 9;
        const quint64 stringsSizeOffset = (recordsEnd + 3u) & ~quint64(3u);
        if (propertyCount > 4096 || stringsSizeOffset + 4 > quint64(properties.size()))
            return fail(error, QStringLiteral("Invalid PCF property array"));
        const quint32 stringsSize = read32(properties, int(stringsSizeOffset), propertiesBigEndian);
        const quint64 stringsStart = stringsSizeOffset + 4;
        if (stringsSize > quint64(properties.size()) - stringsStart)
            return fail(error, QStringLiteral("Truncated PCF property strings"));
        const QByteArray strings = properties.mid(int(stringsStart), int(stringsSize));
        QString propertyFamily;
        QString propertyFont;
        auto readString = [&strings](quint32 offset, QString* value) {
            if (offset >= quint32(strings.size())) return false;
            const int end = strings.indexOf('\0', int(offset));
            if (end < 0) return false;
            *value = QString::fromLatin1(strings.constData() + offset, end - int(offset));
            return true;
        };
        for (quint32 i = 0; i < propertyCount; ++i) {
            const int record = 8 + int(i * 9);
            const quint32 nameOffset = read32(properties, record, propertiesBigEndian);
            const bool stringProperty = quint8(properties.at(record + 4)) != 0;
            const quint32 valueOffset = read32(properties, record + 5, propertiesBigEndian);
            QString propertyName;
            if (!readString(nameOffset, &propertyName))
                return fail(error, QStringLiteral("Invalid PCF property name"));
            if (stringProperty && (propertyName == QLatin1String("FAMILY_NAME") ||
                                   propertyName == QLatin1String("FONT"))) {
                QString propertyValue;
                if (!readString(valueOffset, &propertyValue))
                    return fail(error, QStringLiteral("Invalid PCF font name property"));
                if (propertyName == QLatin1String("FAMILY_NAME")) propertyFamily = propertyValue;
                else if (!propertyValue.isEmpty()) propertyFont = propertyValue;
            }
        }
        if (!propertyFamily.isEmpty()) fontName = propertyFamily;
        else if (!propertyFont.isEmpty()) fontName = propertyFont;
    }

    BitmapFont font;
    font.format = Format::Pcf;
    font.name = fontName;
    font.glyphs.reserve(int(metricCount));
    for (quint32 i = 0; i < metricCount; ++i) {
        const PcfMetric& metric = metrics.at(int(i));
        Glyph glyph;
        glyph.width = int(metric.right) - int(metric.left);
        glyph.height = int(metric.ascent) + int(metric.descent);
        glyph.bearingX = metric.left;
        glyph.bearingY = metric.ascent;
        glyph.advance = metric.advance;
        const quint32 rowBytes = (quint32(glyph.width) + 7u) / 8u;
        const quint32 rowStride = (rowBytes + glyphPad - 1u) & ~(glyphPad - 1u);
        const quint64 required = quint64(rowStride) * quint32(glyph.height);
        const quint32 offset = bitmapOffsets.at(int(i));
        const quint32 end = i + 1 < metricCount ? bitmapOffsets.at(int(i + 1)) : bitmapLength;
        if (offset > end || end > bitmapLength || required > end - offset ||
            required > quint64(kMaxBitmapBytes))
            return fail(error, QStringLiteral("PCF glyph bitmap exceeds its declared bounds"));
        decodedBitmapBytes += quint64(rowBytes) * quint32(glyph.height);
        if (decodedBitmapBytes > quint64(kMaxBitmapBytes))
            return fail(error, QStringLiteral("PCF decoded bitmap limit exceeded"));
        glyph.bitmap = QByteArray(int(rowBytes * quint32(glyph.height)), '\0');
        for (int y = 0; y < glyph.height; ++y) {
            for (int x = 0; x < glyph.width; ++x) {
                const quint64 bitIndex = quint64(y) * rowStride * 8u + quint32(x);
                const quint32 unitOffset = quint32((bitIndex / scanBits) * scanBytes);
                if (unitOffset + scanBytes > end - offset)
                    return fail(error, QStringLiteral("PCF scan unit exceeds glyph bitmap"));
                quint32 unit = 0;
                for (quint32 byte = 0; byte < scanBytes; ++byte) {
                    const quint32 raw = quint8(bitmapData.at(int(rawBitmapStart + offset + unitOffset + byte)));
                    const quint32 shift = bitmapBigEndian ? (scanBytes - byte - 1u) * 8u : byte * 8u;
                    unit |= raw << shift;
                }
                const quint32 bitInUnit = quint32(bitIndex % scanBits);
                const quint32 bitPosition = (bitmapFormat & 8u) ? scanBits - bitInUnit - 1u : bitInUnit;
                if ((unit >> bitPosition) & 1u) {
                    const int normalizedOffset = y * int(rowBytes) + x / 8;
                    glyph.bitmap[normalizedOffset] = char(quint8(glyph.bitmap.at(normalizedOffset)) |
                                                         (0x80u >> (x & 7)));
                }
            }
        }
        font.ascent = qMax(font.ascent, int(metric.ascent));
        font.descent = qMax(font.descent, int(metric.descent));
        font.width = qMax(font.width, glyph.width);
        font.height = qMax(font.height, glyph.height);
        font.glyphs.push_back(glyph);
    }
    font.fixedWidth = true;
    const qint16 firstAdvance = metrics.first().advance;
    for (const PcfMetric& metric : metrics)
        if (metric.advance != firstAdvance) font.fixedWidth = false;

    for (quint32 byte1 = minByte; byte1 <= maxByte; ++byte1) {
        for (quint32 byte2 = minChar; byte2 <= maxChar; ++byte2) {
            const quint32 index = (byte1 - minByte) * (maxChar - minChar + 1) + byte2 - minChar;
            const quint16 glyphIndex = read16(encodingData, 14 + int(index * 2), encodingBigEndian);
            if (glyphIndex == 0xffffu) continue;
            if (glyphIndex >= metricCount)
                return fail(error, QStringLiteral("PCF encoding points outside the glyph array"));
            Glyph& glyph = font.glyphs[int(glyphIndex)];
            glyph.codepoint = minByte == 0 && maxByte == 0
                ? byte2 : (byte1 << 8) | byte2;
        }
    }
    *out = font;
    return true;
}

bool looksLikeFzxHeader(const QByteArray& data)
{
    if (data.size() < 8) return false;
    const int height = quint8(data.at(0));
    const int lastCharacter = quint8(data.at(2));
    if (height == 0 || height > 192 || lastCharacter < 32) return false;
    const int entryCount = lastCharacter - 31;
    const qint64 tableEnd = 3LL + entryCount * 3LL + 2;
    if (tableEnd > data.size()) return false;
    const quint16 declaredSize = le16(data, int(tableEnd - 2));
    if (declaredSize < 2) return false;
    const quint32 dataEnd = quint32(tableEnd + declaredSize - 2);
    quint32 previousOffset = quint32(tableEnd);
    for (int i = 0; i < entryCount; ++i) {
        const int entryOffset = 3 + i * 3;
        const quint16 encodedOffset = le16(data, entryOffset) & 0x3fffu;
        const quint32 absoluteOffset = quint32(entryOffset) + encodedOffset;
        if (absoluteOffset < previousOffset || absoluteOffset > dataEnd) return false;
        previousOffset = absoluteOffset;
    }
    return true;
}

bool parseFzx(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!looksLikeFzxHeader(data)) return fail(error, QStringLiteral("Invalid FZX header or offset table"));
    const int lineHeight = quint8(data.at(0));
    const int tracking = qint8(data.at(1));
    const int lastCharacter = quint8(data.at(2));
    const int count = lastCharacter - 31;
    const int tableEnd = 3 + count * 3 + 2;
    const int declaredSize = le16(data, tableEnd - 2);
    const quint32 dataEnd = quint32(tableEnd + declaredSize - 2);
    if (dataEnd > quint32(data.size()))
        return fail(error, QStringLiteral("FZX declared length exceeds the input"));

    QVector<quint32> offsets;
    offsets.reserve(count + 1);
    QVector<int> kerns;
    kerns.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int entryOffset = 3 + i * 3;
        const quint16 encoded = le16(data, entryOffset);
        const quint32 absoluteOffset = quint32(entryOffset) + (encoded & 0x3fffu);
        if (absoluteOffset < quint32(tableEnd) ||
            (!offsets.isEmpty() && absoluteOffset < offsets.constLast()) ||
            absoluteOffset > dataEnd)
            return fail(error, QStringLiteral("Invalid FZX glyph offset order"));
        offsets.push_back(absoluteOffset);
        kerns.push_back((encoded >> 14) & 3);
    }
    offsets.push_back(dataEnd);

    BitmapFont font;
    font.format = Format::Fzx;
    font.name = QStringLiteral("ZX Spectrum FZX font");
    font.height = lineHeight;
    font.ascent = lineHeight;
    font.fixedWidth = false;
    for (int i = 0; i < count; ++i) {
        const int entryOffset = 3 + i * 3;
        const quint8 metrics = quint8(data.at(entryOffset + 2));
        const int shift = metrics >> 4;
        const int width = (metrics & 0x0f) + 1;
        const quint32 start = offsets.at(i);
        const quint32 end = offsets.at(i + 1);
        const int rowBytes = width > 8 ? 2 : 1;
        const quint32 available = end - start;
        if (available % quint32(rowBytes) != 0)
            return fail(error, QStringLiteral("Invalid FZX glyph dimensions or bitmap span"));
        const int glyphHeight = int(available / quint32(rowBytes));
        if (glyphHeight > kMaxDimension)
            return fail(error, QStringLiteral("FZX glyph height exceeds limits"));

        Glyph glyph;
        glyph.codepoint = quint32(i + 32);
        glyph.width = width;
        glyph.height = glyphHeight;
        glyph.bearingY = lineHeight - shift;
        glyph.advance = qMax(0, width + tracking - kerns.at(i));
        glyph.bitmap = QByteArray((width + 7) / 8 * glyphHeight, '\0');
        for (quint32 row = 0; row < available / quint32(rowBytes); ++row) {
            const int source = int(start + row * quint32(rowBytes));
            for (int x = 0; x < width; ++x) {
                const quint8 bits = quint8(data.at(source + x / 8));
                if (bits & (0x80u >> (x & 7))) {
                    const int target = int(row) * ((width + 7) / 8) + x / 8;
                    glyph.bitmap[target] = char(quint8(glyph.bitmap.at(target)) |
                                                 (0x80u >> (x & 7)));
                }
            }
        }
        font.width = qMax(font.width, width);
        font.glyphs.push_back(glyph);
    }
    *out = font;
    return true;
}

class PkRasterReader final
{
public:
    PkRasterReader(const QByteArray& data, int begin, int end)
        : data_(data), bitPosition_(begin * 8), endBit_(end * 8) {}

    bool decode(int width, int height, int dynF, bool blackFirst, Glyph* glyph)
    {
        if (!glyph || width < 0 || height < 0 || width > kMaxDimension ||
            height > kMaxDimension || qint64(width) * height > 16 * 1024 * 1024)
            return false;
        const int stride = (width + 7) / 8;
        glyph->bitmap = QByteArray(stride * height, '\0');
        if (width == 0 || height == 0) return true;
        if (dynF == 14) {
            for (int pixel = 0; pixel < width * height; ++pixel) {
                int bit = 0;
                if (!nextBit(&bit)) return false;
                if (bit) {
                    const int offset = (pixel / width) * stride + (pixel % width) / 8;
                    glyph->bitmap[offset] = char(quint8(glyph->bitmap.at(offset)) |
                                                 (0x80u >> (pixel % width & 7)));
                }
            }
            return true;
        }
        bool black = blackFirst;
        int pixel = 0;
        int repeatedRows = 0;
        const int total = width * height;
        while (pixel < total) {
            int repeat = 0;
            int run = 0;
            if (!packedNumber(dynF, &repeat, &run) || run <= 0 || run > total - pixel)
                return false;
            if (repeat != 0) {
                if (repeatedRows != 0) return false;
                repeatedRows = repeat;
            }
            for (int i = 0; i < run; ++i) {
                if (black) {
                    const int y = pixel / width;
                    const int x = pixel % width;
                    const int offset = y * stride + x / 8;
                    glyph->bitmap[offset] = char(quint8(glyph->bitmap.at(offset)) |
                                                 (0x80u >> (x & 7)));
                }
                ++pixel;
                if (pixel % width == 0 && repeatedRows != 0) {
                    const int row = pixel / width - 1;
                    if (repeatedRows > height - row - 1) return false;
                    for (int repeatRow = 1; repeatRow <= repeatedRows; ++repeatRow)
                        std::memcpy(glyph->bitmap.data() + (row + repeatRow) * stride,
                                    glyph->bitmap.constData() + row * stride, size_t(stride));
                    pixel += repeatedRows * width;
                    repeatedRows = 0;
                }
            }
            black = !black;
        }
        return true;
    }

    bool finish()
    {
        while ((bitPosition_ & 7) != 0) {
            int bit = 0;
            if (!nextBit(&bit) || bit != 0) return false;
        }
        return bitPosition_ == endBit_;
    }

private:
    bool nextBit(int* value)
    {
        if (!value || bitPosition_ >= endBit_) return false;
        const int byte = bitPosition_ / 8;
        *value = (quint8(data_.at(byte)) >> (7 - (bitPosition_ & 7))) & 1;
        ++bitPosition_;
        return true;
    }

    bool nextNibble(int* value)
    {
        if (!value || bitPosition_ + 4 > endBit_) return false;
        const int byte = bitPosition_ / 8;
        const int shift = (bitPosition_ & 4) ? 0 : 4;
        *value = (quint8(data_.at(byte)) >> shift) & 0x0f;
        bitPosition_ += 4;
        return true;
    }

    bool packedValue(int dynF, int* value)
    {
        int first = 0;
        if (!nextNibble(&first)) return false;
        if (first == 0) {
            int zeros = 0;
            do {
                if (!nextNibble(&first) || ++zeros > 8) return false;
            } while (first == 0);
            quint32 result = quint32(first);
            for (int i = 0; i < zeros; ++i) {
                int nibble = 0;
                if (!nextNibble(&nibble)) return false;
                result = (result << 4) | quint32(nibble);
            }
            const qint64 decoded = qint64(result) - 15 + qint64(13 - dynF) * 16 + dynF;
            if (decoded <= 0 || decoded > INT_MAX) return false;
            *value = int(decoded);
            return true;
        }
        if (first <= dynF) {
            *value = first;
            return true;
        }
        if (first < 14) {
            int next = 0;
            if (!nextNibble(&next)) return false;
            *value = (first - dynF - 1) * 16 + next + dynF + 1;
            return true;
        }
        return false;
    }

    bool packedNumber(int dynF, int* repeat, int* value)
    {
        int nibble = 0;
        if (!nextNibble(&nibble)) return false;
        if (nibble < 14) {
            if (nibble == 0) {
                bitPosition_ -= 4;
                return packedValue(dynF, value);
            }
            bitPosition_ -= 4;
            return packedValue(dynF, value);
        }
        if (*repeat != 0) return false;
        *repeat = 1;
        if (nibble == 14 && !packedValue(dynF, repeat)) return false;
        return packedNumber(dynF, repeat, value);
    }

    const QByteArray& data_;
    int bitPosition_;
    int endBit_;
};

bool readBe(const QByteArray& data, int* offset, int bytes, quint32* value)
{
    if (!offset || !value || bytes < 1 || bytes > 4 || *offset < 0 ||
        *offset > data.size() - bytes) return false;
    quint32 result = 0;
    for (int i = 0; i < bytes; ++i)
        result = (result << 8) | quint8(data.at((*offset)++));
    *value = result;
    return true;
}

qint32 signExtend(quint32 value, int bytes)
{
    const int bits = bytes * 8;
    if (bits == 32) return qint32(value);
    const quint32 sign = quint32(1) << (bits - 1);
    return qint32((value ^ sign) - sign);
}

bool parsePk(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!hasPkHeader(data)) return fail(error, QStringLiteral("Invalid PK preamble"));
    int pos = 0;
    quint32 value = 0;
    readBe(data, &pos, 1, &value);
    readBe(data, &pos, 1, &value);
    quint32 commentLength = 0;
    if (!readBe(data, &pos, 1, &commentLength) || commentLength > quint32(data.size() - pos))
        return fail(error, QStringLiteral("Truncated PK preamble comment"));
    const QByteArray comment = data.mid(pos, int(commentLength));
    pos += int(commentLength);
    for (int i = 0; i < 4; ++i)
        if (!readBe(data, &pos, 4, &value)) return fail(error, QStringLiteral("Truncated PK preamble"));

    BitmapFont font;
    font.format = Format::Pk;
    font.name = comment.isEmpty() ? QStringLiteral("TeX PK font") : QString::fromLatin1(comment);
    font.fixedWidth = true;
    bool sawPost = false;
    while (pos < data.size()) {
        quint32 command = 0;
        if (!readBe(data, &pos, 1, &command)) break;
        if (command == 245) {
            sawPost = true;
            while (pos < data.size()) {
                if (!readBe(data, &pos, 1, &value) || value != 246)
                    return fail(error, QStringLiteral("Invalid PK postamble padding"));
            }
            break;
        }
        if (command == 246) continue;
        if (command >= 240 && command <= 243) {
            quint32 length = 0;
            if (!readBe(data, &pos, int(command - 239), &length) || length > quint32(data.size() - pos))
                return fail(error, QStringLiteral("Invalid PK special length"));
            pos += int(length);
            continue;
        }
        if (command == 244) {
            if (!readBe(data, &pos, 4, &value))
                return fail(error, QStringLiteral("Truncated PK numeric special"));
            continue;
        }
        if (command >= 247 || font.glyphs.size() >= kMaxGlyphs)
            return fail(error, QStringLiteral("Unexpected PK command or too many glyphs"));

        const int flag = int(command);
        const int dynF = flag >> 4;
        const int form = flag & 7;
        quint32 packetLength = 0;
        quint32 code = 0;
        quint32 tfm = 0;
        quint32 advance = 0;
        int advancePixels = 0;
        quint32 width = 0;
        quint32 height = 0;
        quint32 hoff = 0;
        quint32 voff = 0;
        int packetEnd = 0;
        int signedBytes = 0;
        if (form <= 3) {
            quint32 lowLength = 0;
            if (!readBe(data, &pos, 1, &lowLength) || !readBe(data, &pos, 1, &code))
                return fail(error, QStringLiteral("Truncated short PK packet"));
            packetLength = quint32(form) * 256 + lowLength;
            packetEnd = pos + int(packetLength);
            signedBytes = 1;
            if (!readBe(data, &pos, 3, &tfm) || !readBe(data, &pos, 1, &advance) ||
                !readBe(data, &pos, 1, &width) || !readBe(data, &pos, 1, &height) ||
                !readBe(data, &pos, 1, &hoff) || !readBe(data, &pos, 1, &voff))
                return fail(error, QStringLiteral("Truncated short PK glyph header"));
            advancePixels = int(advance);
        } else if (form <= 6) {
            quint32 lowLength = 0;
            if (!readBe(data, &pos, 2, &lowLength) || !readBe(data, &pos, 1, &code))
                return fail(error, QStringLiteral("Truncated extended PK packet"));
            packetLength = quint32(form - 4) * 65536 + lowLength;
            packetEnd = pos + int(packetLength);
            signedBytes = 2;
            if (!readBe(data, &pos, 3, &tfm) || !readBe(data, &pos, 2, &advance) ||
                !readBe(data, &pos, 2, &width) || !readBe(data, &pos, 2, &height) ||
                !readBe(data, &pos, 2, &hoff) || !readBe(data, &pos, 2, &voff))
                return fail(error, QStringLiteral("Truncated extended PK glyph header"));
            advancePixels = int(advance);
        } else {
            if (!readBe(data, &pos, 4, &packetLength) || !readBe(data, &pos, 4, &code))
                return fail(error, QStringLiteral("Truncated long PK packet"));
            if (packetLength > quint32(data.size() - pos))
                return fail(error, QStringLiteral("PK packet exceeds the input"));
            packetEnd = pos + int(packetLength);
            if (!readBe(data, &pos, 4, &tfm) || !readBe(data, &pos, 4, &advance) ||
                !readBe(data, &pos, 4, &value) || !readBe(data, &pos, 4, &width) ||
                !readBe(data, &pos, 4, &height) || !readBe(data, &pos, 4, &hoff) ||
                !readBe(data, &pos, 4, &voff))
                return fail(error, QStringLiteral("Truncated long PK glyph header"));
            advancePixels = int((qint64(qint32(advance)) + 32768) >> 16);
            signedBytes = 4;
        }
        Q_UNUSED(tfm);
        if (packetEnd < pos || packetEnd > data.size() || width > kMaxDimension ||
            height > kMaxDimension || qint64(width) * height > 16 * 1024 * 1024)
            return fail(error, QStringLiteral("Invalid PK packet bounds or glyph dimensions"));

        Glyph glyph;
        glyph.codepoint = code;
        glyph.width = int(width);
        glyph.height = int(height);
        glyph.advance = advancePixels;
        glyph.bearingX = -signExtend(hoff, signedBytes);
        glyph.bearingY = signExtend(voff, signedBytes);
        PkRasterReader raster(data, pos, packetEnd);
        if (!raster.decode(glyph.width, glyph.height, dynF, (flag & 8) != 0, &glyph) ||
            !raster.finish())
            return fail(error, QStringLiteral("Invalid or truncated PK glyph raster"));
        pos = packetEnd;
        font.width = qMax(font.width, glyph.width);
        font.height = qMax(font.height, glyph.height);
        font.glyphs.push_back(glyph);
    }
    if (!sawPost || font.glyphs.isEmpty())
        return fail(error, QStringLiteral("PK font has no complete glyph data or postamble"));
    for (int i = 1; i < font.glyphs.size(); ++i)
        if (font.glyphs.at(i).advance != font.glyphs.first().advance) font.fixedWidth = false;
    font.ascent = font.height;
    *out = font;
    return true;
}

bool parseMacNfnt(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (data.size() < 26) return fail(error, QStringLiteral("Truncated Mac FONT/NFNT header"));
    const quint16 fontType = read16(data, 0, true);
    const int first = read16(data, 2, true);
    const int last = read16(data, 4, true);
    const int kernMax = qint16(read16(data, 8, true));
    const int rectWidth = read16(data, 12, true);
    const int rectHeight = read16(data, 14, true);
    const quint16 owTLoc = read16(data, 16, true);
    const int ascent = read16(data, 18, true);
    const int descent = read16(data, 20, true);
    const int rowWords = read16(data, 24, true);
    if ((fontType != 0x9000 && fontType != 0xb000) || first > last ||
        rectWidth <= 0 || rectHeight <= 0 || !dimensionOk(rectWidth, rectHeight) ||
        ascent > kMaxDimension || descent > kMaxDimension || rowWords <= 0 ||
        rowWords * 16 < rectWidth)
        return fail(error, QStringLiteral("Invalid Mac FONT/NFNT metrics"));

    const quint64 glyphCount = quint64(last - first) + 1;
    const quint64 tableEntries = glyphCount + 2;
    const quint64 bitmapBytes = quint64(rowWords) * 2 * quint64(rectHeight);
    const quint64 bitmapOffset = 26;
    const quint64 locationOffset = bitmapOffset + bitmapBytes;
    const quint64 locationBytes = tableEntries * 2;
    const qint16 nDescent = qint16(read16(data, 10, true));
    quint64 widthOffsetWords = owTLoc;
    if (nDescent > 0) widthOffsetWords |= quint64(quint16(nDescent)) << 16;
    const quint64 widthOffset = 16 + widthOffsetWords * 2;
    if (!rangeFits(bitmapOffset, bitmapBytes, quint64(data.size())) ||
        !rangeFits(locationOffset, locationBytes, quint64(data.size())) ||
        widthOffset < locationOffset + locationBytes ||
        !rangeFits(widthOffset, tableEntries * 2, quint64(data.size())))
        return fail(error, QStringLiteral("Invalid Mac FONT/NFNT table offsets"));

    BitmapFont font;
    font.format = Format::MacNfnt;
    font.name = QStringLiteral("Macintosh bitmap font");
    font.width = rectWidth;
    font.height = rectHeight;
    font.ascent = ascent;
    font.descent = descent;
    font.fixedWidth = true;
    quint16 previousLocation = 0;
    for (quint64 i = 0; i < tableEntries; ++i) {
        const quint16 location = read16(data, int(locationOffset + i * 2), true);
        if (location < previousLocation || location > quint64(rowWords) * 16)
            return fail(error, QStringLiteral("Invalid Mac FONT/NFNT glyph location"));
        previousLocation = location;
    }
    for (quint64 i = 0; i < glyphCount; ++i) {
        const quint16 packed = read16(data, int(widthOffset + i * 2), true);
        if (packed == 0xffff) continue;
        const quint16 start = read16(data, int(locationOffset + i * 2), true);
        const quint16 end = read16(data, int(locationOffset + (i + 1) * 2), true);
        const int width = int(end - start);
        if (width > rectWidth) return fail(error, QStringLiteral("Mac FONT/NFNT glyph exceeds font bounds"));
        Glyph glyph;
        glyph.codepoint = quint32(first + int(i));
        glyph.width = width;
        glyph.height = rectHeight;
        glyph.bearingX = qint8(packed >> 8) + kernMax;
        glyph.advance = packed & 0xff;
        glyph.bearingY = ascent;
        glyph.bitmap = QByteArray((width + 7) / 8 * rectHeight, '\0');
        const int stride = (width + 7) / 8;
        for (int y = 0; y < rectHeight; ++y) {
            for (int x = 0; x < width; ++x) {
                const int sourceBit = int(start) + x;
                const quint8 bits = quint8(data.at(int(bitmapOffset) + y * rowWords * 2 + sourceBit / 8));
                if (bits & (0x80u >> (sourceBit & 7))) {
                    const int target = y * stride + x / 8;
                    glyph.bitmap[target] = char(quint8(glyph.bitmap.at(target)) | (0x80u >> (x & 7)));
                }
            }
        }
        if (!font.glyphs.isEmpty() && font.glyphs.first().advance != glyph.advance)
            font.fixedWidth = false;
        font.glyphs.push_back(glyph);
    }
    if (font.glyphs.isEmpty()) return fail(error, QStringLiteral("Mac FONT/NFNT has no glyphs"));
    *out = font;
    return true;
}

bool parseVfont(const QByteArray& data, BitmapFont* out, QString* error)
{
    constexpr int headerSize = 10;
    constexpr int glyphCount = 256;
    constexpr int dispatchSize = 10;
    constexpr int bitmapOffset = headerSize + glyphCount * dispatchSize;
    if (data.size() < bitmapOffset || !hasVfontHeader(data))
        return fail(error, QStringLiteral("Invalid Sun vfont header"));
    const bool bigEndian = quint8(data.at(0)) == 0x01;
    const int declaredBitmapBytes = read16(data, 2, bigEndian);
    if (declaredBitmapBytes <= 0 || !rangeFits(bitmapOffset, quint64(declaredBitmapBytes),
                                               quint64(data.size())))
        return fail(error, QStringLiteral("Invalid Sun vfont bitmap extent"));

    BitmapFont font;
    font.format = Format::Vfont;
    font.name = QStringLiteral("Sun vfont");
    int maxWidth = 0;
    int maxHeight = 0;
    int maxAscent = 0;
    int maxDescent = 0;
    for (int code = 0; code < glyphCount; ++code) {
        const int record = headerSize + code * dispatchSize;
        const quint16 address = read16(data, record, bigEndian);
        const int byteCount = qint16(read16(data, record + 2, bigEndian));
        const int up = qint8(data.at(record + 4));
        const int down = qint8(data.at(record + 5));
        const int left = qint8(data.at(record + 6));
        const int right = qint8(data.at(record + 7));
        const int advance = qint16(read16(data, record + 8, bigEndian));
        if (byteCount < 0 || quint32(address) + quint32(byteCount) >
                quint32(declaredBitmapBytes) || up + down < 0 ||
            up + down > kMaxDimension || left + right < 0 ||
            left + right > kMaxDimension ||
            advance < -kMaxDimension || advance > kMaxDimension)
            return fail(error, QStringLiteral("Invalid Sun vfont glyph metrics"));
        const int width = left + right;
        const int height = up + down;
        if (byteCount == 0) {
            if (width != 0 || height != 0)
                return fail(error, QStringLiteral("Sun vfont empty glyph has bitmap metrics"));
            if (advance == 0)
                continue;
            Glyph glyph;
            glyph.codepoint = quint32(code);
            glyph.advance = advance;
            font.glyphs.push_back(glyph);
            continue;
        }
        if (width == 0 || height == 0)
            return fail(error, QStringLiteral("Invalid Sun vfont glyph bitmap size"));
        const int sourceStride = (width + 7) / 8;
        const int targetStride = sourceStride;
        if (byteCount != sourceStride * height || !dimensionOk(width, height))
            return fail(error, QStringLiteral("Invalid Sun vfont glyph scanline"));
        Glyph glyph;
        glyph.codepoint = quint32(code);
        glyph.width = width;
        glyph.height = height;
        glyph.bearingX = -left;
        glyph.bearingY = up;
        glyph.advance = advance;
        glyph.bitmap = QByteArray(targetStride * height, '\0');
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const quint8 source = quint8(data.at(bitmapOffset + address +
                    y * sourceStride + x / 8));
                const quint8 mask = quint8(0x80u >> (x & 7));
                if (source & mask) {
                    const int target = y * targetStride + x / 8;
                    glyph.bitmap[target] = char(quint8(glyph.bitmap.at(target)) |
                                                (0x80u >> (x & 7)));
                }
            }
        }
        font.glyphs.push_back(glyph);
        maxWidth = qMax(maxWidth, width);
        maxHeight = qMax(maxHeight, height);
        maxAscent = qMax(maxAscent, up);
        maxDescent = qMax(maxDescent, down);
    }
    if (font.glyphs.isEmpty())
        return fail(error, QStringLiteral("Sun vfont contains no glyphs"));
    font.width = maxWidth;
    font.height = maxHeight;
    font.ascent = maxAscent;
    font.descent = maxDescent;
    font.fixedWidth = true;
    for (const Glyph& glyph : font.glyphs) {
        if (glyph.advance != font.glyphs.first().advance) {
            font.fixedWidth = false;
            break;
        }
    }
    *out = font;
    return true;
}

} // namespace

bool hasBdfHeader(const QByteArray& data)
{
    return data.size() >= 13 && data.startsWith("STARTFONT ") &&
           data.at(10) >= '0' && data.at(10) <= '9' && data.at(11) == '.' &&
           data.at(12) >= '0' && data.at(12) <= '9' && data.size() < 64 * 1024 * 1024;
}

bool hasPsfHeader(const QByteArray& data)
{
    return data.size() >= 2 &&
           ((quint8(data.at(0)) == 0x36 && quint8(data.at(1)) == 0x04) ||
            (data.size() >= 4 && quint8(data.at(0)) == 0x72 && quint8(data.at(1)) == 0xb5 &&
             quint8(data.at(2)) == 0x4a && quint8(data.at(3)) == 0x86));
}

bool hasPcfHeader(const QByteArray& data)
{
    return data.size() >= 4 && quint8(data.at(0)) == 1 &&
           data.at(1) == 'f' && data.at(2) == 'c' && data.at(3) == 'p';
}

bool hasPkHeader(const QByteArray& data)
{
    return data.size() >= 3 && quint8(data.at(0)) == 247 && quint8(data.at(1)) == 89;
}

bool hasFzxHeader(const QByteArray& data)
{
    return looksLikeFzxHeader(data);
}

bool hasBmfHeader(const QByteArray& data)
{
    return data.size() >= 4 && quint8(data.at(0)) == 0xe1 &&
           quint8(data.at(1)) == 0xe6 && quint8(data.at(2)) == 0xd5 &&
           quint8(data.at(3)) == 0x1a;
}

bool hasVfontHeader(const QByteArray& data)
{
    if (data.size() < 10 + 256 * 10)
        return false;
    return (quint8(data.at(0)) == 0x01 && quint8(data.at(1)) == 0x1e) ||
           (quint8(data.at(0)) == 0x1e && quint8(data.at(1)) == 0x01);
}

bool parse(const QByteArray& data, BitmapFont* font, QString* error)
{
    if (!font || data.isEmpty() || data.size() > 64 * 1024 * 1024)
        return fail(error, QStringLiteral("Invalid bitmap font input size"));
    if (error) error->clear();
    if (hasAmigaAnimFontHeader(data)) return parseAmigaAnimFont(data, font, error);
    if (hasBmfHeader(data)) return parseBmf(data, font, error);
    if (hasPsfHeader(data)) return parsePsf(data, font, error);
    if (parseAmigaFontStrike(data, font, nullptr)) return true;
    if (hasSunNewsFontHeader(data)) return parseSunNewsFont(data, font, error);
    if (hasDwfHeader(data)) return parseDwf(data, font, error);
    if (hasVfontHeader(data)) return parseVfont(data, font, error);
    if (hasBdfHeader(data)) return parseBdf(data, font, error);
    if (hasPcfHeader(data)) return parsePcf(data, font, error);
    if (hasPkHeader(data)) return parsePk(data, font, error);
    if (hasFzxHeader(data)) return parseFzx(data, font, error);
    if (parseMacNfnt(data, font, nullptr)) return true;
    if (parseMacBitmapSfnt(data, font)) return true;
    return fail(error, QStringLiteral("Unsupported bitmap font signature"));
}

} // namespace font
} // namespace peare
