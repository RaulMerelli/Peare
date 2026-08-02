#include "OS2_RT_FONT.h"
#include <QSharedPointer>
#include <QMutexLocker>
#include <QMutex>
#include <QHash>
#include <QCryptographicHash>

#include <QImage>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QTextCodec>
#include <QtMath>
#include <algorithm>
#include <climits>

namespace peare {
namespace resources {
namespace {

constexpr int SignatureSize = 20;
constexpr int DefaultMetricsSize = 168;
constexpr int DefinitionSize = 28;
// FONTMETRICS uses 16-bit first/last character codes. Some OS/2 DBCS and
// system fonts legitimately contain more than 1024 cells (for example 1106).
// The cell-table bounds checks below are the authoritative corruption guard.
constexpr int MaximumCells = 65536;
constexpr int MaximumGlyphCommands = 4096;
constexpr int MaximumContourPoints = 65536;
constexpr quint16 FM_DEFN_OUTLINE = 0x0001;

quint16 u16(const QByteArray& d, int o) {
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | quint16(p[1] << 8);
}
qint16 i16(const QByteArray& d, int o) { return qint16(u16(d, o)); }
quint32 u32(const QByteArray& d, int o) {
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
qint32 i32(const QByteArray& d, int o) { return qint32(u32(d, o)); }

int readPositiveInt32(const QByteArray& data, int offset, int fallback) {
    const quint32 value = u32(data, offset);
    return value > 0 && value <= quint32(INT_MAX) ? int(value) : fallback;
}

QString readFixedString(const QByteArray& data, int offset, int length, int codePage) {
    if (codePage == 0) codePage = 850;
    if (offset < 0 || length <= 0 || offset + length > data.size()) return {};
    int actualLength = 0;
    while (actualLength < length && data[offset + actualLength] != '\0') ++actualLength;
    QTextCodec* codec = QTextCodec::codecForName(QByteArray("IBM ") + QByteArray::number(codePage));
    if (!codec) codec = QTextCodec::codecForName(QByteArray("CP") + QByteArray::number(codePage));
    if (!codec) codec = QTextCodec::codecForName("IBM 850");
    if (!codec) codec = QTextCodec::codecForLocale();
    return codec->toUnicode(data.constData() + offset, actualLength).trimmed();
}


struct PreviewGlyph
{
    int characterCode = 0;
    int advanceX = 1;
    int offsetX = 0;
    int offsetY = 0;
    QImage bitmap;
    QPainterPath vectorPath;
};

struct PreviewFont
{
    QString faceName;
    QString formatName;
    int firstCharacter = 0;
    int lastCharacter = 0;
    int defaultCharacter = 0;
    int breakCharacter = 0;
    int pixelHeight = 1;
    int lineHeight = 1;
    int codePage = 0;
    bool isVector = false;
    QString previewMessage;
    QVector<PreviewGlyph> glyphs;
};

const PreviewGlyph* resolveGlyph(const PreviewFont& font, int code)
{
    auto find = [&](int wanted) -> const PreviewGlyph* {
        for (const PreviewGlyph& glyph : font.glyphs)
            if (glyph.characterCode == wanted) return &glyph;
        return nullptr;
    };
    if (const PreviewGlyph* glyph = find(code)) return glyph;
    if (const PreviewGlyph* glyph = find(font.defaultCharacter)) return glyph;
    if (const PreviewGlyph* glyph = find('?')) return glyph;
    return find(' ');
}

QImage scaledGlyph(const PreviewGlyph& glyph, int scale, bool vector)
{
    scale = qMax(1, scale);
    if (!vector || glyph.vectorPath.isEmpty())
    {
        if (scale <= 1) return glyph.bitmap;
        return glyph.bitmap.scaled(qMax(1, glyph.bitmap.width() * scale),
                                   qMax(1, glyph.bitmap.height() * scale),
                                   Qt::IgnoreAspectRatio,
                                   Qt::FastTransformation);
    }

    const QRectF bounds = glyph.vectorPath.boundingRect();
    const int margin = scale;
    // Preserve the original side bearing and advance canvas instead of
    // cropping every scaled outline to its painted pixels.
    const int left = glyph.offsetX * scale - margin;
    const int top = glyph.offsetY * scale - margin;
    const int right = qMax(glyph.advanceX * scale, qCeil(bounds.right() * scale)) + margin;
    const int bottom = qCeil(bounds.bottom() * scale) + margin;
    QImage image(qMax(1, right - left + 1), qMax(1, bottom - top + 1), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.translate(-left, -top);
    painter.scale(scale, scale);
    painter.drawPath(glyph.vectorPath);
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

QImage renderFontText(const PreviewFont& font, const QString& text, int scale,
                      int padding = 8, QRgb foregroundRgba = qRgba(0, 0, 0, 255),
                      QRgb backgroundRgba = qRgba(255, 255, 255, 255))
{
    struct Prepared { QImage image; int x = 0; int y = 0; };
    QVector<Prepared> prepared;
    int penX = 0, minimumX = 0, minimumY = 0, maximumX = 0;
    int maximumY = qMax(1, font.lineHeight * scale);
    for (const QChar value : text)
    {
        const PreviewGlyph* glyph = resolveGlyph(font, value.unicode());
        if (!glyph)
        {
            penX += qMax(1, font.pixelHeight / 2) * scale;
            maximumX = qMax(maximumX, penX);
            continue;
        }
        Prepared item;
        item.image = tintGlyph(scaledGlyph(*glyph, scale, font.isVector), foregroundRgba);
        item.x = penX + glyph->offsetX * scale;
        item.y = glyph->offsetY * scale;
        prepared.push_back(item);
        minimumX = qMin(minimumX, item.x);
        minimumY = qMin(minimumY, item.y);
        maximumX = qMax(maximumX, item.x + item.image.width());
        maximumY = qMax(maximumY, item.y + item.image.height());
        penX += qMax(1, glyph->advanceX) * scale;
        maximumX = qMax(maximumX, penX);
    }
    const int margin = qMax(0, padding);
    QImage result(qMax(1, maximumX - minimumX + margin * 2),
                  qMax(1, maximumY - minimumY + margin * 2), QImage::Format_ARGB32);
    result.fill(backgroundRgba);
    QPainter painter(&result);
    for (const Prepared& item : prepared)
        painter.drawImage(margin + item.x - minimumX, margin + item.y - minimumY, item.image);
    return result;
}

int robustDimension(QVector<int> values, int fallback)
{
    if (values.isEmpty()) return qMax(1, fallback);
    std::sort(values.begin(), values.end());
    const int index = int(qFloor((values.size() - 1) * 0.90));
    return qMax(1, values[qBound(0, index, values.size() - 1)]);
}

QImage renderGlyphMap(const PreviewFont& font)
{
    if (font.glyphs.isEmpty()) return {};
    const int scale = font.isVector ? 1 : (font.pixelHeight <= 16 ? 2 : 1);
    constexpr int columns = 16;
    QVector<QImage> prepared;
    QVector<int> widths, heights;
    for (const PreviewGlyph& glyph : font.glyphs)
    {
        QImage image = scaledGlyph(glyph, scale, font.isVector);
        prepared.push_back(image);
        if (image.width() > 0) widths.push_back(image.width());
        if (image.height() > 0) heights.push_back(image.height());
    }
    const int imageAreaWidth = qMin(96, qMax(robustDimension(widths, qMax(8, font.pixelHeight * scale)),
                                             qMax(8, qMin(font.pixelHeight * 2 * scale, 96))));
    const int imageAreaHeight = qMin(96, qMax(robustDimension(heights, qMax(8, font.lineHeight * scale)),
                                              qMax(8, qMin(font.lineHeight * scale, 96))));
    const int cellWidth = imageAreaWidth + 12;
    const int cellHeight = imageAreaHeight + 18;
    const int rows = (font.glyphs.size() + columns - 1) / columns;
    QImage result(qMax(1, columns * cellWidth + 1), qMax(1, rows * cellHeight + 1), QImage::Format_ARGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, font.isVector);
    QFont codeFont(QStringLiteral("Monospace"), 7);
    codeFont.setStyleHint(QFont::TypeWriter);
    for (int i = 0; i < font.glyphs.size(); ++i)
    {
        const int column = i % columns, row = i / columns;
        const int cellX = column * cellWidth, cellY = row * cellHeight;
        painter.setPen(QColor(Qt::lightGray));
        painter.drawRect(cellX, cellY, cellWidth, cellHeight);
        QImage image = prepared[i];
        if (image.width() > imageAreaWidth || image.height() > imageAreaHeight)
            image = image.scaled(imageAreaWidth, imageAreaHeight, Qt::KeepAspectRatio,
                                 font.isVector ? Qt::SmoothTransformation : Qt::FastTransformation);
        painter.drawImage(cellX + 6 + qMax(0, (imageAreaWidth - image.width()) / 2),
                          cellY + 2 + qMax(0, (imageAreaHeight - image.height()) / 2), image);
        painter.setFont(codeFont);
        painter.setPen(QColor(Qt::darkGray));
        const int code = font.glyphs[i].characterCode;
        painter.drawText(cellX + 2, cellY + imageAreaHeight + 3, cellWidth - 4, 14,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(code, 16).toUpper().rightJustified(code <= 0xFF ? 2 : 4, QLatin1Char('0')));
    }
    return result;
}

ResourcePreview makePreview(const PreviewFont& font)
{
    ResourcePreview out;
    const QString sampleText = QStringLiteral("The quick brown fox jumps over the lazy dog");
    for (int scale = 1; scale <= 4; ++scale)
    {
        out.images.push_back(renderFontText(font, sampleText, scale));
        out.imageLabels.push_back(QStringLiteral("Sample \u2014 %1 px (%2\u00D7)").arg(font.pixelHeight * scale).arg(scale));
    }
    out.text = QStringLiteral("%1 \u2014 %2 \u2014 %3 \u2014 %4 glyphs \u2014 codes %5\u2013%6 \u2014 native height %7 px")
        .arg(font.faceName.isEmpty() ? QStringLiteral("Unnamed font") : font.faceName,
             font.formatName.isEmpty() ? QStringLiteral("FNT") : font.formatName,
             font.isVector ? QStringLiteral("vector") : QStringLiteral("raster"))
        .arg(font.glyphs.size()).arg(font.firstCharacter).arg(font.lastCharacter).arg(font.pixelHeight);
    // Keep individual glyphs for converted export instead of exporting the
    // composed sample and map images shown by the GUI.
    for (const PreviewGlyph& glyph : font.glyphs)
    {
        if (glyph.bitmap.isNull()) continue;
        out.conversionImages.push_back(glyph.bitmap);
        out.conversionImageCodes.push_back(glyph.characterCode);
    }
    if (font.codePage > 0) out.text += QStringLiteral(" \u2014 CP%1").arg(font.codePage);
    if (!font.previewMessage.isEmpty()) out.text += QStringLiteral("\r\n%1").arg(font.previewMessage);
    return out;
}

struct Metrics {
    int metricsOffset = SignatureSize;
    int definitionOffset = 0;
    int definitionRecordSize = 0;
    int cellArrayOffset = 0;
    int cellSize = 0;
    int defaultCellWidth = 0;
    int cellHeight = 0;
    int defaultCellIncrement = 0;
    int defaultASpace = 0;
    int defaultBSpace = 0;
    int defaultCSpace = 0;
    int firstCharacter = 0;
    int lastCharacter = 0;
    int defaultCharacter = 0;
    int breakCharacter = 0;
    int codePage = 0;
    int ascent = 0;
    int descent = 0;
    int emUnits = 0;
    int nominalPointTenths = 0;
    int definitionFlags = 0;
    int characterCount = 0;
    int outlineCharacterCount = 0;
};

bool parse(const QByteArray& data, Metrics& m, QString& error) {
    if (data.size() < SignatureSize + DefaultMetricsSize + DefinitionSize) {
        error = QStringLiteral("Invalid OS/2 font data: insufficient length."); return false;
    }

    // Resource containers from a few early OS/2 producers prepend bookkeeping
    // bytes before the normal 20-byte signature block. Locate the documented
    // "OS/2 FONT" marker and derive FONTMETRICS from it, while retaining the
    // canonical zero-based layout used by the C# decoder.
    const int marker = data.indexOf(QByteArrayLiteral("OS/2 FONT"));
    if (marker >= 8 && marker - 8 <= 16)
        m.metricsOffset = marker - 8 + SignatureSize;
    else
        m.metricsOffset = SignatureSize;

    // FONTMETRICS.fsDefn is the documented discriminator. Read metrics before
    // interpreting FONTDEFINITIONHEADER so outline fonts are never rejected by
    // a raster plausibility test.
    m.firstCharacter = u16(data, m.metricsOffset + 114);
    m.lastCharacter = u16(data, m.metricsOffset + 116);
    m.defaultCharacter = u16(data, m.metricsOffset + 118);
    m.breakCharacter = u16(data, m.metricsOffset + 120);
    m.codePage = u16(data, m.metricsOffset + 74);
    m.emUnits = qAbs(int(i16(data, m.metricsOffset + 76)));
    m.ascent = qAbs(int(i16(data, m.metricsOffset + 80)));
    m.descent = qAbs(int(i16(data, m.metricsOffset + 82)));
    m.nominalPointTenths = qAbs(int(i16(data, m.metricsOffset + 122)));
    m.definitionFlags = u16(data, m.metricsOffset + 130);

    m.characterCount = m.lastCharacter >= m.firstCharacter
        ? m.lastCharacter - m.firstCharacter + 1 : 0;
    if (m.characterCount <= 0 || m.characterCount > MaximumCells) {
        error = QStringLiteral("Invalid OS/2 font character range."); return false;
    }

    int metricsSize = readPositiveInt32(data, m.metricsOffset + 4, DefaultMetricsSize);
    if (metricsSize < 136 || m.metricsOffset + metricsSize + DefinitionSize > data.size())
        metricsSize = DefaultMetricsSize;

    const bool outline = (m.definitionFlags & FM_DEFN_OUTLINE) != 0;
    const int outlineCount = (m.lastCharacter > 0 && m.lastCharacter <= MaximumCells)
        ? m.lastCharacter : m.characterCount;

    const auto headerFits = [&](int offset) {
        if (offset < m.metricsOffset + 136 || offset + DefinitionSize > data.size())
            return false;
        const int cell = u16(data, offset + 12);
        if (cell != 6 && cell != 10)
            return false;
        const int count = outline ? outlineCount : m.characterCount;
        return qint64(offset + DefinitionSize) + qint64(count) * cell <= data.size();
    };

    m.definitionOffset = m.metricsOffset + metricsSize;
    if (!headerFits(m.definitionOffset)) {
        const int canonical = m.metricsOffset + DefaultMetricsSize;
        if (headerFits(canonical)) {
            m.definitionOffset = canonical;
        } else {
            // Some old generators wrote an incorrect FONTMETRICS.ulSize. Locate
            // the immediately following definition header by its documented
            // cell-record size, without using glyph data to decide font type.
            int found = -1;
            const int searchStart = m.metricsOffset + 136;
            const int searchEnd = qMin(data.size() - DefinitionSize,
                                       m.metricsOffset + 512);
            for (int candidate = searchStart; candidate <= searchEnd; candidate += 2) {
                if (headerFits(candidate)) {
                    found = candidate;
                    break;
                }
            }
            if (found < 0) {
                error = QStringLiteral("Invalid OS/2 FONTDEFINITIONHEADER.");
                return false;
            }
            m.definitionOffset = found;
        }
    }

    m.definitionRecordSize = readPositiveInt32(
        data, m.definitionOffset + 4, data.size() - m.definitionOffset);
    m.cellSize = u16(data, m.definitionOffset + 12);
    m.defaultCellWidth = qAbs(int(i16(data, m.definitionOffset + 14)));
    m.cellHeight = qAbs(int(i16(data, m.definitionOffset + 16)));
    m.defaultCellIncrement = qAbs(int(i16(data, m.definitionOffset + 18)));
    m.defaultASpace = i16(data, m.definitionOffset + 20);
    m.defaultBSpace = u16(data, m.definitionOffset + 22);
    m.defaultCSpace = i16(data, m.definitionOffset + 24);
    if (m.cellHeight <= 0) {
        error = QStringLiteral("Invalid OS/2 font cell height: %1").arg(m.cellHeight);
        return false;
    }

    m.cellArrayOffset = m.definitionOffset + DefinitionSize;
    const int activeCount = outline ? outlineCount : m.characterCount;
    if (qint64(m.cellArrayOffset) + qint64(activeCount) * m.cellSize > data.size()) {
        error = QStringLiteral("Invalid OS/2 font data: truncated cell table.");
        return false;
    }

    m.outlineCharacterCount = outlineCount;
    return true;
}

bool looksLikeBitmapFont(const QByteArray& data, const Metrics& m) {
    if (m.cellHeight <= 0 || m.cellHeight > 4096) return false;
    const int samples = qMin(m.characterCount, 32);
    int plausible = 0, nonEmpty = 0;
    for (int i = 0; i < samples; ++i) {
        const int entry = m.cellArrayOffset + i * m.cellSize;
        if (entry < 0 || entry + m.cellSize > data.size()) break;
        const quint32 glyphOffset = u32(data, entry);
        int width = u16(data, entry + (m.cellSize == 6 ? 4 : 6));
        if (width == 0 || width == 0x8000) {
            width = m.defaultBSpace;
            if (width == 0 || width == 0x8000) width = m.defaultCellWidth > 0 ? m.defaultCellWidth : m.defaultCellIncrement;
        }
        if (width <= 0) continue;
        ++nonEmpty;
        const qint64 requiredEnd = qint64(glyphOffset) + qint64((width + 7) / 8) * m.cellHeight;
        if (glyphOffset < quint32(data.size()) && requiredEnd <= data.size()) ++plausible;
    }
    return nonEmpty > 0 && plausible * 4 >= nonEmpty * 3;
}

QImage decodeGlyphBitmap(const QByteArray& data, quint32 glyphDataOffset, int width, int height) {
    QImage bitmap(qMax(1, width), qMax(1, height), QImage::Format_ARGB32);
    bitmap.fill(Qt::transparent);
    const int columns = (width + 7) / 8;
    const qint64 requiredEnd = qint64(glyphDataOffset) + qint64(columns) * height;
    if (glyphDataOffset >= quint32(data.size()) || requiredEnd > data.size()) return bitmap;
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < columns; ++column) {
            const uchar value = uchar(data[int(glyphDataOffset) + column * height + row]);
            for (int bit = 0; bit < 8; ++bit) {
                const int x = column * 8 + bit;
                if (x >= width) break;
                if (value & (0x80 >> bit)) bitmap.setPixelColor(x, row, Qt::black);
            }
        }
    return bitmap;
}

struct Contour { QVector<QPointF> points; };
QPointF readPoint(const QByteArray& data, int offset) { return QPointF(i16(data, offset), i16(data, offset + 2)); }
QPointF transformPoint(QPointF p, double scale, int ascender) { return QPointF(p.x() * scale, (ascender - p.y()) * scale); }
void addPoint(QVector<QPointF>& contour, const QPointF& point, QString& error) {
    if (contour.size() >= MaximumContourPoints) { error = QStringLiteral("OS/2 outline contour contains too many points."); return; }
    if (!contour.isEmpty() && qAbs(contour.last().x() - point.x()) < 0.0001 && qAbs(contour.last().y() - point.y()) < 0.0001) return;
    contour.push_back(point);
}
void closeContour(QVector<Contour>& contours, QVector<QPointF>& current) {
    if (current.size() >= 3) {
        if (qAbs(current.first().x() - current.last().x()) > 0.0001 || qAbs(current.first().y() - current.last().y()) > 0.0001) current.push_back(current.first());
        Contour contour;
        contour.points = current;
        contours.push_back(contour);
    }
    current.clear();
}

QVector<Contour> decodeOutlineGlyph(const QByteArray& data, int start, int length, double unitScale, int ascenderUnits, QString& error) {
    // Decodes the FOCA outline command stream stored in OS/2 GPI font
    // resources. The original conic curves are sampled into scalable contour
    // points; RT_FONT later rasterizes those contours with an alternate
    // (even/odd) fill rule.
    QVector<Contour> contours;
    if (length <= 0 || start < 0 || start >= data.size() || qint64(start) + length > data.size()) return contours;
    QVector<QPointF> current;
    QPointF currentRaw;
    bool hasCurrentPoint = false;
    int position = start, end = start + length, commandCount = 0;
    while (position + 2 <= end) {
        if (++commandCount > MaximumGlyphCommands) { error = QStringLiteral("OS/2 outline glyph contains too many commands."); return {}; }
        const uchar command = uchar(data[position++]);
        const int payloadLength = uchar(data[position++]);
        if (position + payloadLength > end) { error = QStringLiteral("Truncated OS/2 outline command stream."); return {}; }
        const int payloadStart = position, payloadEnd = position + payloadLength; position = payloadEnd;
        if (command == 0xFF) { closeContour(contours, current); break; }
        if (command == 0xC1 || command == 0xE4) { closeContour(contours, current); hasCurrentPoint = false; }
        if (command == 0xC1 || command == 0x81) {
            if (payloadLength & 3) { error = QStringLiteral("Invalid OS/2 outline point list."); return {}; }
            for (int p = payloadStart; p + 4 <= payloadEnd; p += 4) {
                currentRaw = readPoint(data, p); hasCurrentPoint = true;
                addPoint(current, transformPoint(currentRaw, unitScale, ascenderUnits), error);
                if (!error.isEmpty()) return {};
            }
            continue;
        }
        if (command == 0xE4 || command == 0xA4) {
            int curveStart = payloadStart;
            if (command == 0xE4) {
                if (payloadLength < 4) { error = QStringLiteral("OS/2 conic contour has no initial point."); return {}; }
                currentRaw = readPoint(data, payloadStart); hasCurrentPoint = true; current.clear();
                addPoint(current, transformPoint(currentRaw, unitScale, ascenderUnits), error); curveStart += 4;
            }
            if (!hasCurrentPoint || current.isEmpty()) { error = QStringLiteral("OS/2 conic continuation has no initial point."); return {}; }
            const int remaining = payloadEnd - curveStart;
            if (remaining < 0 || remaining % 12 != 0) { error = QStringLiteral("Invalid OS/2 conic command length."); return {}; }
            const int segmentCount = remaining / 12;
            const int weightsStart = curveStart + segmentCount * 8;
            for (int segment = 0; segment < segmentCount; ++segment) {
                const QPointF control = readPoint(data, curveStart + segment * 8);
                const QPointF target = readPoint(data, curveStart + segment * 8 + 4);
                double weight = i32(data, weightsStart + segment * 4) / 65536.0;
                if (!qIsFinite(weight) || weight == 0.0) weight = 1.0;
                for (int index = 1; index <= 28; ++index) {
                    const double t = double(index) / 28.0, mt = 1.0 - t;
                    const double a = mt * mt, b = 2.0 * weight * t * mt, c = t * t, denominator = a + b + c;
                    const QPointF raw = denominator == 0.0 ? target : QPointF((a*currentRaw.x()+b*control.x()+c*target.x())/denominator, (a*currentRaw.y()+b*control.y()+c*target.y())/denominator);
                    addPoint(current, transformPoint(raw, unitScale, ascenderUnits), error);
                    if (!error.isEmpty()) return {};
                }
                currentRaw = target;
            }
            continue;
        }
        error = QStringLiteral("Unsupported OS/2 outline command 0x%1.").arg(QString::number(command, 16).rightJustified(2, QLatin1Char('0')).toUpper()); return {};
    }
    closeContour(contours, current);
    return contours;
}

PreviewFont decodeBitmapFont(const QByteArray& data, const Metrics& m, QString& error) {
    if (m.cellHeight > 1024) { error = QStringLiteral("Invalid OS/2 bitmap font cell height: %1").arg(m.cellHeight); return {}; }
    PreviewFont font;
    font.faceName = readFixedString(data, m.metricsOffset + 40, 32, m.codePage);
    if (font.faceName.isEmpty()) font.faceName = QStringLiteral("OS/2 font");
    font.formatName = m.cellSize == 10 ? QStringLiteral("OS/2 FNT type 3") : QStringLiteral("OS/2 FNT type 1/2");
    font.firstCharacter = m.firstCharacter;
    font.lastCharacter = m.lastCharacter;
    font.defaultCharacter = m.defaultCharacter;
    font.breakCharacter = m.breakCharacter;
    font.pixelHeight = m.cellHeight;
    font.lineHeight = qMax(m.cellHeight, (m.ascent > 0 ? m.ascent : m.cellHeight) + m.descent);
    font.codePage = m.codePage;
    font.isVector = false;
    for (int i = 0; i < m.characterCount; ++i) {
        const int cell = m.cellArrayOffset + i * m.cellSize;
        const quint32 glyphOffset = u32(data, cell);
        int a = 0, b = 0, c = 0;
        if (m.cellSize == 6) { b = u16(data, cell + 4); if (b <= 0) b = m.defaultCellWidth > 0 ? m.defaultCellWidth : m.defaultCellIncrement; }
        else {
            a = i16(data, cell + 4); b = u16(data, cell + 6); c = i16(data, cell + 8);
            if (a == SHRT_MIN) a = m.defaultASpace == SHRT_MIN ? 0 : m.defaultASpace;
            if (b == 0x8000 || b == 0) b = (m.defaultBSpace == 0x8000 || m.defaultBSpace == 0) ? m.defaultCellWidth : m.defaultBSpace;
            if (c == SHRT_MIN) c = m.defaultCSpace == SHRT_MIN ? 0 : m.defaultCSpace;
        }
        if (b <= 0) b = m.defaultCellWidth > 0 ? m.defaultCellWidth : 1;
        if (b > 4096) { error = QStringLiteral("Invalid OS/2 glyph width %1.").arg(b); return {}; }
        int advance = a + b + c;
        if (advance <= 0) advance = m.defaultCellIncrement > 0 ? m.defaultCellIncrement : b;
        PreviewGlyph glyph;
        glyph.characterCode = m.firstCharacter + i;
        glyph.advanceX = qMax(1, advance);
        glyph.offsetX = a;
        glyph.bitmap = decodeGlyphBitmap(data, glyphOffset, b, m.cellHeight);
        font.glyphs.push_back(glyph);
    }
    return font;
}

PreviewFont decodeOutlineFont(const QByteArray& data, const Metrics& m, QString& error) {
    const int count = m.outlineCharacterCount;
    const int baseHeight = m.nominalPointTenths > 0 ? qBound(12, (m.nominalPointTenths + 5) / 10 + 4, 32) : 16;
    int designHeight = m.cellHeight > 0 ? m.cellHeight : qMax(1, m.ascent + m.descent);
    if (designHeight <= 0) designHeight = m.emUnits > 0 ? m.emUnits : 1000;
    const double scale = double(baseHeight) / designHeight;
    int definitionEnd = m.definitionOffset + m.definitionRecordSize;
    if (definitionEnd < m.definitionOffset || definitionEnd > data.size()) definitionEnd = data.size();
    QVector<int> glyphOffsets(count), advances(count), sorted;
    for (int i = 0; i < count; ++i) {
        const int cell = m.cellArrayOffset + i * m.cellSize;
        const quint32 raw = u32(data, cell);
        const int glyph = raw <= quint32(INT_MAX) ? int(raw) : 0;
        glyphOffsets[i] = glyph;
        if (glyph > 0 && glyph < definitionEnd && !sorted.contains(glyph)) sorted.push_back(glyph);
        int a=0,b=0,c=0;
        if (m.cellSize == 6) { b=u16(data,cell+4); if (b<=0||b==0x8000) b=m.defaultCellWidth>0?m.defaultCellWidth:m.defaultCellIncrement; }
        else { a=i16(data,cell+4); b=u16(data,cell+6); c=i16(data,cell+8); if(a==SHRT_MIN)a=m.defaultASpace==SHRT_MIN?0:m.defaultASpace; if(b==0||b==0x8000){b=m.defaultBSpace;if(b==0||b==0x8000)b=m.defaultCellWidth>0?m.defaultCellWidth:m.defaultCellIncrement;} if(c==SHRT_MIN)c=m.defaultCSpace==SHRT_MIN?0:m.defaultCSpace; }
        int advance=a+b+c;
        if(advance<=0) advance=m.defaultCellIncrement>0?m.defaultCellIncrement:qMax(1,b);
        advances[i]=qMax(1,qRound(advance*scale));
    }
    std::sort(sorted.begin(), sorted.end());
    const int lineHeight = qMax(baseHeight, qMax(1, qRound(m.ascent*scale)) + qMax(0,qRound(m.descent*scale)));
    PreviewFont font;
    font.faceName = readFixedString(data,m.metricsOffset+40,32,m.codePage);
    if(font.faceName.isEmpty()) font.faceName=QStringLiteral("OS/2 font");
    font.formatName = QStringLiteral("OS/2 GPI outline");
    font.firstCharacter = m.firstCharacter;
    font.lastCharacter = m.firstCharacter + count - 1;
    font.defaultCharacter = m.firstCharacter + m.defaultCharacter;
    font.breakCharacter = m.firstCharacter + m.breakCharacter;
    font.pixelHeight = baseHeight;
    font.lineHeight = lineHeight;
    font.codePage = m.codePage;
    font.isVector = true;
    int failed=0;
    for (int i=0;i<count;++i) {
        int start=glyphOffsets[i], end=definitionEnd;
        for(int offset: sorted) if(offset>start){end=offset;break;}
        QString error;
        QVector<Contour> contours;
        if(start>0&&start<end&&end<=data.size()) contours=decodeOutlineGlyph(data,start,end-start,scale,m.ascent,error);
        if(!error.isEmpty()) ++failed;
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        for(const Contour& contour: contours) {
            if(contour.points.isEmpty()) continue;
            path.moveTo(contour.points.first());
            for(int p=1;p<contour.points.size();++p) path.lineTo(contour.points[p]);
            path.closeSubpath();
        }
        QRectF bounds = path.boundingRect();
        const int left = qFloor(qMin(0.0, bounds.left()));
        const int top = qFloor(qMin(0.0, bounds.top()));
        const int width = qMax(1, qCeil(qMax(double(advances[i]), bounds.right()) - left) + 1);
        const int height = qMax(lineHeight, qCeil(bounds.bottom() - top) + 1);
        QImage bitmap(width, qMax(1,height), QImage::Format_ARGB32);
        bitmap.fill(Qt::transparent);
        QPainter painter(&bitmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.translate(-left, -top);
        painter.drawPath(path);
        PreviewGlyph glyph;
        glyph.characterCode = m.firstCharacter + i;
        glyph.advanceX = advances[i];
        glyph.offsetX = left;
        glyph.offsetY = top;
        glyph.bitmap = bitmap;
        glyph.vectorPath = path;
        font.glyphs.push_back(glyph);
    }
    if(failed>0) font.previewMessage = QStringLiteral("%1 outline glyph(s) contained unsupported or malformed commands.").arg(failed);
    else if(m.definitionFlags&1) font.previewMessage = QStringLiteral("OS/2 FOCA outline decoded with line and rational conic commands.");
    if (font.glyphs.isEmpty()) error = QStringLiteral("The OS/2 font contains no decodable glyphs.");
    return font;
}

} // namespace

bool OS2_RT_FONT::LooksLike(const QByteArray& data) {
    // The signature is nine characters. Depending on the producer it is
    // followed by NUL, a space or additional header data. Requiring the
    // tenth byte to be a space caused valid standalone OS/2 fonts to be
    // missed by RawDetect.
    return data.size() >= 17 && data.mid(8, 9) == QByteArrayLiteral("OS/2 FONT");
}

QSharedPointer<PreviewFont> cachedOs2Font(const QByteArray& data, QString& error)
{
    static QMutex mutex;
    static QHash<QByteArray,QSharedPointer<PreviewFont>> cache;
    const QByteArray key=QCryptographicHash::hash(data,QCryptographicHash::Sha256);
    { QMutexLocker lock(&mutex); const auto it=cache.constFind(key); if(it!=cache.constEnd()) return it.value(); }
    Metrics m;
    if(!parse(data,m,error)) return {};
    auto decoded=QSharedPointer<PreviewFont>::create((m.definitionFlags & FM_DEFN_OUTLINE) != 0
        ? decodeOutlineFont(data,m,error) : decodeBitmapFont(data,m,error));
    if(!error.isEmpty() || decoded->glyphs.isEmpty()) return decoded;
    QMutexLocker lock(&mutex);
    if(cache.size()>=8) cache.erase(cache.begin());
    cache.insert(key,decoded);
    return decoded;
}

ResourcePreview OS2_RT_FONT::preview(const ResourceEntry& entry) {
    ResourcePreview out; QString error;
    const QSharedPointer<PreviewFont> font=cachedOs2Font(entry.data,error);
    if (!font || !error.isEmpty()) { out.error = error; return out; }
    return makePreview(*font);
}

QImage OS2_RT_FONT::renderText(const ResourceEntry& entry, const QString& text, int scale,
                              int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                              QString* errorOut)
{
    if (errorOut) errorOut->clear();
    QString error;
    const QSharedPointer<PreviewFont> fontPtr=cachedOs2Font(entry.data,error);
    if (!fontPtr || !error.isEmpty() || fontPtr->glyphs.isEmpty()) {
        if (errorOut) *errorOut = error.isEmpty()
            ? QStringLiteral("The OS/2 font contains no decodable glyphs.") : error;
        return {};
    }
    const QImage image = renderFontText(*fontPtr, text, scale, padding,
                                        foregroundRgba, backgroundRgba);
    if (image.isNull() && errorOut)
        *errorOut = QStringLiteral("Font text could not be rendered.");
    return image;
}

} // namespace resources
} // namespace peare
