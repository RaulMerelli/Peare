#include "SfntFont.h"
#include "EotFont.h"

#include <limits>
#include <QHash>
#include <QTextCodec>
#include <QtEndian>

#include <woff2/decode.h>

#include <algorithm>
#include <vector>

namespace peare {
namespace font {
namespace {

const int kMaxFontBytes = 64 * 1024 * 1024;
const quint32 kMaxTables = 256;

quint16 be16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset)) << 8) | quint16(quint8(data.at(offset + 1)));
}

quint32 be32(const QByteArray& data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24) |
           (quint32(quint8(data.at(offset + 1))) << 16) |
           (quint32(quint8(data.at(offset + 2))) << 8) | quint32(quint8(data.at(offset + 3)));
}

void putBe16(QByteArray* data, quint16 value)
{
    data->append(char(value >> 8));
    data->append(char(value));
}

void putBe32(QByteArray* data, quint32 value)
{
    data->append(char(value >> 24));
    data->append(char(value >> 16));
    data->append(char(value >> 8));
    data->append(char(value));
}

quint32 checksum(const QByteArray& data)
{
    quint32 sum = 0;
    for (int offset = 0; offset < data.size(); offset += 4) {
        quint32 word = 0;
        for (int i = 0; i < 4 && offset + i < data.size(); ++i)
            word |= quint32(quint8(data.at(offset + i))) << (24 - i * 8);
        sum += word;
    }
    return sum;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

struct Table {
    quint32 checksum = 0;
    quint32 offset = 0;
    quint32 length = 0;
    QByteArray bytes;
};

QString decodeName(const QByteArray& bytes, quint16 platform)
{
    if (platform == 0 || platform == 3) {
        if (bytes.size() & 1) return {};
        QString result;
        result.reserve(bytes.size() / 2);
        for (int i = 0; i < bytes.size(); i += 2)
            result.append(QChar(be16(bytes, i)));
        return result.trimmed();
    }
    return QString::fromLatin1(bytes).trimmed();
}

bool validCmapSubtable(const QByteArray& cmap, quint32 offset, quint16 format)
{
    if (offset > quint32(cmap.size())) return false;
    const quint32 remaining = quint32(cmap.size()) - offset;
    const int base = int(offset);
    if (format == 4) {
        if (remaining < 4) return false;
        const quint16 length = be16(cmap, base + 2);
        if (length < 16 || length > remaining) return false;
        const quint16 segmentCountX2 = be16(cmap, base + 6);
        return segmentCountX2 && !(segmentCountX2 & 1) &&
               16u + quint32(segmentCountX2) * 4u <= length;
    }
    if (format == 6) {
        if (remaining < 10) return false;
        const quint16 length = be16(cmap, base + 2);
        const quint16 entries = be16(cmap, base + 8);
        return length >= 10u + quint32(entries) * 2u && length <= remaining;
    }
    if (format == 12) {
        if (remaining < 16) return false;
        const quint32 length = be32(cmap, base + 4);
        const quint32 groups = be32(cmap, base + 12);
        return groups <= 100000 && qint64(16) + qint64(groups) * 12 <= length &&
               length <= remaining;
    }
    return false;
}

bool validCmap(const QByteArray& cmap)
{
    if (cmap.size() < 4) return false;
    const quint16 count = be16(cmap, 2);
    if (count == 0 || count > 256 || 4 + qint64(count) * 8 > cmap.size()) return false;
    bool supported = false;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 4 + i * 8;
        const quint32 offset = be32(cmap, record + 4);
        if (offset > quint32(cmap.size()) || quint32(cmap.size()) - offset < 2) continue;
        const quint16 format = be16(cmap, int(offset));
        supported |= validCmapSubtable(cmap, offset, format);
    }
    return supported;
}

