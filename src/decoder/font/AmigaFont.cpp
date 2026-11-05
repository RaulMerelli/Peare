#include "AmigaFont.h"

namespace peare {
namespace font {
namespace {

quint16 readBe16(const QByteArray& data, int offset)
{
    return (quint16(quint8(data.at(offset))) << 8) |
           quint16(quint8(data.at(offset + 1)));
}

quint32 readBe32(const QByteArray& data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24) |
           (quint32(quint8(data.at(offset + 1))) << 16) |
           (quint32(quint8(data.at(offset + 2))) << 8) |
           quint32(quint8(data.at(offset + 3)));
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

} // namespace

bool parseAmigaFontContents(const QByteArray& data, AmigaFontContents* contents,
                            QString* error)
{
    if (!contents || data.size() < 4 || data.size() > 4 * 1024 * 1024)
        return fail(error, QStringLiteral("Invalid Amiga font contents size"));
    const quint16 fileId = readBe16(data, 0);
    if (fileId != 0x0F00 && fileId != 0x0F02 && fileId != 0x0F03)
        return fail(error, QStringLiteral("Invalid Amiga font contents identifier"));
    const quint16 count = readBe16(data, 2);
    if (count > 4096 || 4 + qint64(count) * 260 > data.size())
        return fail(error, QStringLiteral("Invalid Amiga font strike count"));

    AmigaFontContents parsed;
    parsed.fileId = fileId;
    parsed.strikes.reserve(count);
    for (quint16 index = 0; index < count; ++index) {
        const int record = 4 + int(index) * 260;
        const quint16 tagCount = fileId == 0x0F00 ? 0 : readBe16(data, record + 250);
        const int tagsOffset = 256 - int(tagCount) * 8;
        if (tagCount && (tagsOffset < 0 || tagCount > 32))
            return fail(error, QStringLiteral("Invalid Amiga font tag count"));
        const int nameLength = fileId == 0x0F00 ? 256 : tagsOffset;
        const QByteArray name = data.mid(record, nameLength);
        const int terminator = name.indexOf('\0');
        if (terminator <= 0)
            return fail(error, QStringLiteral("Amiga font strike name is empty or unterminated"));
        if (tagCount) {
            for (quint16 tag = 0; tag < tagCount; ++tag) {
                const int tagOffset = record + tagsOffset + int(tag) * 8;
                const quint32 tagId = readBe32(data, tagOffset);
                const quint32 tagData = readBe32(data, tagOffset + 4);
                if ((tag + 1 == tagCount && (tagId != 0 || tagData != tagCount)) ||
                    (tag + 1 != tagCount && tagId == 0))
                    return fail(error, QStringLiteral("Invalid Amiga font tag list"));
            }
        }
        AmigaFontStrike strike;
        strike.fileName = QString::fromLatin1(name.constData(), terminator);
        strike.pixelHeight = readBe16(data, record + 256);
        strike.style = quint8(data.at(record + 258));
        strike.flags = quint8(data.at(record + 259));
        strike.tagCount = tagCount;
        if (strike.pixelHeight == 0)
            return fail(error, QStringLiteral("Invalid Amiga font strike entry"));
        parsed.strikes.append(strike);
    }
    *contents = parsed;
    if (error) error->clear();
    return true;
}

bool parseAmigaFontStrike(const QByteArray& data, BitmapFont* font, QString* error)
{
    const auto invalid = [&](const QString& message) {
        return fail(error, message);
    };
    if (!font || data.size() < 48 || data.size() > 64 * 1024 * 1024 ||
        readBe32(data, 0) != 0x000003F3u)
        return invalid(QStringLiteral("Invalid Amiga font HUNK header"));

    int cursor = 4;
    for (int names = 0; names < 4096; ++names) {
        if (cursor + 4 > data.size())
            return invalid(QStringLiteral("Truncated Amiga HUNK resident-name list"));
        const quint32 words = readBe32(data, cursor);
        cursor += 4;
        if (words == 0) break;
        const quint64 byteCount = quint64(words) * 4;
        if (byteCount > quint64(data.size() - cursor))
            return invalid(QStringLiteral("Invalid Amiga HUNK resident name"));
        cursor += int(byteCount);
        if (names == 4095)
            return invalid(QStringLiteral("Amiga HUNK resident-name list is too large"));
    }
    if (cursor + 16 > data.size())
        return invalid(QStringLiteral("Truncated Amiga HUNK segment table"));
    const quint32 hunkCount = readBe32(data, cursor);
    const quint32 firstHunk = readBe32(data, cursor + 4);
    const quint32 lastHunk = readBe32(data, cursor + 8);
    const quint32 allocationField = readBe32(data, cursor + 12);
    const quint32 allocationWords = allocationField & 0x3FFFFFFFu;
    cursor += 16;
    if (hunkCount != 1 || firstHunk != 0 || lastHunk != 0 || allocationWords == 0 ||
        (allocationField & 0xC0000000u) == 0xC0000000u)
        return invalid(QStringLiteral("Only single-hunk Amiga font strikes are supported"));

    if (cursor + 8 > data.size() || (readBe32(data, cursor) & 0x3FFFFFFFu) != 0x3E9u)
        return invalid(QStringLiteral("Amiga font strike has no HUNK_CODE segment"));
    const quint32 codeWords = readBe32(data, cursor + 4) & 0x3FFFFFFFu;
    cursor += 8;
    const quint64 codeLength64 = quint64(codeWords) * 4;
    if (codeWords == 0 || codeLength64 > quint64(data.size() - cursor) ||
        codeLength64 > quint64(allocationWords) * 4 || codeLength64 < 110)
        return invalid(QStringLiteral("Invalid Amiga font code segment size"));
    const int codeLength = int(codeLength64);
    const QByteArray code = data.mid(cursor, codeLength);
    cursor += codeLength;
    if (quint8(code.at(0)) != 0x70 || quint8(code.at(2)) != 0x4E ||
        quint8(code.at(3)) != 0x75 || readBe16(code, 18) != 0x0F80)
        return invalid(QStringLiteral("Invalid Amiga DiskFontHeader signature"));
    if (quint8(code.at(12)) != 12)
        return invalid(QStringLiteral("Amiga DiskFontHeader has an invalid Node type"));

    bool ended = false;
    while (cursor + 4 <= data.size()) {
        const quint32 tag = readBe32(data, cursor) & 0x3FFFFFFFu;
        cursor += 4;
        if (tag == 0x3F2u) {
            ended = true;
            break;
        }
        if (tag == 0x3ECu) {
            bool terminated = false;
            for (int groups = 0; groups < 65536; ++groups) {
                if (cursor + 4 > data.size())
                    return invalid(QStringLiteral("Truncated Amiga HUNK_RELOC32 record"));
                const quint32 count = readBe32(data, cursor);
                cursor += 4;
                if (count == 0) {
                    terminated = true;
                    break;
                }
                if (cursor + 4 > data.size() || readBe32(data, cursor) != 0)
                    return invalid(QStringLiteral("Amiga font relocation targets another HUNK"));
                cursor += 4;
                if (count > quint32((data.size() - cursor) / 4))
                    return invalid(QStringLiteral("Truncated Amiga HUNK_RELOC32 offsets"));
                for (quint32 item = 0; item < count; ++item) {
                    const quint32 offset = readBe32(data, cursor);
                    cursor += 4;
                    if ((offset & 1u) != 0 || offset > quint32(codeLength - 4))
                        return invalid(QStringLiteral("Invalid Amiga HUNK_RELOC32 offset"));
                }
            }
            if (!terminated)
                return invalid(QStringLiteral("Amiga HUNK_RELOC32 record is not terminated"));
        } else if (tag == 0x3FCu) {
            bool terminated = false;
            quint64 wordCount = 0;
            for (int groups = 0; groups < 65536; ++groups) {
                if (cursor + 2 > data.size())
                    return invalid(QStringLiteral("Truncated Amiga HUNK_RELOC32SHORT record"));
                const quint16 count = readBe16(data, cursor);
                cursor += 2;
                ++wordCount;
                if (count == 0) {
                    terminated = true;
                    break;
                }
                if (cursor + 2 > data.size() || readBe16(data, cursor) != 0)
                    return invalid(QStringLiteral("Amiga short relocation targets another HUNK"));
                cursor += 2;
                ++wordCount;
                if (quint32(count) > quint32((data.size() - cursor) / 2))
                    return invalid(QStringLiteral("Truncated Amiga short relocation offsets"));
                for (quint16 item = 0; item < count; ++item) {
                    const quint16 offset = readBe16(data, cursor);
                    cursor += 2;
                    ++wordCount;
                    if ((offset & 1u) != 0 || quint32(offset) > quint32(codeLength - 4))
                        return invalid(QStringLiteral("Invalid Amiga short relocation offset"));
                }
            }
            if (!terminated)
                return invalid(QStringLiteral("Amiga short relocation record is not terminated"));
            if (wordCount & 1u) {
                if (cursor + 2 > data.size() || readBe16(data, cursor) != 0)
                    return invalid(QStringLiteral("Invalid Amiga short relocation padding"));
                cursor += 2;
            }
        } else {
            return invalid(QStringLiteral("Unsupported Amiga font HUNK record"));
        }
    }
    if (!ended)
        return invalid(QStringLiteral("Amiga font HUNK is not terminated"));
    while (cursor < data.size()) {
        if (data.at(cursor++) != '\0')
            return invalid(QStringLiteral("Unexpected data after Amiga font HUNK"));
    }

    const quint16 height = readBe16(code, 78);
    const quint8 style = quint8(code.at(80));
    const quint8 fontFlags = quint8(code.at(81));
    const quint16 nominalWidth = readBe16(code, 82);
    const quint16 baseline = readBe16(code, 84);
    const quint8 firstChar = quint8(code.at(90));
    const quint8 lastChar = quint8(code.at(91));
    const quint32 bitmapOffset = readBe32(code, 92);
    const quint16 modulo = readBe16(code, 96);
    const quint32 locationOffset = readBe32(code, 98);
    const quint32 spacingOffset = readBe32(code, 102);
    const quint32 kerningOffset = readBe32(code, 106);
    const bool colorFont = (style & 0x40u) != 0;
    const int encodedCount = int(lastChar) - int(firstChar) + 1;
    const int glyphCount = encodedCount;
    const int tableEntryCount = glyphCount;
    if (style & 0x80u)
        return invalid(QStringLiteral("Tagged Amiga font strikes are unsupported"));
    if (fontFlags & 0x04u)
        return invalid(QStringLiteral("Reverse-path Amiga font strikes are unsupported"));
    if (height == 0 || height > 4096 || nominalWidth == 0 || nominalWidth > 4096 ||
        baseline > height || firstChar > lastChar || modulo == 0 ||
        modulo > 8192 || glyphCount > 257)
        return invalid(QStringLiteral("Invalid Amiga TextFont metrics"));
    const quint64 bitmapBytes = quint64(modulo) * height;
    const quint64 locationsBytes = quint64(tableEntryCount) * 4;
    const quint64 locationEnd = quint64(locationOffset) + locationsBytes;
    const quint64 bitmapEnd = quint64(bitmapOffset) + bitmapBytes;
    const quint64 spacingEnd = quint64(spacingOffset) + quint64(tableEntryCount) * 2;
    const quint64 kerningEnd = quint64(kerningOffset) + quint64(tableEntryCount) * 2;
    const auto overlaps = [](quint64 firstStart, quint64 firstEnd,
                             quint64 secondStart, quint64 secondEnd) {
        return firstStart < secondEnd && secondStart < firstEnd;
    };
    if (bitmapOffset < (colorFont ? 154u : 110u) || locationOffset < 110 ||
        (spacingOffset && spacingOffset < 110) || (kerningOffset && kerningOffset < 110) ||
        quint64(bitmapOffset) + bitmapBytes > quint64(codeLength) ||
        quint64(locationOffset) + locationsBytes > quint64(codeLength) ||
        (spacingOffset && quint64(spacingOffset) + quint64(glyphCount) * 2 > quint64(codeLength)) ||
        (kerningOffset && quint64(kerningOffset) + quint64(glyphCount) * 2 > quint64(codeLength)) ||
        overlaps(bitmapOffset, bitmapEnd, locationOffset, locationEnd) ||
        (spacingOffset && overlaps(bitmapOffset, bitmapEnd, spacingOffset, spacingEnd)) ||
        (kerningOffset && overlaps(bitmapOffset, bitmapEnd, kerningOffset, kerningEnd)) ||
        (spacingOffset && overlaps(locationOffset, locationEnd, spacingOffset, spacingEnd)) ||
        (kerningOffset && overlaps(locationOffset, locationEnd, kerningOffset, kerningEnd)) ||
        (spacingOffset && kerningOffset &&
         overlaps(spacingOffset, spacingEnd, kerningOffset, kerningEnd)))
        return invalid(QStringLiteral("Amiga TextFont table points outside its HUNK"));

    QVector<quint32> planeOffsets;
    QVector<quint32> palette;
    int foregroundPaletteIndex = -1;
    quint8 planePick = 0;
    quint8 planeOnOff = 0;
    quint8 lowColor = 0;
    quint8 highColor = 0;
    quint8 depth = 0;
    quint64 colorHeaderStart = 0;
    quint64 colorHeaderEnd = 0;
    quint64 colorValuesStart = 0;
    quint64 colorValuesEnd = 0;
    if (colorFont) {
        if (codeLength < 154)
            return invalid(QStringLiteral("Truncated Amiga ColorTextFont extension"));
        const quint16 colorFlags = readBe16(code, 110);
        depth = quint8(code.at(112));
        const quint8 foregroundColor = quint8(code.at(113));
        lowColor = quint8(code.at(114));
        highColor = quint8(code.at(115));
        planePick = quint8(code.at(116));
        planeOnOff = quint8(code.at(117));
        const quint32 colorsOffset = readBe32(code, 118);
        if ((colorFlags != 1u && colorFlags != 2u) || depth == 0 || depth > 8 ||
            lowColor > highColor || quint32(highColor) - lowColor >= (1u << depth) ||
            codeLength < 154 || colorsOffset < 154 ||
            quint64(colorsOffset) + 8 > quint64(codeLength))
            return invalid(QStringLiteral("Invalid Amiga ColorTextFont palette metadata"));
        if (foregroundColor != 0xFFu) {
            if (foregroundColor < lowColor || foregroundColor > highColor)
                return invalid(QStringLiteral("Invalid Amiga ColorTextFont foreground color"));
            foregroundPaletteIndex = int(foregroundColor - lowColor);
        }
        const quint16 reserved = readBe16(code, int(colorsOffset));
        const quint16 colorCount = readBe16(code, int(colorsOffset) + 2);
        const quint32 colorValuesOffset = readBe32(code, int(colorsOffset) + 4);
        if (reserved != 0 || colorCount == 0 || colorCount > 256 ||
            colorCount < quint16(highColor - lowColor + 1) ||
            colorValuesOffset < colorsOffset + 8 ||
            quint64(colorValuesOffset) + quint64(colorCount) * 2 > quint64(codeLength))
            return invalid(QStringLiteral("Invalid Amiga ColorFontColors table"));
        colorHeaderStart = colorsOffset;
        colorHeaderEnd = quint64(colorsOffset) + 8;
        colorValuesStart = colorValuesOffset;
        colorValuesEnd = quint64(colorValuesOffset) + quint64(colorCount) * 2;
        for (int index = 0; index < 8; ++index) {
            const quint32 offset = readBe32(code, 122 + index * 4);
            if (index < depth) {
                if (offset < 154 || quint64(offset) + bitmapBytes > quint64(codeLength) ||
                    (index == 0 && offset != bitmapOffset))
                    return invalid(QStringLiteral("Invalid Amiga ColorTextFont bitplane"));
                planeOffsets.append(offset);
            }
        }
        const quint64 extensionStart = 110;
        if (overlaps(colorHeaderStart, colorHeaderEnd, colorValuesStart, colorValuesEnd) ||
            overlaps(colorHeaderStart, colorHeaderEnd, extensionStart, 154) ||
            overlaps(colorValuesStart, colorValuesEnd, extensionStart, 154) ||
            overlaps(colorHeaderStart, colorHeaderEnd, locationOffset, locationEnd) ||
            overlaps(colorValuesStart, colorValuesEnd, locationOffset, locationEnd) ||
            (spacingOffset && (overlaps(colorHeaderStart, colorHeaderEnd,
                                        spacingOffset, spacingEnd) ||
                               overlaps(colorValuesStart, colorValuesEnd,
                                        spacingOffset, spacingEnd))) ||
            (kerningOffset && (overlaps(colorHeaderStart, colorHeaderEnd,
                                        kerningOffset, kerningEnd) ||
                               overlaps(colorValuesStart, colorValuesEnd,
                                        kerningOffset, kerningEnd))))
            return invalid(QStringLiteral("Overlapping Amiga ColorFont data"));
        for (int index = 0; index < planeOffsets.size(); ++index) {
            const quint64 planeStart = planeOffsets.at(index);
            const quint64 planeEnd = planeStart + bitmapBytes;
            if (overlaps(planeStart, planeEnd, locationOffset, locationEnd) ||
                (spacingOffset && overlaps(planeStart, planeEnd, spacingOffset, spacingEnd)) ||
                (kerningOffset && overlaps(planeStart, planeEnd, kerningOffset, kerningEnd)) ||
                overlaps(planeStart, planeEnd, colorHeaderStart, colorHeaderEnd) ||
                overlaps(planeStart, planeEnd, colorValuesStart, colorValuesEnd))
                return invalid(QStringLiteral("Overlapping Amiga ColorFont bitplane data"));
            for (int previous = 0; previous < index; ++previous) {
                const quint64 previousStart = planeOffsets.at(previous);
                if (overlaps(planeStart, planeEnd, previousStart, previousStart + bitmapBytes))
                    return invalid(QStringLiteral("Overlapping Amiga ColorFont bitplanes"));
            }
        }
        palette.reserve(colorCount);
        for (int index = 0; index < colorCount; ++index) {
            const quint16 xrgb = readBe16(code, int(colorValuesOffset) + index * 2);
            const quint32 red = ((xrgb >> 8) & 0x0Fu) * 17u;
            const quint32 green = ((xrgb >> 4) & 0x0Fu) * 17u;
            const quint32 blue = (xrgb & 0x0Fu) * 17u;
            palette.append((red << 16) | (green << 8) | blue);
        }
    }

    BitmapFont parsed;
    parsed.format = colorFont ? Format::AmigaColor : Format::Amiga;
    const QByteArray rawName = code.mid(26, 32);
    const int nameEnd = rawName.indexOf('\0');
    parsed.name = QString::fromLatin1(rawName.constData(), nameEnd < 0 ? rawName.size() : nameEnd);
    if (parsed.name.isEmpty()) parsed.name = QStringLiteral("Amiga bitmap font");
    parsed.width = nominalWidth;
    parsed.height = height;
    parsed.ascent = baseline;
    parsed.descent = height - baseline;
    parsed.fixedWidth = (fontFlags & 0x20u) == 0;
    parsed.palette = palette;
    parsed.foregroundPaletteIndex = foregroundPaletteIndex;
    parsed.glyphs.reserve(glyphCount);
    quint64 decodedBitmapBytes = 0;
    for (int glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
        const quint32 location = readBe32(code, int(locationOffset) + glyphIndex * 4);
        const quint32 bitOffset = location >> 16;
        const quint32 glyphWidth = location & 0xFFFFu;
        if (glyphWidth > 4096 || bitOffset + glyphWidth > quint32(modulo) * 8u)
            return invalid(QStringLiteral("Amiga glyph exceeds the strike bitmap"));
        Glyph glyph;
        glyph.codepoint = glyphIndex < encodedCount
            ? quint32(firstChar) + quint32(glyphIndex) : 0xFFFFFFFFu;
        glyph.width = int(glyphWidth);
        glyph.height = height;
        glyph.bearingY = baseline;
        const int rowBytes = (glyph.width + 7) / 8;
        decodedBitmapBytes += colorFont ? quint64(glyph.width) * height
                                        : quint64(rowBytes) * height;
        if (decodedBitmapBytes > 64u * 1024u * 1024u)
            return invalid(QStringLiteral("Decoded Amiga glyph data exceeds the size limit"));
        glyph.advance = spacingOffset
            ? qint16(readBe16(code, int(spacingOffset) + glyphIndex * 2))
            : nominalWidth;
        glyph.bearingX = kerningOffset
            ? qint16(readBe16(code, int(kerningOffset) + glyphIndex * 2)) : 0;
        if (qAbs(glyph.advance) > 4096 || qAbs(glyph.bearingX) > 4096)
            return invalid(QStringLiteral("Amiga glyph spacing is outside the supported range"));
        if (colorFont)
            glyph.pixels = QByteArray(glyph.width * int(height), '\0');
        else
            glyph.bitmap = QByteArray(rowBytes * int(height), '\0');
        for (int y = 0; y < int(height); ++y) {
            for (int x = 0; x < glyph.width; ++x) {
                const quint32 sourceBit = bitOffset + quint32(x);
                if (colorFont) {
                    quint8 colorIndex = quint8(planeOnOff & quint8(~planePick));
                    for (int plane = 0; plane < planeOffsets.size(); ++plane) {
                        if (!(planePick & (1u << plane))) continue;
                        const quint8 source = quint8(code.at(int(planeOffsets.at(plane)) +
                            y * modulo + int(sourceBit / 8)));
                        if (source & (0x80u >> (sourceBit & 7u)))
                            colorIndex |= quint8(1u << plane);
                    }
                    if (colorIndex < lowColor || colorIndex > highColor)
                        return invalid(QStringLiteral("Amiga glyph references an undefined color"));
                    glyph.pixels[y * glyph.width + x] = char(colorIndex - lowColor);
                } else {
                    const quint8 source = quint8(code.at(int(bitmapOffset) + y * modulo +
                                                          int(sourceBit / 8)));
                    if (!(source & (0x80u >> (sourceBit & 7u)))) continue;
                    const int destination = y * rowBytes + x / 8;
                    glyph.bitmap[destination] = char(quint8(glyph.bitmap.at(destination)) |
                                                       (0x80u >> (x & 7)));
                }
            }
        }
        parsed.glyphs.append(glyph);
    }
    *font = parsed;
    if (error) error->clear();
    return true;
}

} // namespace font
} // namespace peare
