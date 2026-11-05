#include "WadImageDecoder.h"

#include <QtEndian>
#include <algorithm>
#include <limits>

namespace peare { namespace resources {
namespace {

quint16 u16(const QByteArray& data, int offset)
{
    return offset >= 0 && offset <= data.size() - 2
        ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset)) : 0;
}

quint32 u32(const QByteArray& data, int offset)
{
    return offset >= 0 && offset <= data.size() - 4
        ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset)) : 0;
}

QString lumpName(const char* bytes, int length)
{
    QByteArray value(bytes, length);
    const int zero = value.indexOf('\0');
    if (zero >= 0) value.truncate(zero);
    return QString::fromLatin1(value).trimmed().toUpper();
}

bool loadPalette(const WadLumpResolver& resolver, QVector<QRgb>* colors, QString* error)
{
    const QByteArray palette = resolver(QStringLiteral("WAD_PALETTE"), QStringLiteral("PLAYPAL"));
    if (palette.size() < 768) {
        if (error) *error = QStringLiteral("WAD PLAYPAL palette is missing or truncated");
        return false;
    }
    colors->resize(256);
    for (int i = 0; i < 256; ++i)
        (*colors)[i] = qRgb(uchar(palette[i * 3]), uchar(palette[i * 3 + 1]), uchar(palette[i * 3 + 2]));
    return true;
}

bool decodePatch(const QByteArray& data, const QVector<QRgb>& palette, QImage* image, QString* error)
{
    if (data.size() < 8) {
        if (error) *error = QStringLiteral("Doom patch header is truncated");
        return false;
    }
    const quint16 width = u16(data, 0), height = u16(data, 2);
    if (!width || !height || width > 4096 || height > 4096 ||
        quint64(width) * height > 16u * 1024u * 1024u ||
        quint64(8) + quint64(width) * 4 > quint64(data.size())) {
        if (error) *error = QStringLiteral("Invalid Doom patch dimensions or column table");
        return false;
    }
    QImage decoded(width, height, QImage::Format_ARGB32);
    if (decoded.isNull()) {
        if (error) *error = QStringLiteral("Could not allocate Doom patch image");
        return false;
    }
    decoded.fill(Qt::transparent);
    for (quint16 x = 0; x < width; ++x) {
        const quint32 column = u32(data, 8 + int(x) * 4);
        if (column >= quint32(data.size())) {
            if (error) *error = QStringLiteral("Doom patch column offset is outside the lump");
            return false;
        }
        int cursor = int(column);
        int previousTop = -1;
        bool ended = false;
        while (cursor < data.size()) {
            const quint8 topDelta = quint8(data[cursor++]);
            if (topDelta == 0xFF) { ended = true; break; }
            if (cursor + 2 > data.size()) break;
            const quint8 length = quint8(data[cursor++]);
            ++cursor;
            int top = int(topDelta);
            if (top <= previousTop) top += previousTop;
            if (top < 0 || top > 65535 || cursor + int(length) + 1 > data.size()) break;
            for (int y = 0; y < length; ++y) {
                const int row = top + y;
                if (row < height)
                    decoded.setPixel(int(x), row, palette[quint8(data[cursor + y])]);
            }
            cursor += int(length) + 1;
            previousTop = top;
        }
        if (!ended) {
            if (error) *error = QStringLiteral("Doom patch column has no valid terminator");
            return false;
        }
    }
    *image = decoded;
    return true;
}

bool textureNames(const QByteArray& pnames, QVector<QString>* names, QString* error)
{
    if (pnames.size() < 4) {
        if (error) *error = QStringLiteral("WAD PNAMES lump is missing or truncated");
        return false;
    }
    const quint32 count = u32(pnames, 0);
    if (count > 65536 || quint64(count) * 8 + 4 > quint64(pnames.size())) {
        if (error) *error = QStringLiteral("Invalid WAD PNAMES table");
        return false;
    }
    names->reserve(int(count));
    for (quint32 i = 0; i < count; ++i)
        names->append(lumpName(pnames.constData() + 4 + int(i) * 8, 8));
    return true;
}