QVector<quint32> unicodeCodepointsByGlyph(const QByteArray& cmap, quint16 glyphCount)
{
    QHash<quint16, quint32> byGlyph;
    const quint16 count = be16(cmap, 2);
    for (quint16 recordIndex = 0; recordIndex < count; ++recordIndex) {
        const int record = 4 + recordIndex * 8;
        const quint16 platform = be16(cmap, record);
        const quint16 encoding = be16(cmap, record + 2);
        const bool unicodeMap = platform == 0 ||
            (platform == 3 && (encoding == 1 || encoding == 10));
        const bool macRomanMap = platform == 1 && encoding == 0;
        if (!unicodeMap && !macRomanMap) continue;
        const quint32 offset = be32(cmap, record + 4);
        if (offset > quint32(cmap.size()) || quint32(cmap.size()) - offset < 2) continue;
        const int base = int(offset);
        const quint16 format = be16(cmap, base);
        if (!validCmapSubtable(cmap, offset, format)) continue;
        if (format == 12) {
            const quint32 groups = be32(cmap, base + 12);
            for (quint32 group = 0; group < groups; ++group) {
                const int item = base + 16 + int(group * 12);
                const quint32 first = be32(cmap, item);
                const quint32 last = be32(cmap, item + 4);
                const quint32 firstGlyph = be32(cmap, item + 8);
                if (last < first) continue;
                for (quint32 codepoint = first; codepoint <= last && codepoint <= 0x10ffffu;
                     ++codepoint) {
                    if (codepoint >= 0xd800u && codepoint <= 0xdfffu) continue;
                    const quint64 glyph = quint64(firstGlyph) + codepoint - first;
                    if (glyph > 0 && glyph < glyphCount && !byGlyph.contains(quint16(glyph)))
                        byGlyph.insert(quint16(glyph), codepoint);
                    if (byGlyph.size() >= glyphCount - 1) break;
                }
                if (byGlyph.size() >= glyphCount - 1) break;
            }
        } else if (format == 4) {
            const quint16 segmentCount = be16(cmap, base + 6) / 2;
            const int endCodes = base + 14;
            const int startCodes = endCodes + segmentCount * 2 + 2;
            const int deltas = startCodes + segmentCount * 2;
            const int rangeOffsets = deltas + segmentCount * 2;
            const int tableEnd = base + be16(cmap, base + 2);
            for (quint16 segment = 0; segment < segmentCount; ++segment) {
                const quint16 first = be16(cmap, startCodes + segment * 2);
                const quint16 last = be16(cmap, endCodes + segment * 2);
                if (last < first) continue;
                const qint16 delta = qint16(be16(cmap, deltas + segment * 2));
                const int rangePosition = rangeOffsets + segment * 2;
                const quint16 rangeOffset = be16(cmap, rangePosition);
                for (quint32 codepoint = first; codepoint <= last && codepoint != 0xffffu;
                     ++codepoint) {
                    quint32 glyph = 0;
                    if (rangeOffset == 0) {
                        glyph = quint16(codepoint + delta);
                    } else {
                        const int glyphPosition = rangePosition + rangeOffset +
                                                  int((codepoint - first) * 2);
                        if (glyphPosition + 2 > tableEnd) break;
                        glyph = be16(cmap, glyphPosition);
                        if (glyph) glyph = quint16(glyph + delta);
                    }
                    if (glyph > 0 && glyph < glyphCount && !byGlyph.contains(quint16(glyph)))
                        byGlyph.insert(quint16(glyph), codepoint);
                    if (byGlyph.size() >= glyphCount - 1) break;
                }
                if (byGlyph.size() >= glyphCount - 1) break;
            }
        } else if (format == 6) {
            const quint16 firstCode = be16(cmap, base + 6);
            const quint16 entries = be16(cmap, base + 8);
            QTextCodec* macRoman = macRomanMap
                ? QTextCodec::codecForName("macintosh") : nullptr;
            for (quint32 index = 0; index < entries; ++index) {
                const quint16 glyph = be16(cmap, base + 10 + int(index * 2));
                const quint32 character = quint32(firstCode) + index;
                if (glyph == 0 || glyph >= glyphCount || byGlyph.contains(glyph)) continue;
                if (macRomanMap) {
                    if (!macRoman || character > 0xff) continue;
                    const char byte = char(character);
                    const QVector<uint> scalars = macRoman->toUnicode(&byte, 1).toUcs4();
                    if (scalars.size() != 1 ||
                        (scalars.first() >= 0xd800u && scalars.first() <= 0xdfffu))
                        continue;
                    byGlyph.insert(glyph, scalars.first());
                } else {
                    byGlyph.insert(glyph, character);
                }
            }
        }
    }
    QVector<quint32> result;
    result.reserve(byGlyph.size());
    for (quint32 codepoint : byGlyph) result.push_back(codepoint);
    std::sort(result.begin(), result.end());
    return result;
}

