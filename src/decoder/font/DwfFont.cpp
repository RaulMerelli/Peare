#include "DwfFont.h"

#include <algorithm>
#include <limits>

namespace peare {
namespace font {
namespace {

constexpr quint32 kHeaderSize = 84;
constexpr quint32 kMaxGlyphCount = 256;
constexpr quint32 kMaxGlyphDimension = 4096;
constexpr quint32 kMaxFontSize = 64 * 1024 * 1024;

quint8 byteAt(const QByteArray& data, quint32 offset)
{
    return quint8(data.at(int(offset)));
}

quint16 readLe16(const QByteArray& data, quint32 offset)
{
    return quint16(byteAt(data, offset)) | (quint16(byteAt(data, offset + 1)) << 8);
}

qint32 readSignedLe16(const QByteArray& data, quint32 offset)
{
    const quint16 value = readLe16(data, offset);
    return value < 0x8000u ? qint32(value) : qint32(value) - 0x10000;
}

qint32 readSigned8(const QByteArray& data, quint32 offset)
{
    const quint8 value = byteAt(data, offset);
    return value < 0x80u ? qint32(value) : qint32(value) - 0x100;
}

quint32 readLe32(const QByteArray& data, quint32 offset)
{
    return quint32(byteAt(data, offset)) | (quint32(byteAt(data, offset + 1)) << 8) |
           (quint32(byteAt(data, offset + 2)) << 16) | (quint32(byteAt(data, offset + 3)) << 24);
}

quint16 readBe16(const QByteArray& data, quint32 offset)
{
    return (quint16(byteAt(data, offset)) << 8) | quint16(byteAt(data, offset + 1));
}

bool parseCompX10SingleGlyph(const QByteArray& data, BitmapFont* out)
{
    const quint32 fontHeight = readLe16(data, 0x14);
    const quint32 fontWidth = readLe16(data, 0x16);
    const quint32 nameLength = readLe16(data, 6);
    if (nameLength == 0 || nameLength > quint32(data.size()) - 0x60u)
        return false;
    const quint32 field = 0x60u + nameLength + 0x30u;
    if (field > quint32(data.size()) - 8u)
        return false;
    const quint32 record = readLe32(data, field);
    const quint32 recordSize = readLe32(data, field + 4u);
    if (record < 0xb0u || record > quint32(data.size()) - 15u ||
        readLe16(data, record) != 4u || readLe16(data, record + 2u) != 0xffffu)
        return false;
    const qint32 left = readSignedLe16(data, record + 4u);
    const qint32 right = readSignedLe16(data, record + 6u);
    const qint32 advance = readSignedLe16(data, record + 8u);
    const qint32 ascent = readSignedLe16(data, record + 10u);
    const qint32 descent = readSignedLe16(data, record + 12u);
    const qint32 width = right - left;
    const qint32 height = ascent + descent;
    if (width <= 0 || height <= 0 || quint32(width) != fontWidth ||
        quint32(height) != fontHeight || advance < 0 ||
        quint32(width) > kMaxGlyphDimension || quint32(height) > kMaxGlyphDimension)
        return false;
    const quint32 stride = (quint32(width) + 7u) / 8u;
    const quint64 bitmapSize = quint64(stride) * quint32(height);
    const quint32 bitmapStart = record + 15u;
    if (bitmapSize > quint32(data.size()) - bitmapStart ||
        recordSize != 15u + bitmapSize || bitmapStart + bitmapSize != quint32(data.size()))
        return false;

    BitmapFont font;
    font.format = Format::Dwf;
    font.height = int(height);
    font.width = int(width);
    font.ascent = int(ascent);
    font.descent = int(descent);
    font.fixedWidth = true;
    font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
    Glyph glyph;
    glyph.codepoint = 0;
    glyph.width = int(width);
    glyph.height = int(height);
    glyph.bearingX = int(left);
    glyph.bearingY = int(ascent);
    glyph.advance = int(advance);
    glyph.bitmap = data.mid(int(bitmapStart), int(bitmapSize));
    font.glyphs.append(glyph);
    *out = font;
    return true;
}

bool parseCompX10(const QByteArray& data, BitmapFont* out)
{
    const quint8 marker = byteAt(data, 0x54);
    if (marker != 4 && marker != 5 && marker != 6)
        return false;
    const quint32 glyphCount = readLe32(data, 0x4c);
    if (glyphCount == 0 || glyphCount > kMaxGlyphCount)
        return false;
    if (glyphCount == 1 && parseCompX10SingleGlyph(data, out))
        return true;
    const quint32 fontHeight = readLe16(data, 0x14);
    if (fontHeight == 0 || fontHeight > kMaxGlyphDimension)
        return false;
    const quint32 nameLength = readLe16(data, 6);
    if (nameLength == 0 || nameLength > quint32(data.size()) - 0x60)
        return false;
    const QByteArray fontName(data.constData() + 0x60, int(nameLength));
    if ((marker == 4 || marker == 5) && fontName.startsWith("-"))
        return false;

    QVector<quint32> bestOffsets;
    quint32 bestCount = 0;
    const quint32 lastTable = qMin(quint32(data.size()) - glyphCount * 2, 0x1000u);
    for (quint32 table = kHeaderSize; table <= lastTable; ++table) {
        QVector<quint32> offsets(int(glyphCount), std::numeric_limits<quint32>::max());
        quint32 populated = 0;
        quint32 previous = 0;
        bool ordered = true;
        for (quint32 index = 0; index < glyphCount; ++index) {
            const quint32 offset = readLe16(data, table + index * 2);
            if (offset == 0)
                continue;
            const quint32 target = table + offset;
            if (target < table + glyphCount * 2 || target > quint32(data.size()) - 5)
                continue;
            const quint32 width = byteAt(data, target + 1);
            const quint32 ascent = byteAt(data, target + 3);
            const quint32 descent = byteAt(data, target + 4);
            const quint32 height = ascent + descent;
            const quint32 bitmapSize = (width * height + 7) / 8;
            if (byteAt(data, target) != 0 || width == 0 || width > 256 || height == 0 ||
                height > 256 || (marker != 6 && height != fontHeight) ||
                bitmapSize > quint32(data.size()) - target - 5)
                continue;
            if (populated != 0 && target <= previous)
                ordered = false;
            previous = target;
            offsets[int(index)] = target;
            ++populated;
        }

        QVector<quint32> records;
        records.reserve(int(populated));
        for (quint32 target : offsets)
            if (target != std::numeric_limits<quint32>::max())
                records.append(target);
        std::sort(records.begin(), records.end());
        records.erase(std::unique(records.begin(), records.end()), records.end());
        for (quint32 index = 0; index < glyphCount; ++index) {
            const quint32 target = offsets.at(int(index));
            if (target == std::numeric_limits<quint32>::max())
                continue;
            const auto record = std::lower_bound(records.cbegin(), records.cend(), target);
            const quint32 next = record + 1 == records.cend()
                ? quint32(data.size()) : *(record + 1);
            const quint32 width = byteAt(data, target + 1);
            const quint32 height = byteAt(data, target + 3) + byteAt(data, target + 4);
            const quint32 bitmapSize = (width * height + 7) / 8;
            if (target + 5 + bitmapSize > next)
                offsets[int(index)] = std::numeric_limits<quint32>::max();
        }
        populated = 0;
        for (quint32 target : offsets)
            if (target != std::numeric_limits<quint32>::max())
                ++populated;
        if (ordered && populated > bestCount) {
            bestCount = populated;
            bestOffsets = offsets;
        }
    }
    if (bestCount < qMin(2u, glyphCount))
        return false;

    BitmapFont font;
    font.format = Format::Dwf;
    if (nameLength > 0 && nameLength <= quint32(data.size()) - 0x60)
        font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
    if (font.name.isEmpty())
        font.name = QStringLiteral("DECWindows Font");
    font.height = int(fontHeight);
    font.width = readLe16(data, 0x16);
    if (font.height == 0 || font.height > int(kMaxGlyphDimension) ||
        font.width > int(kMaxGlyphDimension))
        return false;
    font.fixedWidth = true;
    font.glyphs.reserve(int(bestCount));
    int priorAdvance = -1;
    for (quint32 index = 0; index < glyphCount; ++index) {
        const quint32 record = bestOffsets.at(int(index));
        if (record == std::numeric_limits<quint32>::max())
            continue;
        const quint32 width = byteAt(data, record + 1);
        const quint32 advance = byteAt(data, record + 2);
        const quint32 ascent = byteAt(data, record + 3);
        const quint32 descent = byteAt(data, record + 4);
        const quint32 height = ascent + descent;
        const quint32 stride = (width + 7) / 8;
        Glyph glyph;
        glyph.codepoint = index;
        glyph.width = int(width);
        glyph.height = int(height);
        glyph.bearingY = int(ascent);
        glyph.advance = int(advance);
        glyph.bitmap = QByteArray(int(stride * height), '\0');
        for (quint32 bit = 0; bit < width * height; ++bit) {
            if (!(byteAt(data, record + 5 + bit / 8) & (1u << (bit & 7))))
                continue;
            const quint32 row = bit / width;
            const quint32 column = bit % width;
            const quint32 targetByte = row * stride + column / 8;
            glyph.bitmap[int(targetByte)] = char(quint8(glyph.bitmap.at(int(targetByte))) |
                                                  (0x80u >> (column & 7)));
        }
        font.width = qMax(font.width, glyph.width);
        font.ascent = qMax(font.ascent, int(ascent));
        font.descent = qMax(font.descent, int(descent));
        if (priorAdvance >= 0 && priorAdvance != glyph.advance)
            font.fixedWidth = false;
        priorAdvance = glyph.advance;
        font.glyphs.append(glyph);
    }
    if (font.height == 0)
        font.height = font.ascent + font.descent;
    *out = font;
    return true;
}

bool parseTerminalDwf(const QByteArray& data, BitmapFont* out)
{
    const quint32 firstCode = readLe32(data, 0x44);
    const quint32 lastCode = readLe32(data, 0x4c);
    if (firstCode != 0 || lastCode != 253)
        return false;
    const quint32 nameLength = readLe16(data, 6);
    const quint32 nameEnd = 0x60u + nameLength;
    if (nameEnd >= quint32(data.size()))
        return false;

    struct GlyphRecord {
        quint32 codepoint;
        quint32 offset;
        quint32 size;
        qint32 left;
        qint32 right;
        qint32 advance;
        qint32 ascent;
        qint32 descent;
    };

    BitmapFont bestFont;
    quint32 bestScore = 0;
    for (quint32 terminator = nameEnd; terminator + 9u <= quint32(data.size()); ++terminator) {
        if (byteAt(data, terminator) != 0)
            continue;
        const quint32 tableStart = readLe32(data, terminator + 1u);
        const quint32 dataLength = readLe32(data, terminator + 5u);
        if (tableStart != terminator + 9u || dataLength != quint32(data.size()) - tableStart ||
            tableStart > quint32(data.size()) - 2u)
            continue;

        const quint32 offsetBase = tableStart + 2u;
        const quint64 tableEnd64 = quint64(offsetBase) + 253u * 2u;
        if (tableEnd64 > quint32(data.size()))
            continue;
        const quint32 tableEnd = quint32(tableEnd64);
        QVector<quint32> glyphOffsets(253, std::numeric_limits<quint32>::max());
        QVector<quint32> recordStarts;
        for (quint32 i = 0; i < 253; ++i) {
            const quint32 relative = readLe16(data, offsetBase + i * 2u);
            if (relative == 0 || relative == 0xffffu)
                continue;
            const quint64 metricOffset = quint64(offsetBase) + relative;
            if (metricOffset < quint64(tableEnd) + 2u || metricOffset > quint32(data.size()) - 3u)
                continue;
            const quint32 recordStart = quint32(metricOffset) - 2u;
            glyphOffsets[int(i)] = recordStart;
            recordStarts.append(recordStart);
        }
        if (recordStarts.size() < 8)
            continue;
        std::sort(recordStarts.begin(), recordStarts.end());
        recordStarts.erase(std::unique(recordStarts.begin(), recordStarts.end()), recordStarts.end());

        QVector<GlyphRecord> parsed;
        for (quint32 i = 0; i < 253; ++i) {
            const quint32 recordStart = glyphOffsets.at(int(i));
            if (recordStart == std::numeric_limits<quint32>::max())
                continue;
            const auto recordIt = std::lower_bound(recordStarts.cbegin(), recordStarts.cend(), recordStart);
            const quint32 next = recordIt + 1 == recordStarts.cend()
                ? quint32(data.size()) : *(recordIt + 1);
            if (next < recordStart + 5u)
                continue;

            GlyphRecord record;
            record.codepoint = i + 1u;
            record.offset = recordStart + 5u;
            record.size = next - record.offset;
            record.left = readSigned8(data, recordStart);
            record.right = readSigned8(data, recordStart + 1u);
            record.advance = readSigned8(data, recordStart + 2u);
            record.ascent = readSigned8(data, recordStart + 3u);
            record.descent = readSigned8(data, recordStart + 4u);

            const qint32 width = record.right - record.left;
            const qint32 height = record.ascent + record.descent;
            if (width < 0 || height < 0 || quint32(width) > kMaxGlyphDimension ||
                quint32(height) > kMaxGlyphDimension || record.advance < 0)
                continue;
            const quint64 bitmapSize = (quint64(width) * quint32(height) + 7u) / 8u;
            if (bitmapSize != record.size)
                continue;
            parsed.append(record);
        }
        const quint32 score = quint32(parsed.size());
        if (score < 8 || score * 100u < quint32(recordStarts.size()) * 95u || score <= bestScore)
            continue;

        BitmapFont font;
        font.format = Format::Dwf;
        font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
        while (font.name.endsWith(QChar::Null))
            font.name.chop(1);
        if (font.name.isEmpty())
            font.name = QStringLiteral("DECWindows Font");
        font.height = readLe16(data, 0x14);
        font.width = readLe16(data, 0x16);
        font.fixedWidth = true;
        font.glyphs.reserve(int(score));
        int priorAdvance = std::numeric_limits<int>::min();
        for (const GlyphRecord& record : parsed) {
            const quint32 width = quint32(record.right - record.left);
            const quint32 height = quint32(record.ascent + record.descent);
            const quint32 stride = (width + 7u) / 8u;
            Glyph glyph;
            glyph.codepoint = record.codepoint;
            glyph.width = int(width);
            glyph.height = int(height);
            glyph.bearingX = int(record.left);
            glyph.bearingY = int(record.ascent);
            glyph.advance = int(record.advance);
            glyph.bitmap = QByteArray(int(stride * height), '\0');
            for (quint32 bit = 0; bit < width * height; ++bit) {
                if (!(byteAt(data, record.offset + bit / 8u) & (1u << (bit & 7u))))
                    continue;
                const quint32 row = bit / width;
                const quint32 column = bit % width;
                const quint32 targetByte = row * stride + column / 8u;
                glyph.bitmap[int(targetByte)] = char(quint8(glyph.bitmap.at(int(targetByte))) |
                                                      (0x80u >> (column & 7u)));
            }
            font.width = qMax(font.width, glyph.width);
            font.ascent = qMax(font.ascent, int(record.ascent));
            font.descent = qMax(font.descent, int(record.descent));
            if (priorAdvance != std::numeric_limits<int>::min() && priorAdvance != glyph.advance)
                font.fixedWidth = false;
            priorAdvance = glyph.advance;
            font.glyphs.append(glyph);
        }
        bestFont = font;
        bestScore = score;
    }
    if (bestScore == 0)
        return false;
    *out = bestFont;
    return true;
}

bool parseLegacyDwf(const QByteArray& data, BitmapFont* out)
{
    const quint32 firstCode = readLe32(data, 0x44);
    const quint32 lastCode = readLe32(data, 0x4c);
    if (firstCode > lastCode || lastCode > 255)
        return false;
    const quint32 glyphCount = lastCode - firstCode + 1;
    const quint32 nameLength = readLe16(data, 6);
    const quint32 nameEnd = 0x60u + nameLength;
    if (nameEnd >= quint32(data.size()))
        return false;

    struct Record {
        quint32 codepoint;
        quint32 offset;
        quint32 size;
        qint32 left;
        qint32 right;
        qint32 advance;
        qint32 ascent;
        qint32 descent;
    };

    BitmapFont bestFont;
    quint32 bestScore = 0;
    for (quint32 terminator = nameEnd; terminator + 9u <= quint32(data.size()); ++terminator) {
        if (byteAt(data, terminator) != 0)
            continue;
        const quint32 tableStart = readLe32(data, terminator + 1u);
        const quint32 dataLength = readLe32(data, terminator + 5u);
        if (tableStart != terminator + 9u || dataLength != quint32(data.size()) - tableStart)
            continue;
        const quint32 dataEnd = tableStart + dataLength;
        const quint64 tableEnd64 = quint64(tableStart) + quint64(glyphCount) * 2u;
        if (tableEnd64 > dataEnd)
            continue;
        const quint32 tableEnd = quint32(tableEnd64);

        QVector<quint32> offsets(int(glyphCount), std::numeric_limits<quint32>::max());
        QVector<quint32> records;
        for (quint32 i = 0; i < glyphCount; ++i) {
            const quint32 relative = readLe16(data, tableStart + i * 2u);
            if (relative == 0 || relative == 0xffffu)
                continue;
            const quint64 target64 = quint64(tableStart) + relative;
            if (target64 < tableEnd || target64 >= dataEnd)
                continue;
            offsets[int(i)] = quint32(target64);
            records.append(quint32(target64));
        }
        if (records.size() < 8)
            continue;
        std::sort(records.begin(), records.end());
        records.erase(std::unique(records.begin(), records.end()), records.end());

        struct MetricLayout {
            quint32 bytes;
            bool wide;
        };
        const MetricLayout layouts[] = {{5u, false}, {6u, false}, {10u, true}};
        for (const MetricLayout& layout : layouts) {
            const quint32 metricBytes = layout.bytes;
            QVector<Record> parsed;
            quint32 score = 0;
            for (quint32 i = 0; i < glyphCount; ++i) {
                const quint32 record = offsets.at(int(i));
                if (record == std::numeric_limits<quint32>::max())
                    continue;
                const auto recordIt = std::lower_bound(records.cbegin(), records.cend(), record);
                const quint32 next = recordIt + 1 == records.cend() ? dataEnd : *(recordIt + 1);
                if (next <= record || next - record < metricBytes)
                    continue;

                Record glyph;
                glyph.codepoint = firstCode + i;
                glyph.offset = record + metricBytes;
                glyph.size = next - record - metricBytes;
                if (layout.wide) {
                    glyph.left = readSignedLe16(data, record);
                    glyph.right = readSignedLe16(data, record + 2u);
                    glyph.advance = readSignedLe16(data, record + 4u);
                    glyph.ascent = readSignedLe16(data, record + 6u);
                    glyph.descent = readSignedLe16(data, record + 8u);
                } else {
                    glyph.left = readSigned8(data, record);
                    glyph.right = readSigned8(data, record + 1u);
                    glyph.advance = readSigned8(data, record + 2u);
                    glyph.ascent = readSigned8(data, record + 3u);
                    glyph.descent = readSigned8(data, record + 4u);
                }
                const qint32 width = glyph.right - glyph.left;
                const qint32 height = glyph.ascent + glyph.descent;
                if (width < 0 || height < 0 || quint32(width) > kMaxGlyphDimension ||
                    quint32(height) > kMaxGlyphDimension || glyph.advance < 0)
                    continue;
                const quint64 bitmapSize = (quint64(width) * quint32(height) + 7u) / 8u;
                if (bitmapSize != glyph.size) {
                    if (!layout.wide || bitmapSize + 1u != glyph.size)
                        continue;
                    ++glyph.offset;
                    --glyph.size;
                }
                if (bitmapSize != glyph.size)
                    continue;
                parsed.append(glyph);
                ++score;
            }
            if (score < 8 || score * 100u < quint32(records.size()) * 95u || score <= bestScore)
                continue;

            BitmapFont font;
            font.format = Format::Dwf;
            if (nameLength > 0 && nameLength <= quint32(data.size()) - 0x60u) {
                font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
                while (font.name.endsWith(QChar::Null))
                    font.name.chop(1);
            }
            if (font.name.isEmpty())
                font.name = QStringLiteral("DECWindows Font");
            font.height = readLe16(data, 0x14);
            font.width = readLe16(data, 0x16);
            font.fixedWidth = true;
            font.glyphs.reserve(int(score));
            int priorAdvance = std::numeric_limits<int>::min();
            for (const Record& record : parsed) {
                const qint32 width = record.right - record.left;
                const qint32 height = record.ascent + record.descent;
                const quint32 stride = (quint32(width) + 7u) / 8u;
                Glyph glyph;
                glyph.codepoint = record.codepoint;
                glyph.width = int(width);
                glyph.height = int(height);
                glyph.bearingX = int(record.left);
                glyph.bearingY = int(record.ascent);
                glyph.advance = int(record.advance);
                glyph.bitmap = QByteArray(int(stride * quint32(height)), '\0');
                for (quint32 bit = 0; bit < quint32(width) * quint32(height); ++bit) {
                    if (!(byteAt(data, record.offset + bit / 8u) & (1u << (bit & 7u))))
                        continue;
                    const quint32 row = bit / quint32(width);
                    const quint32 column = bit % quint32(width);
                    const quint32 targetByte = row * stride + column / 8u;
                    glyph.bitmap[int(targetByte)] = char(quint8(glyph.bitmap.at(int(targetByte))) |
                                                          (0x80u >> (column & 7u)));
                }
                font.width = qMax(font.width, glyph.width);
                font.ascent = qMax(font.ascent, int(record.ascent));
                font.descent = qMax(font.descent, int(record.descent));
                if (priorAdvance != std::numeric_limits<int>::min() && priorAdvance != glyph.advance)
                    font.fixedWidth = false;
                priorAdvance = glyph.advance;
                font.glyphs.append(glyph);
            }
            bestFont = font;
            bestScore = score;
        }
    }
    if (bestScore == 0)
        return false;
    *out = bestFont;
    return true;
}

bool parseMitX11Indexed(const QByteArray& data, BitmapFont* out)
{
    const quint32 firstCode = readLe32(data, 0x44);
    const quint32 lastCode = readLe32(data, 0x4c);
    if (firstCode > lastCode || lastCode > 255)
        return false;
    const quint32 glyphCount = lastCode - firstCode + 1;
    const QByteArray property = QByteArrayLiteral("Public domain font.");
    const int propertyOffset = data.indexOf(property);
    if (propertyOffset < 0)
        return false;
    const int propertyEnd = data.indexOf('\0', propertyOffset);
    if (propertyEnd < 0 || quint32(data.size() - propertyEnd) < 9)
        return false;
    const quint32 tableStart = readLe32(data, quint32(propertyEnd + 1));
    const quint32 dataLength = readLe32(data, quint32(propertyEnd + 5));
    if (tableStart < kHeaderSize || tableStart > quint32(data.size()) ||
        dataLength > quint32(data.size()) - tableStart)
        return false;
    const quint32 dataEnd = tableStart + dataLength;
    const quint64 tableEnd64 = quint64(tableStart) + quint64(glyphCount) * 2u;
    if (tableEnd64 > dataEnd)
        return false;
    const quint32 tableEnd = quint32(tableEnd64);

    QVector<quint32> offsets(int(glyphCount), std::numeric_limits<quint32>::max());
    QVector<quint32> records;
    for (quint32 i = 0; i < glyphCount; ++i) {
        const quint32 relative = readLe16(data, tableStart + i * 2u);
        if (relative == 0 || relative == 0xffffu)
            continue;
        const quint64 target64 = quint64(tableStart) + relative;
        if (target64 < tableEnd || target64 >= dataEnd)
            return false;
        offsets[int(i)] = quint32(target64);
        records.append(quint32(target64));
    }
    if (records.isEmpty())
        return false;
    std::sort(records.begin(), records.end());
    records.erase(std::unique(records.begin(), records.end()), records.end());

    struct MetricLayout {
        quint32 metricBytes;
        bool wide;
    };
    const MetricLayout layouts[] = {{5, false}, {10, true}};
    BitmapFont bestFont;
    quint32 bestCount = 0;
    for (const MetricLayout& layout : layouts) {
        BitmapFont font;
        font.format = Format::Dwf;
        font.height = readLe16(data, 0x14);
        font.width = readLe16(data, 0x16);
        const quint32 nameLength = readLe16(data, 6);
        if (nameLength > 0 && nameLength <= quint32(data.size()) - 0x60) {
            font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
            while (font.name.endsWith(QChar::Null))
                font.name.chop(1);
        }
        if (font.name.isEmpty())
            font.name = QStringLiteral("DECWindows Font");
        font.fixedWidth = true;
        int priorAdvance = std::numeric_limits<int>::min();
        bool validLayout = true;
        for (quint32 i = 0; i < glyphCount; ++i) {
            const quint32 record = offsets.at(int(i));
            if (record == std::numeric_limits<quint32>::max())
                continue;
            const auto recordIt = std::lower_bound(records.cbegin(), records.cend(), record);
            const quint32 next = recordIt + 1 == records.cend()
                ? dataEnd : *(recordIt + 1);
            const bool lastRecord = recordIt + 1 == records.cend();
            if (next <= record || next - record < layout.metricBytes) {
                validLayout = false;
                break;
            }

            qint32 left = 0;
            qint32 right = 0;
            qint32 advance = 0;
            qint32 ascent = 0;
            qint32 descent = 0;
            if (layout.wide) {
                left = readSignedLe16(data, record);
                right = readSignedLe16(data, record + 2);
                advance = readSignedLe16(data, record + 4);
                ascent = readSignedLe16(data, record + 6);
                descent = readSignedLe16(data, record + 8);
            } else {
                left = readSigned8(data, record);
                right = readSigned8(data, record + 1);
                advance = readSigned8(data, record + 2);
                ascent = readSigned8(data, record + 3);
                descent = readSigned8(data, record + 4);
            }
            const qint32 width = right - left;
            const qint32 height = ascent + descent;
            if (width <= 0 || quint32(width) > kMaxGlyphDimension || height <= 0 ||
                quint32(height) > kMaxGlyphDimension || advance < 0 ||
                quint32(next - record - layout.metricBytes) > kMaxFontSize) {
                validLayout = false;
                break;
            }
            const quint32 rowBytes = (quint32(width) + 7u) / 8u;
            const quint64 rowSize = quint64(rowBytes) * quint32(height);
            const quint64 packedSize = (quint64(width) * quint32(height) + 7u) / 8u;
            const quint32 bitmapLength = next - record - layout.metricBytes;
            const bool rowPadded = rowSize <= bitmapLength &&
                (bitmapLength - rowSize <= 1 || (lastRecord && rowSize == packedSize));
            const bool tightlyPacked = packedSize <= bitmapLength &&
                (bitmapLength - packedSize <= 1 || lastRecord);
            if (!rowPadded && !tightlyPacked) {
                validLayout = false;
                break;
            }

            Glyph glyph;
            glyph.codepoint = firstCode + i;
            glyph.width = int(width);
            glyph.height = int(height);
            glyph.bearingX = int(left);
            glyph.bearingY = int(ascent);
            glyph.advance = int(advance);
            glyph.bitmap = QByteArray(int(rowSize), '\0');
            const quint32 bitmapStart = record + layout.metricBytes;
            for (quint32 y = 0; y < quint32(height); ++y) {
                for (quint32 x = 0; x < quint32(width); ++x) {
                    const quint64 sourceBit = rowPadded
                        ? quint64(y) * rowBytes * 8u + x
                        : quint64(y) * quint32(width) + x;
                    if (!(byteAt(data, bitmapStart + quint32(sourceBit / 8u)) &
                          (0x80u >> (sourceBit & 7u))))
                        continue;
                    const quint32 targetByte = y * rowBytes + x / 8u;
                    glyph.bitmap[int(targetByte)] = char(
                        quint8(glyph.bitmap.at(int(targetByte))) | (0x80u >> (x & 7u)));
                }
            }
            font.width = qMax(font.width, glyph.width);
            font.ascent = qMax(font.ascent, int(ascent));
            font.descent = qMax(font.descent, int(descent));
            if (priorAdvance != std::numeric_limits<int>::min() && priorAdvance != glyph.advance)
                font.fixedWidth = false;
            priorAdvance = glyph.advance;
            font.glyphs.append(glyph);
        }
        if (validLayout && font.glyphs.size() > int(bestCount)) {
            bestCount = quint32(font.glyphs.size());
            bestFont = font;
        }
    }
    if (bestCount == 0)
        return false;
    *out = bestFont;
    return true;
}

bool parseMitX11(const QByteArray& data, BitmapFont* out)
{
    if (parseMitX11Indexed(data, out))
        return true;
    if (byteAt(data, 0x54) != 5)
        return false;
    const quint32 glyphCount = readLe32(data, 0x4c);
    if (glyphCount == 0 || glyphCount > kMaxGlyphCount)
        return false;
    const quint32 tableLimit = qMin(quint32(data.size()) - glyphCount * 2, quint32(0x1000));
    QVector<quint32> bestOffsets;
    QVector<quint32> bestBitmapSizes;
    quint32 bestScore = 0;

    for (quint32 table = kHeaderSize; table <= tableLimit; ++table) {
        const qint64 base = qint64(table) - 18;
        const quint32 dataStart = table + glyphCount * 2 + 2;
        if (dataStart > quint32(data.size()))
            continue;
        QVector<quint32> offsets(int(glyphCount), std::numeric_limits<quint32>::max());
        quint32 populated = 0;
        for (quint32 i = 0; i < glyphCount; ++i) {
            const quint32 value = readBe16(data, table + i * 2);
            if (value == 0)
                continue;
            const qint64 target = base + value;
            if (target < qint64(dataStart) || target > qint64(data.size()) - 6)
                continue;
            offsets[int(i)] = quint32(target);
            ++populated;
        }

        QVector<quint32> records;
        records.reserve(int(populated));
        for (quint32 target : offsets)
            if (target != std::numeric_limits<quint32>::max())
                records.append(target);
        std::sort(records.begin(), records.end());
        records.erase(std::unique(records.begin(), records.end()), records.end());

        QVector<quint32> bitmapSizes(int(glyphCount), 0);
        quint32 score = 0;
        for (quint32 i = 0; i < glyphCount; ++i) {
            const quint32 record = offsets.at(int(i));
            if (record == std::numeric_limits<quint32>::max())
                continue;
            const auto recordIt = std::lower_bound(records.cbegin(), records.cend(), record);
            if (recordIt == records.cend() || *recordIt != record)
                continue;
            const quint32 next = (recordIt + 1 == records.cend())
                ? quint32(data.size()) : *(recordIt + 1);
            const quint32 width = byteAt(data, record);
            const quint32 stride = (width + 7) / 8;
            if (next <= record + 6 || width == 0 || width > kMaxGlyphDimension)
                continue;
            const quint32 bitmapSize = next - record - 6;
            if (stride == 0 || bitmapSize < stride || bitmapSize % stride != 0 ||
                bitmapSize / stride > kMaxGlyphDimension)
                continue;
            bitmapSizes[int(i)] = bitmapSize;
            ++score;
        }
        if (score > bestScore) {
            bestScore = score;
            bestOffsets = offsets;
            bestBitmapSizes = bitmapSizes;
        }
    }
    if (bestScore < qMax(8u, glyphCount / 2))
        return false;

    BitmapFont font;
    font.format = Format::Dwf;
    const quint32 nameLength = readLe16(data, 6);
    if (nameLength > 0 && nameLength <= quint32(data.size()) - 0x60)
        font.name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
    if (font.name.isEmpty())
        font.name = QStringLiteral("DECWindows Font");
    font.height = readLe16(data, 0x14);
    font.width = readLe16(data, 0x16);
    if (font.height == 0 || font.height > int(kMaxGlyphDimension) ||
        font.width > int(kMaxGlyphDimension))
        return false;
    font.fixedWidth = true;
    font.glyphs.reserve(int(bestScore));
    int priorAdvance = -1;
    for (quint32 i = 0; i < glyphCount; ++i) {
        const quint32 record = bestOffsets.at(int(i));
        const quint32 bitmapSize = bestBitmapSizes.at(int(i));
        if (record == std::numeric_limits<quint32>::max() || bitmapSize == 0)
            continue;
        const quint32 width = byteAt(data, record);
        const quint32 stride = (width + 7) / 8;
        Glyph glyph;
        glyph.codepoint = i;
        glyph.width = int(width);
        glyph.height = int(bitmapSize / stride);
        glyph.bearingY = byteAt(data, record + 2);
        glyph.advance = byteAt(data, record + 1);
        glyph.bitmap = data.mid(int(record + 6), int(bitmapSize));
        QByteArray mirroredBitmap(glyph.bitmap.size(), '\0');
        for (quint32 y = 0; y < quint32(glyph.height); ++y) {
            for (quint32 x = 0; x < width; ++x) {
                const quint32 sourceOffset = y * stride + x / 8;
                if (!(byteAt(glyph.bitmap, sourceOffset) & (0x80u >> (x & 7))))
                    continue;
                const quint32 targetX = width - x - 1;
                const quint32 targetOffset = y * stride + targetX / 8;
                mirroredBitmap[int(targetOffset)] = char(
                    quint8(mirroredBitmap.at(int(targetOffset))) | (0x80u >> (targetX & 7)));
            }
        }
        glyph.bitmap = mirroredBitmap;
        font.width = qMax(font.width, glyph.width);
        font.ascent = qMax(font.ascent, glyph.bearingY);
        if (priorAdvance >= 0 && priorAdvance != glyph.advance)
            font.fixedWidth = false;
        priorAdvance = glyph.advance;
        font.glyphs.append(glyph);
    }
    *out = font;
    return true;
}

} // namespace

bool hasDwfHeader(const QByteArray& data)
{
    return data.size() >= int(kHeaderSize) && quint8(data.at(0)) == 0x76 &&
           quint8(data.at(1)) == 0x5e && quint8(data.at(2)) == 0xdf &&
           quint8(data.at(3)) == 0x01 && readLe16(data, 4) == kHeaderSize;
}

bool hasTerminalDwfIdentity(const QByteArray& data)
{
    const quint32 nameLength = readLe16(data, 6);
    if (nameLength == 0 || nameLength > quint32(data.size()) - 0x60u)
        return false;
    const QString name = QString::fromLatin1(data.constData() + 0x60, int(nameLength));
    return name.startsWith(QStringLiteral("-DEC-Terminal-")) &&
           readLe32(data, 0x44) == 0 && readLe32(data, 0x4c) == 253;
}

bool parseDwf(const QByteArray& data, BitmapFont* font, QString* error)
{
    if (!font || data.size() < int(kHeaderSize) || quint32(data.size()) > kMaxFontSize ||
        !hasDwfHeader(data)) {
        if (error)
            *error = QStringLiteral("Invalid DECWindows Font header");
        return false;
    }
    bool parsed = parseCompX10(data, font);
    if (!parsed && hasTerminalDwfIdentity(data))
        parsed = parseTerminalDwf(data, font);
    else if (!parsed)
        parsed = parseLegacyDwf(data, font) || parseMitX11(data, font);
    if (parsed) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("Unsupported DECWindows Font layout");
    return false;
}

} // namespace font
} // namespace peare
