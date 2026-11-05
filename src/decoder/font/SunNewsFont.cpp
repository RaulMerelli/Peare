#include "SunNewsFont.h"

#include <algorithm>

namespace peare {
namespace font {
namespace {

constexpr quint32 kSignature = 0x8c000003u;
constexpr quint32 kVersion = 0x00010000u;
constexpr int kHeaderSize = 80;
constexpr int kGlyphRecordSize = 16;
constexpr int kGlyphImageHeaderSize = 8;
constexpr int kMaxInputSize = 64 * 1024 * 1024;
constexpr int kMaxBitmapBytes = 64 * 1024 * 1024;
constexpr int kMaxGlyphDimension = 4096;
constexpr int kMaxGlyphCount = 65536;
constexpr int kMaxStringBytes = 1024;

quint16 readU16(const QByteArray& data, int offset)
{
    return (quint16(uchar(data.at(offset))) << 8) | quint16(uchar(data.at(offset + 1)));
}

qint16 readS16(const QByteArray& data, int offset)
{
    return qint16(readU16(data, offset));
}

quint32 readU32(const QByteArray& data, int offset)
{
    return (quint32(uchar(data.at(offset))) << 24) |
           (quint32(uchar(data.at(offset + 1))) << 16) |
           (quint32(uchar(data.at(offset + 2))) << 8) |
           quint32(uchar(data.at(offset + 3)));
}

bool rangeFits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool readString(const QByteArray& data, quint32 offset, QString* value, int* end)
{
    if (offset >= quint32(data.size()))
        return false;
    const int limit = qMin(data.size(), int(offset) + kMaxStringBytes + 1);
    int terminator = int(offset);
    while (terminator < limit && data.at(terminator) != '\0')
        ++terminator;
    if (terminator == limit)
        return false;
    *value = QString::fromLatin1(data.constData() + offset, terminator - int(offset));
    *end = terminator + 1;
    return true;
}

struct GlyphRecord {
    quint32 offset = 0;
    int code = 0;
    int width = 0;
    int height = 0;
    int rowBytes = 0;
    int imageBytes = 0;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;
};

} // namespace

bool hasSunNewsFontHeader(const QByteArray& data)
{
    return data.size() >= kHeaderSize && data.size() <= kMaxInputSize &&
           readU32(data, 0) == kSignature && readU32(data, 4) == kVersion;
}

bool parseSunNewsFont(const QByteArray& data, BitmapFont* font, QString* error)
{
    if (error)
        error->clear();
    if (!font || !hasSunNewsFontHeader(data))
        return fail(error, QStringLiteral("Invalid or unsupported Sun NeWS font header"));

    const quint32 nameOffset = readU32(data, 44);
    const quint32 copyrightOffset = readU32(data, 48);
    const int extendedCount = readU16(data, 84);
    const quint64 extendedTableEnd = 88 + quint64(extendedCount) * kGlyphRecordSize;
    const bool extendedTable = extendedCount > 0 && extendedCount <= kMaxGlyphCount &&
            rangeFits(88, quint64(extendedCount) * kGlyphRecordSize, quint64(data.size())) &&
            copyrightOffset == extendedTableEnd + 4 && nameOffset > copyrightOffset;
    const int tableStart = extendedTable ? 88 : kHeaderSize;
    const int glyphCount = extendedTable ? extendedCount : int(readU16(data, 52)) + 1;
    if (glyphCount > kMaxGlyphCount)
        return fail(error, QStringLiteral("Sun NeWS character map exceeds the supported limit"));
    const quint64 tableEnd = quint64(tableStart) + quint64(glyphCount) * kGlyphRecordSize;
    if (!rangeFits(tableStart, quint64(glyphCount) * kGlyphRecordSize, quint64(data.size())))
        return fail(error, QStringLiteral("Sun NeWS glyph table is truncated"));

    BitmapFont parsed;
    parsed.format = Format::SunNews;
    parsed.name = QStringLiteral("Sun NeWS bitmap font");
    int metadataEnd = int(tableEnd);
    if (nameOffset) {
        int end = 0;
        QString name;
        if (nameOffset < tableEnd || !readString(data, nameOffset, &name, &end))
            return fail(error, QStringLiteral("Invalid Sun NeWS font name"));
        if (!name.isEmpty())
            parsed.name = name;
        metadataEnd = qMax(metadataEnd, end);
    }
    if (copyrightOffset) {
        int end = 0;
        QString ignored;
        if (copyrightOffset < tableEnd || !readString(data, copyrightOffset, &ignored, &end))
            return fail(error, QStringLiteral("Invalid Sun NeWS copyright string"));
        metadataEnd = qMax(metadataEnd, end);
    }

    QVector<GlyphRecord> records;
    QVector<quint32> offsets;
    records.reserve(glyphCount);
    offsets.reserve(glyphCount);
    for (int code = 0; code < glyphCount; ++code) {
        const int recordOffset = tableStart + code * kGlyphRecordSize;
        const quint32 imageOffset = readU32(data, recordOffset + (extendedTable ? 4 : 12));
        if (!imageOffset)
            continue;
        if (imageOffset < quint32(metadataEnd) || !rangeFits(imageOffset, kGlyphImageHeaderSize,
                                                              quint64(data.size())))
            return fail(error, QStringLiteral("Invalid Sun NeWS glyph offset"));

        const quint16 rowMarker = readU16(data, int(imageOffset));
        const int width = readU16(data, int(imageOffset) + 2);
        const int height = readU16(data, int(imageOffset) + 4);
        const int rowBytes = readU16(data, int(imageOffset) + 6);
        const int minimumRowBytes = ((width + 15) / 16) * 2;
        if (width <= 0 || width > kMaxGlyphDimension || height <= 0 ||
            height > kMaxGlyphDimension || rowBytes < minimumRowBytes ||
            rowBytes > ((kMaxGlyphDimension + 15) / 16) * 2 || (rowBytes & 1) ||
            rowMarker != (rowBytes == 2 ? 0x2000 : 0x4000))
            return fail(error, QStringLiteral("Invalid Sun NeWS glyph bitmap header"));

        const quint64 imageBytes = quint64(kGlyphImageHeaderSize) + quint64(rowBytes) * height;
        if (imageBytes > quint64(kMaxBitmapBytes) ||
            !rangeFits(imageOffset, imageBytes, quint64(data.size())))
            return fail(error, QStringLiteral("Sun NeWS glyph bitmap is out of bounds"));

        GlyphRecord record;
        record.offset = imageOffset;
        record.code = code;
        record.width = width;
        record.height = height;
        record.rowBytes = rowBytes;
        record.imageBytes = int(imageBytes);
        record.bearingX = readS16(data, recordOffset + (extendedTable ? 8 : 0));
        record.advance = readS16(data, recordOffset + (extendedTable ? 10 : 4));
        record.bearingY = readS16(data, recordOffset + (extendedTable ? 12 : 2));
        records.push_back(record);
        offsets.push_back(imageOffset);
    }
    if (records.isEmpty())
        return fail(error, QStringLiteral("Sun NeWS font contains no bitmap glyphs"));

    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    for (const GlyphRecord& record : records) {
        const auto next = std::upper_bound(offsets.cbegin(), offsets.cend(), record.offset);
        const quint64 blockEnd = next == offsets.cend() ? quint64(data.size()) : quint64(*next);
        if (quint64(record.offset) + quint64(record.imageBytes) > blockEnd)
            return fail(error, QStringLiteral("Overlapping Sun NeWS glyph bitmaps"));
    }

    parsed.width = readU16(data, 20);
    parsed.height = readU16(data, 22);
    parsed.ascent = readU16(data, 18);
    parsed.descent = qAbs(int(readS16(data, 36)));
    if (parsed.width > kMaxGlyphDimension || parsed.height > kMaxGlyphDimension ||
        parsed.ascent > kMaxGlyphDimension || parsed.descent > kMaxGlyphDimension)
        return fail(error, QStringLiteral("Invalid Sun NeWS font metrics"));

    quint64 totalBitmapBytes = 0;
    for (const GlyphRecord& record : records) {
        if (qAbs(record.advance) > kMaxGlyphDimension ||
            qAbs(record.bearingX) > kMaxGlyphDimension ||
            qAbs(record.bearingY) > kMaxGlyphDimension)
            return fail(error, QStringLiteral("Invalid Sun NeWS glyph metrics"));

        const int targetRowBytes = (record.width + 7) / 8;
        const quint64 targetBytes = quint64(targetRowBytes) * record.height;
        totalBitmapBytes += targetBytes;
        if (totalBitmapBytes > quint64(kMaxBitmapBytes))
            return fail(error, QStringLiteral("Sun NeWS decoded bitmap limit exceeded"));

        Glyph glyph;
        glyph.codepoint = quint32(record.code);
        glyph.width = record.width;
        glyph.height = record.height;
        glyph.bearingX = record.bearingX;
        glyph.bearingY = record.bearingY;
        glyph.advance = record.advance;
        glyph.bitmap = QByteArray(int(targetBytes), '\0');
        const int sourceOffset = int(record.offset) + kGlyphImageHeaderSize;
        for (int y = 0; y < record.height; ++y) {
            for (int x = 0; x < record.width; ++x) {
                const int sourceWord = sourceOffset + y * record.rowBytes + (x / 16) * 2;
                const quint16 bits = readU16(data, sourceWord);
                if (bits & quint16(0x8000u >> (x & 15))) {
                    const int target = y * targetRowBytes + x / 8;
                    glyph.bitmap[target] = char(uchar(glyph.bitmap.at(target)) |
                                                uchar(0x80u >> (x & 7)));
                }
            }
        }
        parsed.width = qMax(parsed.width, glyph.width);
        parsed.height = qMax(parsed.height, glyph.height);
        parsed.ascent = qMax(parsed.ascent, qMax(0, glyph.bearingY));
        parsed.glyphs.push_back(glyph);
    }

    parsed.fixedWidth = true;
    for (int i = 1; i < parsed.glyphs.size(); ++i) {
        if (parsed.glyphs.at(i).advance != parsed.glyphs.first().advance) {
            parsed.fixedWidth = false;
            break;
        }
    }
    *font = parsed;
    return true;
}

} // namespace font
} // namespace peare