bool readNameTable(const QByteArray& table, QString* family, QString* style)
{
    if (table.size() < 6) return false;
    const quint16 count = be16(table, 2);
    const quint16 storage = be16(table, 4);
    if (count > 4096 || 6 + qint64(count) * 12 > table.size() || storage > table.size()) return false;
    QString preferredFamily;
    QString fallbackFamily;
    QString preferredStyle;
    QString fallbackStyle;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 6 + i * 12;
        const quint16 platform = be16(table, record);
        const quint16 nameId = be16(table, record + 6);
        const quint16 length = be16(table, record + 8);
        const quint16 offset = be16(table, record + 10);
        if (offset > quint32(table.size() - storage) || length > table.size() - storage - offset)
            return false;
        const QString value = decodeName(table.mid(storage + offset, length), platform);
        if (nameId == 1 || nameId == 16) {
            if (nameId == 16 && !value.isEmpty()) preferredFamily = value;
            else if (fallbackFamily.isEmpty() && !value.isEmpty()) fallbackFamily = value;
        } else if (nameId == 2 || nameId == 17) {
            if (nameId == 17 && !value.isEmpty()) preferredStyle = value;
            else if (fallbackStyle.isEmpty() && !value.isEmpty()) fallbackStyle = value;
        }
    }
    *family = preferredFamily.isEmpty() ? fallbackFamily : preferredFamily;
    *style = preferredStyle.isEmpty() ? fallbackStyle : preferredStyle;
    return true;
}

bool normalizeEotCffFlavor(QByteArray* data)
{
    if (!data || data->size() < 12 || be32(*data, 0) != 0x00010000u)
        return false;
    const quint16 count = be16(*data, 4);
    if (!count || count > kMaxTables || 12 + qint64(count) * 16 > data->size())
        return false;
    bool hasCff = false;
    bool hasGlyfOrLoca = false;
    for (quint16 i = 0; i < count; ++i) {
        const QByteArray tag = data->mid(12 + i * 16, 4);
        hasCff = hasCff || tag == QByteArrayLiteral("CFF ") ||
            tag == QByteArrayLiteral("CFF2");
        hasGlyfOrLoca = hasGlyfOrLoca || tag == QByteArrayLiteral("glyf") ||
            tag == QByteArrayLiteral("loca");
    }
    if (!hasCff || hasGlyfOrLoca)
        return false;
    (*data)[0] = 'O';
    (*data)[1] = 'T';
    (*data)[2] = 'T';
    (*data)[3] = 'O';
    return true;
}

