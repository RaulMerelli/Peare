#include "BitmapFont.h"

#include <limits>
#include <QHash>
#include <QtEndian>

#include <algorithm>

namespace peare {
namespace font {
namespace {

quint16 be16(const QByteArray& data, int offset)
{
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 be32(const QByteArray& data, int offset)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

bool fits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

struct SfntTable {
    quint32 offset = 0;
    quint32 length = 0;
};

bool readSfntTables(const QByteArray& data, QHash<QByteArray, SfntTable>* tables)
{
    if (data.size() < 12 || be32(data, 0) != 0x00010000u)
        return false;
    const quint16 count = be16(data, 4);
    if (!count || count > 256 || 12LL + qint64(count) * 16 > data.size())
        return false;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 12 + i * 16;
        const QByteArray tag = data.mid(record, 4);
        SfntTable table;
        table.offset = be32(data, record + 8);
        table.length = be32(data, record + 12);
        if (tables->contains(tag) || !fits(table.offset, table.length, quint64(data.size())))
            return false;
        tables->insert(tag, table);
    }
    return true;
}

QVector<quint32> unicodeByGlyph(const QByteArray& cmap, quint16 glyphCount)
{
    QVector<quint32> result(glyphCount, 0xffffffffu);
    if (cmap.size() < 4) return result;
    const quint16 count = be16(cmap, 2);
    if (!count || count > 256 || 4LL + qint64(count) * 8 > cmap.size()) return result;
    for (quint16 i = 0; i < count; ++i) {
        const int record = 4 + i * 8;
        const quint16 platform = be16(cmap, record);
        const quint16 encoding = be16(cmap, record + 2);
        if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10)))
            continue;
        const quint32 offset = be32(cmap, record + 4);
        if (!fits(offset, 2, quint64(cmap.size()))) continue;
        const quint16 format = be16(cmap, int(offset));
        if (format == 4) {
            if (!fits(offset, 8, quint64(cmap.size()))) continue;
            const quint16 length = be16(cmap, int(offset) + 2);
            const quint16 segments = be16(cmap, int(offset) + 6) / 2;
            if (!segments || !fits(offset, length, quint64(cmap.size())) ||
                16LL + qint64(segments) * 8 > length) continue;
            const int base = int(offset);
            const int endCodes = base + 14;
            const int startCodes = endCodes + segments * 2 + 2;
            const int deltas = startCodes + segments * 2;
            const int rangeOffsets = deltas + segments * 2;
            const int tableEnd = base + length;
            for (quint16 segment = 0; segment < segments; ++segment) {
                const quint16 first = be16(cmap, startCodes + segment * 2);
                const quint16 last = be16(cmap, endCodes + segment * 2);
                const qint16 delta = qint16(be16(cmap, deltas + segment * 2));
                const int rangePosition = rangeOffsets + segment * 2;
                const quint16 rangeOffset = be16(cmap, rangePosition);
                if (last < first) continue;
                for (quint32 code = first; code <= last && code != 0xffffu; ++code) {
                    quint32 glyph = 0;
                    if (!rangeOffset) {
                        glyph = quint16(code + delta);
                    } else {
                        const int glyphPosition = rangePosition + rangeOffset +
                                                  int((code - first) * 2);
                        if (!fits(quint64(glyphPosition), 2, quint64(tableEnd))) break;
                        glyph = be16(cmap, glyphPosition);
                        if (glyph) glyph = quint16(glyph + delta);
                    }
                    if (glyph && glyph < glyphCount && result.at(int(glyph)) == 0xffffffffu)
                        result[int(glyph)] = code;
                }
            }
        } else if (format == 12) {
            if (!fits(offset, 16, quint64(cmap.size()))) continue;
            const quint32 length = be32(cmap, int(offset) + 4);
            const quint32 groups = be32(cmap, int(offset) + 12);
            if (groups > 100000 || !fits(offset, length, quint64(cmap.size())) ||
                16LL + qint64(groups) * 12 > length) continue;
            for (quint32 group = 0; group < groups; ++group) {
                const int entry = int(offset) + 16 + int(group * 12);
                const quint32 first = be32(cmap, entry);
                const quint32 last = be32(cmap, entry + 4);
                const quint32 firstGlyph = be32(cmap, entry + 8);
                if (last < first || last > 0x10ffffu) continue;
                for (quint32 code = first; code <= last; ++code) {
                    const quint64 glyph = quint64(firstGlyph) + code - first;
                    if (glyph && glyph < glyphCount && result[int(glyph)] == 0xffffffffu)
                        result[int(glyph)] = code;
                }
            }
        }
    }
    return result;
}

bool readSmallMetrics(const QByteArray& data, int offset, Glyph* glyph)
{
    if (!fits(quint64(offset), 5, quint64(data.size()))) return false;
    glyph->height = quint8(data.at(offset));
    glyph->width = quint8(data.at(offset + 1));
    glyph->bearingX = qint8(data.at(offset + 2));
    glyph->bearingY = qint8(data.at(offset + 3));
    glyph->advance = quint8(data.at(offset + 4));
    return true;
}

