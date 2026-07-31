#include "RT_CURSOR.h"

#include "../RT_BITMAP/RT_BITMAP.h"
#include "../RT_ICON/Win12MonochromeResource.h"

#include <QDebug>
#include <QtEndian>

namespace peare {
namespace resources {
namespace {

quint16 ReadUInt16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;

    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 ReadUInt32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;

    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(data.constData() + offset));
}

Img EmptyImage()
{
    QImage bitmap(1, 1, QImage::Format_ARGB32);
    bitmap.fill(Qt::transparent);
    Img image;
    image.Bitmap = bitmap;
    image.BitCount = 0;
    image.Size = QSize(0, 0);
    return image;
}

Img Get(const QByteArray& resData)
{
    if (resData.size() < 14)
        return EmptyImage();

    Img legacyImage;
    if (Win12MonochromeResource::TryDecode(
            resData,
            0,
            legacyImage))
    {
        return legacyImage;
    }

    if (resData.size() > 4
        && static_cast<quint8>(resData.at(0)) == 0x89
        && static_cast<quint8>(resData.at(1)) == 0x50
        && static_cast<quint8>(resData.at(2)) == 0x4E
        && static_cast<quint8>(resData.at(3)) == 0x47)
    {
        QImage bitmap;
        bitmap.loadFromData(resData, "PNG");
        if (bitmap.isNull())
            return EmptyImage();

        Img image;
        image.Bitmap = bitmap;
        image.BitCount = bitmap.depth();
        image.Size = bitmap.size();
        return image;
    }

    if (resData.size() < 20)
        return EmptyImage();

    // Skip first 4 bytes (hotspotX + hotspotY)
    constexpr int hotspotOffset = 4;

    const int biSize = static_cast<int>(ReadUInt32(resData, hotspotOffset + 0));
    const int width = static_cast<int>(ReadUInt32(resData, hotspotOffset + 4));
    const int fullHeight = static_cast<int>(ReadUInt32(resData, hotspotOffset + 8));
    const int height = fullHeight / 2;
    const int bitCount = static_cast<int>(ReadUInt16(resData, hotspotOffset + 14));

    int paletteEntries = 0;
    if (bitCount <= 8)
    {
        paletteEntries = static_cast<int>(ReadUInt32(resData, hotspotOffset + 32));
        if (paletteEntries == 0)
            paletteEntries = 1 << bitCount;
    }

    if (width <= 0 || height <= 0 || bitCount == 0)
    {
        qDebug() << "Invalid bitmap dimensions or bit count.";

        Img legacyFallback;
        if (Win12MonochromeResource::TryDecode(
                resData,
                0,
                legacyFallback))
        {
            return legacyFallback;
        }

        return EmptyImage();
    }

    const qint64 pixelDataOffset = hotspotOffset + qint64(biSize) + qint64(paletteEntries) * 4;
    const qint64 colorStride = ((qint64(width) * bitCount + 31) / 32) * 4;
    const qint64 maskStride = ((qint64(width) + 31) / 32) * 4;
    const qint64 maskDataOffset = pixelDataOffset + colorStride * height;

    const qint64 pixelDataLength = colorStride * height;
    const qint64 maskDataLength = maskStride * height;

    if (pixelDataOffset < 0 || maskDataOffset < 0
        || pixelDataOffset + pixelDataLength > resData.size()
        || maskDataOffset + maskDataLength > resData.size())
    {
        return EmptyImage();
    }

    // Palette
    QVector<QRgb> palette;
    if (bitCount <= 8)
    {
        palette.reserve(paletteEntries);
        const int paletteStart = hotspotOffset + biSize;

        for (int j = 0; j < paletteEntries; ++j)
        {
            const int entryOffset = paletteStart + j * 4;
            if (entryOffset + 3 >= resData.size())
                break;

            const quint8 blue = static_cast<quint8>(resData.at(entryOffset));
            const quint8 green = static_cast<quint8>(resData.at(entryOffset + 1));
            const quint8 red = static_cast<quint8>(resData.at(entryOffset + 2));
            palette.append(qRgba(red, green, blue, 255));
        }
    }

    // Bitmap data
    const QByteArray pixelData = resData.mid(pixelDataOffset, pixelDataLength);
    const QByteArray maskData = resData.mid(maskDataOffset, maskDataLength);

    const Optional<Img> image = RT_BITMAP::GenerateBitmapFromData(
        pixelData,
        maskData,
        width,
        height,
        bitCount,
        palette);

    return image.value_or(EmptyImage());
}

} // namespace

QVector<QImage> RT_CURSOR::decode(const QByteArray& data)
{
    const Img image = Get(data);
    return image.Size.isEmpty() ? QVector<QImage>{} : QVector<QImage>{image.Bitmap};
}

ResourcePreview RT_CURSOR::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.images = decode(entry.data);

    if (preview.images.isEmpty())
        preview.error = QStringLiteral("Cursor preview failed");

    return preview;
}

} // namespace resources
} // namespace peare