bool validateSfnt(const QByteArray& data, SfntFont* out, QString* error)
{
    if (data.size() < 12 || data.size() > kMaxFontBytes)
        return fail(error, QStringLiteral("Invalid SFNT size"));
    SfntFormat format;
    if (!identifySfnt(data, &format) || format == SfntFormat::Woff)
        return fail(error, QStringLiteral("Unsupported SFNT signature"));
    const quint32 flavor = be32(data, 0);
    const quint16 count = be16(data, 4);
    if (count == 0 || count > kMaxTables || 12 + qint64(count) * 16 > data.size())
        return fail(error, QStringLiteral("Invalid SFNT table directory"));
    QHash<QByteArray, Table> tables;
    QVector<QPair<quint32, quint32>> ranges;
    const quint32 directoryEnd = 12 + quint32(count) * 16;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 12 + i * 16;
        const QByteArray tag = data.mid(record, 4);
        Table table;
        table.checksum = be32(data, record + 4);
        table.offset = be32(data, record + 8);
        table.length = be32(data, record + 12);
        if (tag.size() != 4 || tables.contains(tag) ||
            (table.length && (table.offset < directoryEnd || (table.offset & 3u))) ||
            table.offset > quint32(data.size()) || table.length > quint32(data.size()) - table.offset)
            return fail(error, QStringLiteral("Invalid SFNT table bounds"));
        table.bytes = data.mid(int(table.offset), int(table.length));
        if (table.length) ranges.push_back(qMakePair(table.offset, table.offset + table.length));
        tables.insert(tag, table);
    }
    std::sort(ranges.begin(), ranges.end(), [](const QPair<quint32, quint32>& a,
                                               const QPair<quint32, quint32>& b) {
        return a.first < b.first;
    });
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges.at(i).first < ranges.at(i - 1).second)
            return fail(error, QStringLiteral("Overlapping SFNT tables"));

    const Table head = tables.value(QByteArrayLiteral("head"));
    const Table maxp = tables.value(QByteArrayLiteral("maxp"));
    const Table hhea = tables.value(QByteArrayLiteral("hhea"));
    const Table hmtx = tables.value(QByteArrayLiteral("hmtx"));
    const Table cmap = tables.value(QByteArrayLiteral("cmap"));
    if (head.length < 54 || be32(head.bytes, 12) != 0x5f0f3cf5u ||
        maxp.length < 6 || hhea.length < 36 || cmap.length < 4)
        return fail(error, QStringLiteral("Missing or invalid required SFNT tables"));
    const quint16 unitsPerEm = be16(head.bytes, 18);
    const quint16 glyphCount = be16(maxp.bytes, 4);
    const quint16 horizontalMetrics = be16(hhea.bytes, 34);
    bool hasTrueTypeOutlines = false;
    if (unitsPerEm < 16 || unitsPerEm > 16384 || glyphCount == 0 ||
        horizontalMetrics == 0 || horizontalMetrics > glyphCount ||
        4LL * horizontalMetrics + 2LL * (glyphCount - horizontalMetrics) > hmtx.length ||
        !validCmap(cmap.bytes))
        return fail(error, QStringLiteral("Invalid SFNT metrics or character map"));
    quint64 advanceTotal = 0;
    quint16 maximumAdvance = 0;
    quint16 firstAdvance = 0;
    bool fixedWidth = true;
    for (quint32 i = 0; i < glyphCount; ++i) {
        const quint16 advance = be16(hmtx.bytes, int(qMin<quint32>(i, horizontalMetrics - 1) * 4));
        if (i == 0) firstAdvance = advance;
        else if (advance != firstAdvance) fixedWidth = false;
        advanceTotal += advance;
        maximumAdvance = qMax(maximumAdvance, advance);
    }

    if (flavor == 0x00010000u || flavor == 0x74727565u) {
        const Table loca = tables.value(QByteArrayLiteral("loca"));
        const Table glyf = tables.value(QByteArrayLiteral("glyf"));
        const bool bitmapOnly = !glyf.length &&
            tables.value(QByteArrayLiteral("EBDT")).length > 0 &&
            tables.value(QByteArrayLiteral("EBLC")).length > 0;
        if (!loca.length || (!glyf.length && !bitmapOnly) ||
            be32(maxp.bytes, 0) != 0x00010000u) {
            return fail(error, QStringLiteral("TrueType outline tables are missing"));
        }
        const qint16 locaFormat = qint16(be16(head.bytes, 50));
        if (locaFormat != 0 && locaFormat != 1)
            return fail(error, QStringLiteral("Unsupported TrueType location format"));
        const quint32 required = quint32(glyphCount + 1) * (locaFormat == 0 ? 2u : 4u);
        if (loca.length < required) return fail(error, QStringLiteral("Truncated TrueType location table"));
        quint32 previous = 0;
        for (quint32 i = 0; i <= glyphCount; ++i) {
            const quint32 offset = locaFormat == 0 ? quint32(be16(loca.bytes, int(i * 2))) * 2u
                                                   : be32(loca.bytes, int(i * 4));
            if (offset < previous || offset > glyf.length) {
                return fail(error, QStringLiteral("Invalid TrueType glyph offsets at glyph %1: %2 < %3 or beyond glyf size %4")
                    .arg(i).arg(offset).arg(previous).arg(glyf.length));
            }
            if (i < glyphCount && offset - previous >= 10 &&
                qint16(be16(glyf.bytes, int(previous))) != 0)
                hasTrueTypeOutlines = true;
            previous = offset;
        }
    } else if (flavor == 0x4f54544fu) {
        if (!tables.contains(QByteArrayLiteral("CFF ")) &&
            !tables.contains(QByteArrayLiteral("CFF2")))
            return fail(error, QStringLiteral("OpenType CFF outline table is missing"));
    } else return fail(error, QStringLiteral("Unsupported SFNT flavor"));

    out->format = format;
    out->unitsPerEm = unitsPerEm;
    out->glyphCount = glyphCount;
    out->ascent = qint16(be16(hhea.bytes, 4));
    out->descent = qint16(be16(hhea.bytes, 6));
    out->averageAdvance = quint16(advanceTotal / glyphCount);
    out->maximumAdvance = maximumAdvance;
    out->fixedWidth = fixedWidth;
    out->hasCff2 = tables.contains(QByteArrayLiteral("CFF2"));
    out->hasTrueTypeOutlines = hasTrueTypeOutlines;
    out->characterMap = unicodeCodepointsByGlyph(cmap.bytes, glyphCount);
    out->sfntBytes = data;
    const Table name = tables.value(QByteArrayLiteral("name"));
    if (name.length && !readNameTable(name.bytes, &out->familyName, &out->styleName))
        return fail(error, QStringLiteral("Invalid SFNT name table"));
    return true;
}

