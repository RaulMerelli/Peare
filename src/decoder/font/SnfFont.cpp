#include "SnfFont.h"

namespace peare {
namespace font {
namespace {

constexpr quint32 kHeaderSize = 108;
constexpr quint32 kGlyphRecordSize = 16;
constexpr quint32 kPropertyRecordSize = 12;
constexpr quint32 kMaxInputSize = 32 * 1024 * 1024;

quint32 read32(const QByteArray& data, quint32 offset, bool littleEndian)
{
    const auto byte = [&data](quint32 index) { return quint32(uchar(data.at(int(index)))); };
    if (littleEndian)
        return byte(offset) | (byte(offset + 1) << 8) | (byte(offset + 2) << 16) |
               (byte(offset + 3) << 24);
    return (byte(offset) << 24) | (byte(offset + 1) << 16) | (byte(offset + 2) << 8) |
           byte(offset + 3);
}

quint16 read16(const QByteArray& data, quint32 offset, bool littleEndian)
{
    const quint32 first = uchar(data.at(int(offset)));
    const quint32 second = uchar(data.at(int(offset + 1)));
    return quint16(littleEndian ? first | (second << 8) : (first << 8) | second);
}

qint16 readSigned16(const QByteArray& data, quint32 offset, bool littleEndian)
{
    return qint16(read16(data, offset, littleEndian));
}

bool fits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

bool readString(const QByteArray& data, quint32 offset, quint32 length, QString* out)
{
    if (offset >= length)
        return false;
    const int start = int(offset);
    const int end = data.indexOf('\0', start);
    if (end < 0 || quint32(end) >= length)
        return false;
    *out = QString::fromLatin1(data.constData() + start, end - start);
    return true;
}

bool parseLayout(const QByteArray& data, bool littleEndian, SnfFont* out)
{
    if (quint32(data.size()) < kHeaderSize || quint32(data.size()) > kMaxInputSize)
        return false;
    if (read32(data, 0, littleEndian) != 4 || read32(data, 104, littleEndian) != 4)
        return false;

    const quint32 firstColumn = read32(data, 28, littleEndian);
    const quint32 lastColumn = read32(data, 32, littleEndian);
    const quint32 firstRow = read32(data, 36, littleEndian);
    const quint32 lastRow = read32(data, 40, littleEndian);
    const quint32 propertyCount = read32(data, 44, littleEndian);
    const quint32 stringBytes = read32(data, 48, littleEndian);
    const quint32 maxBoundsOffset = read32(data, 92, littleEndian);
    const quint32 glyphBytes = littleEndian ? maxBoundsOffset & 0x00ffffffu
                                            : maxBoundsOffset >> 8;
    if (firstColumn > lastColumn || lastColumn > 255 || firstRow > lastRow || lastRow > 255 ||
        propertyCount > 4096 || stringBytes > 1024 * 1024)
        return false;

    const quint64 rows = quint64(lastRow) - firstRow + 1;
    const quint64 columns = quint64(lastColumn) - firstColumn + 1;
    const quint64 glyphCount = rows * columns;
    const quint64 glyphTable = kHeaderSize;
    const quint64 glyphData = glyphTable + glyphCount * kGlyphRecordSize;
    const quint64 propertyTable = glyphData + ((quint64(glyphBytes) + 3u) & ~quint64(3));
    const quint64 stringTable = propertyTable + quint64(propertyCount) * kPropertyRecordSize;
    if (glyphCount > 65536 || !fits(glyphTable, glyphCount * kGlyphRecordSize, quint64(data.size())) ||
        !fits(glyphData, (quint64(glyphBytes) + 3u) & ~quint64(3), quint64(data.size())) ||
        !fits(propertyTable, quint64(propertyCount) * kPropertyRecordSize, quint64(data.size())) ||
        !fits(stringTable, stringBytes, quint64(data.size())))
        return false;

    SnfFont parsed;
    parsed.littleEndian = littleEndian;
    parsed.firstCode = firstRow * 256u + firstColumn;
    parsed.lastCode = lastRow * 256u + lastColumn;
    parsed.fontDescent = qint32(read32(data, 56, littleEndian));
    parsed.fontAscent = qint32(read32(data, 60, littleEndian));
    parsed.propertyCount = propertyCount;
    parsed.glyphSlotCount = quint32(glyphCount);
    parsed.glyphs.reserve(int(glyphCount));

    for (quint64 index = 0; index < glyphCount; ++index) {
        const quint32 record = quint32(glyphTable + index * kGlyphRecordSize);
        const quint32 packed = read32(data, record + 12, littleEndian);
        const quint32 bitmapOffset = littleEndian ? packed & 0x00ffffffu : packed >> 8;
        const bool exists = littleEndian ? ((packed >> 24) & 1u) != 0
                                         : ((packed >> 7) & 1u) != 0;
        if (bitmapOffset > glyphBytes)
            return false;
        if (!exists)
            continue;
        SnfGlyphMetric glyph;
        glyph.code = (firstRow + quint32(index / columns)) * 256u +
                     firstColumn + quint32(index % columns);
        glyph.leftBearing = readSigned16(data, record, littleEndian);
        glyph.rightBearing = readSigned16(data, record + 2, littleEndian);
        glyph.advance = readSigned16(data, record + 4, littleEndian);
        glyph.ascent = readSigned16(data, record + 6, littleEndian);
        glyph.descent = readSigned16(data, record + 8, littleEndian);
        parsed.glyphs.append(glyph);
    }

    const QByteArray strings = data.mid(int(stringTable), int(stringBytes));
    for (quint32 index = 0; index < propertyCount; ++index) {
        const quint32 record = quint32(propertyTable + quint64(index) * kPropertyRecordSize);
        const quint32 nameOffset = read32(data, record, littleEndian);
        const quint32 value = read32(data, record + 4, littleEndian);
        const quint32 indirect = read32(data, record + 8, littleEndian);
        QString propertyName;
        if (indirect > 1 || !readString(strings, nameOffset, stringBytes, &propertyName))
            return false;
        if (indirect && propertyName == QLatin1String("FONT") &&
            !readString(strings, value, stringBytes, &parsed.name))
            return false;
    }

    *out = parsed;
    return true;
}

} // namespace

bool hasSnfHeader(const QByteArray& data)
{
    SnfFont font;
    return parseSnf(data, &font, nullptr);
}

bool parseSnf(const QByteArray& data, SnfFont* font, QString* error)
{
    if (!font)
        return false;
    if (parseLayout(data, true, font) || parseLayout(data, false, font)) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("Invalid or unsupported X11 SNF version-4 layout");
    return false;
}

} // namespace font
} // namespace peare
