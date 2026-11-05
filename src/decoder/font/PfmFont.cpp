#include "PfmFont.h"

#include <QtGlobal>

namespace peare {
namespace font {
namespace {

quint16 read16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) |
           (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 read32(const QByteArray& data, int offset)
{
    return quint32(read16(data, offset)) |
           (quint32(read16(data, offset + 2)) << 16);
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QString readName(const QByteArray& data, quint32 offset)
{
    if (!offset || offset >= quint32(data.size())) return {};
    const int start = int(offset);
    int end = data.indexOf('\0', start);
    if (end < 0 || end - start > 256) return {};
    return QString::fromLocal8Bit(data.constData() + start, end - start).trimmed();
}

} // namespace

bool hasPfmHeader(const QByteArray& data)
{
    return data.size() >= 117 && data.size() <= 64 * 1024 * 1024 &&
           (read16(data, 0) == 0x0100 || read16(data, 0) == 0x0200) &&
           read32(data, 2) >= 117 && read32(data, 2) <= quint32(data.size());
}

bool parsePfm(const QByteArray& data, AfmFont* out, QString* error)
{
    if (!out || !hasPfmHeader(data))
        return fail(error, QStringLiteral("Invalid PFM header"));
    if (error) error->clear();

    const quint32 fileSize = read32(data, 2);
    const quint8 first = quint8(data.at(95));
    const quint8 last = quint8(data.at(96));
    if (last < first) return fail(error, QStringLiteral("Invalid PFM character range"));

    AfmFont font;
    font.fontName = readName(data, read32(data, 105));
    font.familyName = font.fontName;
    font.fullName = font.fontName;
    const quint16 rawWeight = read16(data, 83);
    if (rawWeight >= 700) font.weight = QStringLiteral("Bold");
    else if (rawWeight > 0) font.weight = QStringLiteral("Regular");
    font.italicAngle = quint8(data.at(80)) ? -1 : 0;
    font.ascender = read16(data, 74);
    font.descender = -double(qMax(0, int(read16(data, 88)) - int(read16(data, 74))));
    font.declaredGlyphs = int(last) - int(first) + 1;

    quint32 extOffset = 117;
    quint16 extSize = 0;
    const quint32 afterWidths = 117 + quint32(last - first + 2) * 2;
    const quint32 candidates[] = {extOffset, afterWidths};
    bool foundExtension = false;
    for (quint32 candidate : candidates) {
        if (quint64(candidate) + 2 > fileSize) continue;
        const quint16 candidateSize = read16(data, int(candidate));
        if (candidateSize >= 10 && quint64(candidate) + candidateSize <= fileSize) {
            extOffset = candidate;
            extSize = candidateSize;
            foundExtension = true;
            break;
        }
    }
    if (!foundExtension)
        return fail(error, QStringLiteral("Invalid PFM extension"));
    const quint32 extentOffset = read32(data, int(extOffset) + 6);
    const int glyphCount = font.declaredGlyphs;
    if (extentOffset && (quint64(extentOffset) + quint64(glyphCount) * 2 > fileSize))
        return fail(error, QStringLiteral("PFM extent table exceeds the file"));

    const quint16 averageWidth = read16(data, 91);
    const quint16 maximumWidth = read16(data, 93);
    font.bboxWidth = maximumWidth;
    font.bboxHeight = font.ascender - font.descender;
    for (int index = 0; index < glyphCount; ++index) {
        AfmGlyph glyph;
        glyph.code = int(first) + index;
        glyph.advance = extentOffset
            ? read16(data, int(extentOffset) + index * 2) : averageWidth;
        font.glyphs.push_back(glyph);
    }
    if (font.familyName.isEmpty()) font.familyName = QStringLiteral("Printer Font Metrics");
    *out = font;
    return true;
}

} // namespace font
} // namespace peare