bool readBigMetrics(const QByteArray& data, int offset, Glyph* glyph)
{
    if (!fits(quint64(offset), 8, quint64(data.size()))) return false;
    glyph->height = quint8(data.at(offset));
    glyph->width = quint8(data.at(offset + 1));
    glyph->bearingX = qint8(data.at(offset + 2));
    glyph->bearingY = qint8(data.at(offset + 3));
    glyph->advance = quint8(data.at(offset + 4));
    return true;
}

bool appendBitmapGlyph(const QByteArray& bdat, quint32 imageOffset, quint32 imageLength,
                       quint16 imageFormat, const Glyph* sharedMetrics,
                       Glyph* glyph)
{
    int pixelOffset = int(imageOffset);
    if (imageFormat == 1 || imageFormat == 2) {
        if (!readSmallMetrics(bdat, pixelOffset, glyph)) return false;
        pixelOffset += 5;
    } else if (imageFormat == 6 || imageFormat == 7) {
        if (!readBigMetrics(bdat, pixelOffset, glyph)) return false;
        pixelOffset += 8;
    } else if (imageFormat == 5 && sharedMetrics) {
        glyph->width = sharedMetrics->width;
        glyph->height = sharedMetrics->height;
        glyph->bearingX = sharedMetrics->bearingX;
        glyph->bearingY = sharedMetrics->bearingY;
        glyph->advance = sharedMetrics->advance;
    } else {
        return false;
    }
    if (glyph->width > 4096 || glyph->height > 4096 ||
        quint64(glyph->width) * quint64(glyph->height) > 16u * 1024u * 1024u)
        return false;
    const quint32 rowBytes = (quint32(glyph->width) + 7u) / 8u;
    const bool byteAligned = imageFormat == 1 || imageFormat == 6;
    const quint64 sourceSize = byteAligned
        ? quint64(rowBytes) * quint32(glyph->height)
        : (quint64(glyph->width) * quint32(glyph->height) + 7u) / 8u;
    if (quint64(pixelOffset) + sourceSize > quint64(imageOffset) + imageLength ||
        !fits(quint64(pixelOffset), sourceSize, quint64(bdat.size())))
        return false;
    glyph->bitmap = QByteArray(int(quint64(rowBytes) * quint32(glyph->height)), '\0');
    for (int y = 0; y < glyph->height; ++y) {
        for (int x = 0; x < glyph->width; ++x) {
            const quint64 sourceBit = byteAligned
                ? quint64(y) * rowBytes * 8u + quint32(x)
                : quint64(y) * quint32(glyph->width) + quint32(x);
            const quint8 source = quint8(bdat.at(pixelOffset + int(sourceBit / 8u)));
            if (source & (0x80u >> (sourceBit & 7u))) {
                const int target = y * int(rowBytes) + x / 8;
                glyph->bitmap[target] = char(quint8(glyph->bitmap.at(target)) |
                                             (0x80u >> (x & 7)));
            }
        }
    }
    return true;
}

} // namespace

