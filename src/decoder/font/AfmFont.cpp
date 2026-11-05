#include "AfmFont.h"

#include <QRegularExpression>

namespace peare {
namespace font {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool number(const QByteArray& text, double* value)
{
    bool ok = false;
    const double parsed = QString::fromLatin1(text.trimmed()).toDouble(&ok);
    if (!ok || parsed < -1000000000.0 || parsed > 1000000000.0) return false;
    *value = parsed;
    return true;
}

QByteArray fieldValue(const QByteArray& line, const QByteArray& key)
{
    if (!line.startsWith(key) || line.size() <= key.size() || line.at(key.size()) != ' ')
        return {};
    return line.mid(key.size() + 1).trimmed();
}

bool parseGlyph(const QByteArray& line, AfmGlyph* glyph)
{
    if (!glyph || line.size() > 4096) return false;
    bool hasWidth = false;
    const QList<QByteArray> fields = line.split(';');
    for (QByteArray field : fields) {
        field = field.trimmed();
        if (field.startsWith("C ")) {
            bool ok = false;
            const int code = field.mid(2).trimmed().toInt(&ok);
            if (!ok || code < -1 || code > 0x10ffff) return false;
            glyph->code = code;
        } else if (field.startsWith("CH ")) {
            QByteArray hex = field.mid(3).trimmed();
            if (hex.startsWith('<') && hex.endsWith('>')) hex = hex.mid(1, hex.size() - 2);
            bool ok = false;
            const uint code = hex.toUInt(&ok, 16);
            if (!ok || code > 0x10ffff) return false;
            glyph->code = int(code);
        } else if (field.startsWith("WX ") || field.startsWith("W0X ")) {
            const int prefix = field.startsWith("W0X ") ? 4 : 3;
            if (!number(field.mid(prefix), &glyph->advance)) return false;
            hasWidth = true;
        } else if (field.startsWith("N ")) {
            glyph->name = QString::fromLatin1(field.mid(2).trimmed());
        }
    }
    return hasWidth;
}

} // namespace

bool hasAfmHeader(const QByteArray& data)
{
    if (data.size() < 18 || data.size() > 64 * 1024 * 1024) return false;
    int lineEnd = data.indexOf('\n');
    const int carriageReturn = data.indexOf('\r');
    if (lineEnd < 0 || (carriageReturn >= 0 && carriageReturn < lineEnd))
        lineEnd = carriageReturn;
    const QByteArray firstLine = data.left(lineEnd < 0 ? data.size() : lineEnd).trimmed();
    static const QRegularExpression header(QStringLiteral("^StartFontMetrics\\s+[0-9]+\\.[0-9]+$"));
    return header.match(QString::fromLatin1(firstLine)).hasMatch();
}

bool parseAfm(const QByteArray& data, AfmFont* out, QString* error)
{
    if (!out || !hasAfmHeader(data)) return fail(error, QStringLiteral("Invalid AFM header"));
    if (error) error->clear();
    AfmFont font;
    bool inMetrics = false;
    bool endedMetrics = false;
    bool endedFont = false;
    int lineNumber = 0;
    QByteArray normalized = data;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');
    const QList<QByteArray> lines = normalized.split('\n');
    for (QByteArray line : lines) {
        if (++lineNumber > 1000000 || line.size() > 65536)
            return fail(error, QStringLiteral("AFM input exceeds line limits"));
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith("Comment ")) continue;
        if (inMetrics && line != "EndCharMetrics") {
            AfmGlyph glyph;
            if (!parseGlyph(line, &glyph))
                return fail(error, QStringLiteral("Invalid AFM character metric"));
            font.glyphs.push_back(glyph);
            if (font.glyphs.size() > 1000000)
                return fail(error, QStringLiteral("AFM glyph count exceeds limits"));
            continue;
        }
        if (line == "EndCharMetrics") {
            if (!inMetrics) return fail(error, QStringLiteral("Unexpected EndCharMetrics"));
            inMetrics = false;
            endedMetrics = true;
            continue;
        }
        if (line == "EndFontMetrics") {
            endedFont = true;
            break;
        }
        if (line.startsWith("StartCharMetrics ")) {
            bool ok = false;
            font.declaredGlyphs = line.mid(17).trimmed().toInt(&ok);
            if (!ok || font.declaredGlyphs < 0 || font.declaredGlyphs > 1000000)
                return fail(error, QStringLiteral("Invalid AFM character count"));
            inMetrics = true;
            continue;
        }
        QByteArray value = fieldValue(line, "FontName");
        if (!value.isEmpty()) font.fontName = QString::fromLatin1(value);
        value = fieldValue(line, "FamilyName");
        if (!value.isEmpty()) font.familyName = QString::fromLatin1(value);
        value = fieldValue(line, "FullName");
        if (!value.isEmpty()) font.fullName = QString::fromLatin1(value);
        value = fieldValue(line, "Weight");
        if (!value.isEmpty()) font.weight = QString::fromLatin1(value);
        value = fieldValue(line, "ItalicAngle");
        if (!value.isEmpty() && !number(value, &font.italicAngle))
            return fail(error, QStringLiteral("Invalid AFM italic angle"));
        value = fieldValue(line, "Ascender");
        if (!value.isEmpty() && !number(value, &font.ascender))
            return fail(error, QStringLiteral("Invalid AFM ascender"));
        value = fieldValue(line, "Descender");
        if (!value.isEmpty() && !number(value, &font.descender))
            return fail(error, QStringLiteral("Invalid AFM descender"));
        value = fieldValue(line, "FontBBox");
        if (!value.isEmpty()) {
            const QList<QByteArray> box = value.simplified().split(' ');
            if (box.size() != 4) return fail(error, QStringLiteral("Invalid AFM font bounds"));
            double left = 0, bottom = 0, right = 0, top = 0;
            if (!number(box.at(0), &left) || !number(box.at(1), &bottom) ||
                !number(box.at(2), &right) || !number(box.at(3), &top) ||
                right < left || top < bottom)
                return fail(error, QStringLiteral("Invalid AFM font bounds"));
            font.bboxWidth = right - left;
            font.bboxHeight = top - bottom;
        }
    }
    if (!endedFont || !endedMetrics || font.glyphs.size() > font.declaredGlyphs)
        return fail(error, QStringLiteral("Incomplete AFM metrics section"));
    if (font.familyName.isEmpty()) font.familyName = font.fullName;
    if (font.familyName.isEmpty()) font.familyName = font.fontName;
    if (font.familyName.isEmpty()) font.familyName = QStringLiteral("Adobe Font Metrics");
    *out = font;
    return true;
}

} // namespace font
} // namespace peare