bool unwrapWoff(const QByteArray& data, QByteArray* sfnt, QString* error)
{
    if (data.size() < 44 || be32(data, 8) != quint32(data.size()) || be16(data, 14) != 0)
        return fail(error, QStringLiteral("Invalid WOFF header"));
    const quint32 flavor = be32(data, 4);
    const quint16 count = be16(data, 12);
    const quint32 expectedSize = be32(data, 16);
    if (count == 0 || count > kMaxTables || 44 + qint64(count) * 20 > data.size() ||
        expectedSize > kMaxFontBytes)
        return fail(error, QStringLiteral("Invalid WOFF table directory"));

    struct WoffTable { QByteArray tag; quint32 checksum; quint32 offset; quint32 compLength; quint32 length; };
    QVector<WoffTable> entries;
    QVector<QPair<quint32, quint32>> ranges;
    const quint32 dataStart = 44 + quint32(count) * 20;
    QByteArray previousTag;
    quint64 computedSize = 12 + quint64(count) * 16;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 44 + i * 20;
        WoffTable table;
        table.tag = data.mid(record, 4);
        table.offset = be32(data, record + 4);
        table.compLength = be32(data, record + 8);
        table.length = be32(data, record + 12);
        table.checksum = be32(data, record + 16);
        if (table.tag.size() != 4 || (i && table.tag <= previousTag) ||
            table.offset < dataStart || (table.offset & 3) || table.compLength == 0 ||
            table.compLength > table.length || table.offset > quint32(data.size()) ||
            table.compLength > quint32(data.size()) - table.offset || table.length > kMaxFontBytes)
            return fail(error, QStringLiteral("Invalid WOFF table bounds"));
        previousTag = table.tag;
        entries.push_back(table);
        ranges.push_back(qMakePair(table.offset, table.offset + table.compLength));
        computedSize += (quint64(table.length) + 3u) & ~quint64(3u);
    }
    std::sort(ranges.begin(), ranges.end(), [](const QPair<quint32, quint32>& a,
                                               const QPair<quint32, quint32>& b) {
        return a.first < b.first;
    });
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges.at(i).first < ranges.at(i - 1).second)
            return fail(error, QStringLiteral("Overlapping WOFF table data"));
    if (computedSize != expectedSize) return fail(error, QStringLiteral("WOFF SFNT size mismatch"));

    const quint32 metaOffset = be32(data, 24);
    const quint32 metaLength = be32(data, 28);
    const quint32 metaOriginal = be32(data, 32);
    const quint32 privateOffset = be32(data, 36);
    const quint32 privateLength = be32(data, 40);
    if ((metaOffset == 0) != (metaLength == 0) || (metaOffset == 0) != (metaOriginal == 0) ||
        (privateOffset == 0) != (privateLength == 0) ||
        (metaLength && (metaOffset < dataStart || metaOffset > quint32(data.size()) ||
                        metaLength > quint32(data.size()) - metaOffset)) ||
        (privateLength && (privateOffset < dataStart || privateOffset > quint32(data.size()) ||
                           privateLength > quint32(data.size()) - privateOffset)))
        return fail(error, QStringLiteral("Invalid WOFF auxiliary data bounds"));
    for (const QPair<quint32, quint32>& range : ranges) {
        if ((metaLength && metaOffset < range.second && range.first < metaOffset + metaLength) ||
            (privateLength && privateOffset < range.second && range.first < privateOffset + privateLength))
            return fail(error, QStringLiteral("WOFF auxiliary data overlaps a font table"));
    }

    QVector<QByteArray> tableBytes;
    for (const WoffTable& table : entries) {
        QByteArray bytes;
        const QByteArray source = data.mid(int(table.offset), int(table.compLength));
        if (table.compLength == table.length) bytes = source;
        else {
            QByteArray prefixed;
            putBe32(&prefixed, table.length);
            prefixed.append(source);
            bytes = qUncompress(prefixed);
        }
        if (quint32(bytes.size()) != table.length)
            return fail(error, QStringLiteral("WOFF table decompression failed"));
        if (table.tag == QByteArrayLiteral("head")) {
            if (bytes.size() < 12) return fail(error, QStringLiteral("Truncated WOFF head table"));
            bytes[8] = bytes[9] = bytes[10] = bytes[11] = 0;
        }
        if (checksum(bytes) != table.checksum)
            return fail(error, QStringLiteral("WOFF table checksum mismatch"));
        tableBytes.push_back(bytes);
    }

    QByteArray result;
    result.reserve(int(computedSize));
    putBe32(&result, flavor);
    putBe16(&result, count);
    quint16 power = 1;
    quint16 selector = 0;
    while (quint32(power) * 2 <= count) { power = quint16(power * 2); ++selector; }
    putBe16(&result, quint16(power * 16));
    putBe16(&result, selector);
    putBe16(&result, quint16(count * 16 - power * 16));
    quint32 tableOffset = 12 + quint32(count) * 16;
    int headOffset = -1;
    for (int i = 0; i < entries.size(); ++i) {
        const WoffTable& table = entries.at(i);
        const QByteArray& bytes = tableBytes.at(i);
        putBe32(&result, be32(table.tag, 0));
        putBe32(&result, table.checksum);
        putBe32(&result, tableOffset);
        putBe32(&result, table.length);
        if (table.tag == QByteArrayLiteral("head")) headOffset = int(tableOffset);
        tableOffset += (table.length + 3u) & ~3u;
    }
    for (const QByteArray& bytes : tableBytes) {
        result.append(bytes);
        while (result.size() & 3) result.append('\0');
    }
    if (headOffset < 0 || result.size() != int(expectedSize))
        return fail(error, QStringLiteral("WOFF has no valid head table"));
    const quint32 adjustment = 0xb1b0afbau - checksum(result);
    qToBigEndian<quint32>(adjustment, reinterpret_cast<uchar*>(result.data() + headOffset + 8));
    *sfnt = result;
    return true;
}