QVector<QImage> decodeTextures(const QByteArray& data, const QVector<QRgb>& palette,
                               const WadLumpResolver& resolver, QString* error)
{
    if (data.size() < 4) {
        if (error) *error = QStringLiteral("TEXTUREx header is truncated");
        return {};
    }
    const quint32 count = u32(data, 0);
    if (count > 65536 || quint64(count) * 4 + 4 > quint64(data.size())) {
        if (error) *error = QStringLiteral("Invalid TEXTUREx directory");
        return {};
    }
    QVector<QString> patchNames;
    if (!textureNames(resolver(QStringLiteral("WAD_PNAMES"), QStringLiteral("PNAMES")), &patchNames, error)) return {};
    QVector<QImage> result;
    result.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const quint32 offset = u32(data, 4 + int(i) * 4);
        if (offset > quint32(data.size()) || quint32(data.size()) - offset < 22) {
            if (error) *error = QStringLiteral("TEXTUREx definition offset is invalid");
            return {};
        }
        const int base = int(offset);
        const int width = u16(data, base + 12), height = u16(data, base + 14);
        const quint16 patchCount = u16(data, base + 20);
        if (!width || !height || width > 4096 || height > 4096 ||
            quint64(width) * height > 16u * 1024u * 1024u ||
            quint64(base) + 22u + quint64(patchCount) * 10u > quint64(data.size())) {
            if (error) *error = QStringLiteral("Invalid TEXTUREx dimensions or patch list");
            return {};
        }
        QImage composed(width, height, QImage::Format_ARGB32);
        if (composed.isNull()) {
            if (error) *error = QStringLiteral("Could not allocate composite texture");
            return {};
        }
        composed.fill(Qt::transparent);
        for (quint16 patchIndex = 0; patchIndex < patchCount; ++patchIndex) {
            const int patchRecord = base + 22 + int(patchIndex) * 10;
            const qint16 originX = qint16(u16(data, patchRecord));
            const qint16 originY = qint16(u16(data, patchRecord + 2));
            const quint16 nameIndex = u16(data, patchRecord + 4);
            if (nameIndex >= patchNames.size()) {
                if (error) *error = QStringLiteral("TEXTUREx references an invalid PNAMES index");
                return {};
            }
            QByteArray patchData = resolver(QStringLiteral("WAD_DOOM_PATCH"), patchNames[nameIndex]);
            if (patchData.isEmpty()) patchData = resolver(QStringLiteral("WAD_DOOM_SPRITE"), patchNames[nameIndex]);
            if (patchData.isEmpty()) patchData = resolver(QStringLiteral("WAD_LUMP"), patchNames[nameIndex]);
            QImage patch;
            if (!decodePatch(patchData, palette, &patch, error)) return {};
            for (int y = 0; y < patch.height(); ++y) {
                const int targetY = originY + y;
                if (targetY < 0 || targetY >= height) continue;
                for (int x = 0; x < patch.width(); ++x) {
                    const int targetX = originX + x;
                    if (targetX < 0 || targetX >= width) continue;
                    const QRgb pixel = patch.pixel(x, y);
                    if (qAlpha(pixel)) composed.setPixel(targetX, targetY, pixel);
                }
            }
        }
        result.append(std::move(composed));
    }
    return result;
}

} // namespace

QVector<QImage> WadImageDecoder::decode(const QByteArray& payload, const QString& type,
                                        const WadLumpResolver& resolver, QString* error)
{
    if (error) error->clear();
    if (type != QLatin1String("WAD_TEXTUREX") &&
        type != QLatin1String("WAD_DOOM_PATCH") && type != QLatin1String("WAD_DOOM_SPRITE"))
        return {};
    QVector<QRgb> palette;
    if (!loadPalette(resolver, &palette, error)) return {};
    if (type == QLatin1String("WAD_TEXTUREX"))
        return decodeTextures(payload, palette, resolver, error);
    QImage image;
    if (!decodePatch(payload, palette, &image, error)) return {};
    return {image};
}

} }