bool parseMacBitmapSfnt(const QByteArray& data, BitmapFont* output)
{
    if (!output) return false;
    QHash<QByteArray, SfntTable> tables;
    if (!readSfntTables(data, &tables)) return false;
    const SfntTable maxpTable = tables.value(QByteArrayLiteral("maxp"));
    const SfntTable cmapTable = tables.value(QByteArrayLiteral("cmap"));
    const SfntTable blocTable = tables.value(QByteArrayLiteral("bloc"));
    const SfntTable bdatTable = tables.value(QByteArrayLiteral("bdat"));
    if (maxpTable.length < 6 || cmapTable.length < 4 ||
        blocTable.length < 8 || bdatTable.length < 4)
        return false;
    const quint16 glyphCount = be16(data, int(maxpTable.offset) + 4);
    const QByteArray cmap = data.mid(int(cmapTable.offset), int(cmapTable.length));
    const QByteArray bloc = data.mid(int(blocTable.offset), int(blocTable.length));
    const QByteArray bdat = data.mid(int(bdatTable.offset), int(bdatTable.length));
    if (!glyphCount || be32(bloc, 0) != 0x00020000u || be32(bdat, 0) != 0x00020000u)
        return false;
    const quint32 strikeCount = be32(bloc, 4);
    if (!strikeCount || strikeCount > 128 || 8ULL + quint64(strikeCount) * 48 > quint64(bloc.size()))
        return false;

    BitmapFont parsed;
    parsed.format = Format::MacNfnt;
    parsed.name = QStringLiteral("Apple bitmap SFNT");
    parsed.glyphs.resize(glyphCount);
    const QVector<quint32> codepoints = unicodeByGlyph(cmap, glyphCount);
    bool decodedStrike = false;
    int bestPpem = -1;
    for (quint32 strikeIndex = 0; strikeIndex < strikeCount; ++strikeIndex) {
        const int sizeOffset = 8 + int(strikeIndex * 48);
        const quint32 arrayOffset = be32(bloc, sizeOffset);
        const quint32 arraySize = be32(bloc, sizeOffset + 4);
        const quint32 subtableCount = be32(bloc, sizeOffset + 8);
        const quint32 colorRef = be32(bloc, sizeOffset + 12);
        const quint16 firstGlyph = be16(bloc, sizeOffset + 40);
        const quint16 lastGlyph = be16(bloc, sizeOffset + 42);
        const quint8 ppem = quint8(bloc.at(sizeOffset + 45));
        const quint8 bitDepth = quint8(bloc.at(sizeOffset + 46));
        const quint8 flags = quint8(bloc.at(sizeOffset + 47));
        if (!subtableCount || subtableCount > 65536 || colorRef || bitDepth != 1 ||
            (flags & 2u) || (flags & ~3u) || firstGlyph > lastGlyph || lastGlyph >= glyphCount ||
            !fits(arrayOffset, quint64(subtableCount) * 8, quint64(bloc.size())) ||
            arraySize > quint32(bloc.size()) - arrayOffset)
            continue;
        QVector<Glyph> strikeGlyphs(glyphCount);
        bool strikeValid = true;
        for (quint32 subIndex = 0; subIndex < subtableCount && strikeValid; ++subIndex) {
            const int entry = int(arrayOffset + subIndex * 8);
            const quint16 first = be16(bloc, entry);
            const quint16 last = be16(bloc, entry + 2);
            const quint32 relative = be32(bloc, entry + 4);
            const quint64 subOffset = quint64(arrayOffset) + relative;
            if (first > last || first < firstGlyph || last > lastGlyph ||
                !fits(subOffset, 8, quint64(bloc.size()))) {
                strikeValid = false;
                break;
            }
            const quint16 indexFormat = be16(bloc, int(subOffset));
            const quint16 imageFormat = be16(bloc, int(subOffset) + 2);
            const quint32 imageDataOffset = be32(bloc, int(subOffset) + 4);
            QVector<quint32> offsets;
            Glyph sharedMetrics;
            if (indexFormat == 1 || indexFormat == 3) {
                const quint32 entryBytes = indexFormat == 1 ? 4u : 2u;
                const quint64 offsetBytes = quint64(last - first + 2) * entryBytes;
                if (!fits(subOffset + 8, offsetBytes, quint64(bloc.size()))) {
                    strikeValid = false;
                    break;
                }
                for (quint32 i = 0; i <= quint32(last - first + 1); ++i)
                    offsets.push_back(indexFormat == 1
                        ? be32(bloc, int(subOffset) + 8 + int(i * 4))
                        : be16(bloc, int(subOffset) + 8 + int(i * 2)));
            } else if (indexFormat == 2 && imageFormat == 5) {
                if (!fits(subOffset + 8, 12, quint64(bloc.size()))) {
                    strikeValid = false;
                    break;
                }
                const quint32 imageSize = be32(bloc, int(subOffset) + 8);
                if (!readBigMetrics(bloc, int(subOffset) + 12, &sharedMetrics) || !imageSize) {
                    strikeValid = false;
                    break;
                }
                for (quint32 i = 0; i <= quint32(last - first + 1); ++i)
                    offsets.push_back(i * imageSize);
            } else {
                strikeValid = false;
                break;
            }
            if (offsets.size() != int(last - first + 2)) {
                strikeValid = false;
                break;
            }
            for (quint32 glyphIndex = first; glyphIndex <= last; ++glyphIndex) {
                const quint32 local = glyphIndex - first;
                if (offsets.at(int(local)) > offsets.at(int(local + 1))) {
                    strikeValid = false;
                    break;
                }
                Glyph glyph;
                const quint32 length = offsets.at(int(local + 1)) - offsets.at(int(local));
                if (!appendBitmapGlyph(bdat, imageDataOffset + offsets.at(int(local)), length,
                                       imageFormat, indexFormat == 2 ? &sharedMetrics : nullptr,
                                       &glyph)) {
                    strikeValid = false;
                    break;
                }
                glyph.codepoint = codepoints.at(int(glyphIndex));
                strikeGlyphs[int(glyphIndex)] = glyph;
            }
        }
        if (strikeValid && int(ppem) > bestPpem) {
            for (int i = 0; i < strikeGlyphs.size(); ++i)
                if (strikeGlyphs.at(i).width || strikeGlyphs.at(i).height)
                    parsed.glyphs[i] = strikeGlyphs.at(i);
            bestPpem = ppem;
            decodedStrike = true;
        }
    }
    if (!decodedStrike) return false;
    for (const Glyph& glyph : parsed.glyphs) {
        parsed.width = qMax(parsed.width, glyph.width);
        parsed.height = qMax(parsed.height, glyph.height);
        parsed.ascent = qMax(parsed.ascent, glyph.bearingY);
        parsed.descent = qMax(parsed.descent, glyph.height - glyph.bearingY);
    }
    parsed.fixedWidth = true;
    *output = parsed;
    return true;
}

} // namespace font
} // namespace peare