bool unwrapWoff2(const QByteArray& data, QByteArray* sfnt, QString* error)
{
    const size_t outputSize = woff2::ComputeWOFF2FinalSize(
        reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()));
    if (outputSize == 0 || outputSize > size_t(kMaxFontBytes))
        return fail(error, QStringLiteral("Invalid or oversized WOFF2 output"));
    QByteArray output(int(outputSize), '\0');
    if (!woff2::ConvertWOFF2ToTTF(reinterpret_cast<uint8_t*>(output.data()), outputSize,
                                  reinterpret_cast<const uint8_t*>(data.constData()),
                                  size_t(data.size())))
        return fail(error, QStringLiteral("WOFF2 decompression failed"));
    *sfnt = output;
    return true;
}

} // namespace

bool identifySfnt(const QByteArray& data, SfntFormat* format)
{
    if (data.size() < 4) return false;
    const quint32 signature = be32(data, 0);
    SfntFormat detected;
    if (signature == 0x00010000u || signature == 0x74727565u) detected = SfntFormat::TrueType;
    else if (signature == 0x4f54544fu) detected = SfntFormat::OpenType;
    else if (signature == 0x774f4646u) detected = SfntFormat::Woff;
    else if (signature == 0x774f4632u) detected = SfntFormat::Woff2;
    else return false;
    if (format) *format = detected;
    return true;
}

bool hasTtcHeader(const QByteArray& data)
{
    if (data.size() < 12 || data.left(4) != QByteArrayLiteral("ttcf")) return false;
    const quint32 version = be32(data, 4);
    const quint32 count = be32(data, 8);
    return (version == 0x00010000u || version == 0x00020000u) &&
        count > 0 && count <= 256 && 12LL + qint64(count) * 4 <= data.size();
}

bool parseTtcCollection(const QByteArray& data, QVector<SfntFont>* fonts, QString* error)
{
    if (!fonts || data.size() < 12 || data.size() > kMaxFontBytes ||
        data.left(4) != QByteArrayLiteral("ttcf"))
        return fail(error, QStringLiteral("Invalid TrueType collection header or size"));
    const quint32 version = be32(data, 4);
    const quint32 count = be32(data, 8);
    const quint64 headerEnd = 12u + quint64(count) * 4u + (version == 0x00020000u ? 12u : 0u);
    if ((version != 0x00010000u && version != 0x00020000u) || count == 0 || count > 256 ||
        headerEnd > quint64(data.size()))
        return fail(error, QStringLiteral("Invalid TrueType collection version or face count"));
    if (version == 0x00020000u) {
        const quint32 dsigTag = be32(data, int(12 + count * 4));
        const quint32 dsigLength = be32(data, int(16 + count * 4));
        const quint32 dsigOffset = be32(data, int(20 + count * 4));
        if ((dsigTag == 0 && (dsigLength || dsigOffset)) ||
            (dsigTag != 0 && (dsigTag != 0x44534947u || dsigLength == 0 ||
             dsigOffset < headerEnd || dsigOffset > quint32(data.size()) ||
             dsigLength > quint32(data.size()) - dsigOffset)))
            return fail(error, QStringLiteral("Invalid TTC digital-signature directory"));
    }

    QVector<quint32> faceOffsets;
    QVector<quint64> directoryEnds;
    faceOffsets.reserve(int(count));
    directoryEnds.reserve(int(count));
    quint64 allDirectoriesEnd = headerEnd;
    for (quint32 face = 0; face < count; ++face) {
        const quint32 faceOffset = be32(data, int(12 + face * 4));
        if (faceOffset < headerEnd || faceOffset > quint32(data.size()) - 12)
            return fail(error, QStringLiteral("Invalid TTC face offset"));
        const quint16 tableCount = be16(data, int(faceOffset + 4));
        const quint64 directoryEnd = quint64(faceOffset) + 12u + quint64(tableCount) * 16u;
        if (tableCount == 0 || tableCount > kMaxTables || directoryEnd > quint64(data.size()))
            return fail(error, QStringLiteral("Invalid TTC face table directory"));
        for (int prior = 0; prior < faceOffsets.size(); ++prior) {
            if (faceOffset == faceOffsets.at(prior) ||
                (quint64(faceOffset) < directoryEnds.at(prior) &&
                 quint64(faceOffsets.at(prior)) < directoryEnd))
                return fail(error, QStringLiteral("Overlapping TTC face directories"));
        }
        faceOffsets.push_back(faceOffset);
        directoryEnds.push_back(directoryEnd);
        allDirectoriesEnd = qMax(allDirectoriesEnd, directoryEnd);
    }

    QVector<SfntFont> parsedFonts;
    parsedFonts.reserve(int(count));
    quint64 totalNormalizedBytes = 0;
    for (quint32 face = 0; face < count; ++face) {
        const quint32 faceOffset = faceOffsets.at(int(face));
        const quint16 tableCount = be16(data, int(faceOffset + 4));
        const quint64 sourceDirectoryEnd = quint64(faceOffset) + 12u + quint64(tableCount) * 16u;

        QByteArray sfnt = data.mid(int(faceOffset), int(12u + quint64(tableCount) * 16u));
        quint32 headOffset = 0;
        for (quint16 index = 0; index < tableCount; ++index) {
            const int record = 12 + index * 16;
            const quint32 sourceOffset = be32(data, int(faceOffset) + record + 8);
            const quint32 length = be32(data, int(faceOffset) + record + 12);
            if (sourceOffset < allDirectoriesEnd || sourceOffset < sourceDirectoryEnd ||
                sourceOffset > quint32(data.size()) ||
                length > quint32(data.size()) - sourceOffset ||
                length > quint32(kMaxFontBytes - sfnt.size()))
                return fail(error, QStringLiteral("TTC table exceeds the collection bounds"));
            while (sfnt.size() & 3) sfnt.append('\0');
            const quint32 normalizedOffset = quint32(sfnt.size());
            if (length) sfnt.append(data.constData() + sourceOffset, int(length));
            if (data.mid(int(faceOffset) + record, 4) == QByteArrayLiteral("head")) {
                if (length < 54) return fail(error, QStringLiteral("Truncated TTC head table"));
                headOffset = normalizedOffset;
                sfnt[record + 4] = data.at(int(faceOffset) + record + 4);
                sfnt[record + 5] = data.at(int(faceOffset) + record + 5);
                sfnt[record + 6] = data.at(int(faceOffset) + record + 6);
                sfnt[record + 7] = data.at(int(faceOffset) + record + 7);
            }
            while (sfnt.size() & 3) sfnt.append('\0');
            const quint32 outputOffset = normalizedOffset;
            sfnt[record + 8] = char(outputOffset >> 24);
            sfnt[record + 9] = char(outputOffset >> 16);
            sfnt[record + 10] = char(outputOffset >> 8);
            sfnt[record + 11] = char(outputOffset);
        }
        if (sfnt.size() > kMaxFontBytes || headOffset == 0)
            return fail(error, QStringLiteral("TTC face has no valid head table"));
        for (int byte = 0; byte < 4; ++byte) sfnt[int(headOffset) + 8 + byte] = 0;
        quint32 checksum = 0;
        for (int offset = 0; offset < sfnt.size(); offset += 4)
            checksum += be32(sfnt, offset);
        const quint32 adjustment = 0xB1B0AFBAu - checksum;
        sfnt[int(headOffset) + 8] = char(adjustment >> 24);
        sfnt[int(headOffset) + 9] = char(adjustment >> 16);
        sfnt[int(headOffset) + 10] = char(adjustment >> 8);
        sfnt[int(headOffset) + 11] = char(adjustment);
        SfntFont parsed;
        if (!validateSfnt(sfnt, &parsed, error)) return false;
        totalNormalizedBytes += quint64(sfnt.size());
        if (totalNormalizedBytes > 256u * 1024u * 1024u)
            return fail(error, QStringLiteral("TTC normalized faces exceed the aggregate size limit"));
        parsedFonts.push_back(parsed);
    }
    *fonts = parsedFonts;
    if (error) error->clear();
    return true;
}

bool parseSfnt(const QByteArray& data, SfntFont* font, QString* error)
{
    if (!font || data.isEmpty() || data.size() > kMaxFontBytes)
        return fail(error, QStringLiteral("Invalid font input size"));
    if (error) error->clear();
    SfntFormat format;
    if (!identifySfnt(data, &format)) {
        EotFont eot;
        if (!parseEot(data, &eot, error))
            return fail(error, QStringLiteral("Unsupported font signature"));
        normalizeEotCffFlavor(&eot.sfntBytes);
        SfntFont parsed;
        QString validationError;
        if (!validateSfnt(eot.sfntBytes, &parsed, &validationError)) {
            if (error) *error = validationError;
            return false;
        }
        parsed.sfntBytes = eot.sfntBytes;
        *font = parsed;
        return true;
    }
    QByteArray sfnt = data;
    if (format == SfntFormat::Woff && !unwrapWoff(data, &sfnt, error)) return false;
    if (format == SfntFormat::Woff2 && !unwrapWoff2(data, &sfnt, error)) return false;
    SfntFont parsed;
    if (!validateSfnt(sfnt, &parsed, error)) return false;
    parsed.format = format;
    *font = parsed;
    return true;
}

} // namespace font
} // namespace peare
