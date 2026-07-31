#include "RT_ICON.h"

#include "../RT_BITMAP/RT_BITMAP.h"
#include "Win12MonochromeResource.h"

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

QImage EmptyImage()
{
    return {};
}

QImage Get_ICON_Win1_Win2(const QByteArray& resData)
{
    Img image;
    if (Win12MonochromeResource::TryDecode(
            resData,
            0,
            image))
    {
        return image.Bitmap;
    }

    qDebug() << "Error: data is not a valid Windows 1.x/2.x icon resource.";
    return EmptyImage();
}

QImage Get(const QByteArray& resData)
{
    if (resData.size() < 14)
        return EmptyImage();

    Img legacyImage;
    if (Win12MonochromeResource::TryDecode(
            resData,
            0,
            legacyImage))
    {
        return legacyImage.Bitmap;
    }

    if (resData.size() > 4
        && static_cast<uchar>(resData.at(0)) == 0x89
        && static_cast<uchar>(resData.at(1)) == 0x50
        && static_cast<uchar>(resData.at(2)) == 0x4E
        && static_cast<uchar>(resData.at(3)) == 0x47)
    {
        // PNG format
        QImage image;
        image.loadFromData(resData, "PNG");
        return image.isNull() ? EmptyImage() : image;
    }

    if (resData.size() < 16)
        return EmptyImage();

    const int biSize = static_cast<int>(ReadUInt32(resData, 0));
    const int width = static_cast<int>(ReadUInt32(resData, 4));
    const int fullHeight = static_cast<int>(ReadUInt32(resData, 8));
    const int height = fullHeight / 2;
    const int bitCount = static_cast<int>(ReadUInt16(resData, 14));

    if (width <= 0 || height <= 0 || bitCount == 0)
    {
        qDebug() << "Invalid bitmap dimensions or bit count.";
        return Get_ICON_Win1_Win2(resData);
    }

    int paletteEntries = 0;
    if (bitCount <= 8)
    {
        paletteEntries = static_cast<int>(ReadUInt32(resData, 32));
        if (paletteEntries == 0)
            paletteEntries = 1 << bitCount;
    }

    const qint64 pixelDataOffset = qint64(biSize) + qint64(paletteEntries) * 4;
    const qint64 colorStride = ((qint64(width) * bitCount + 31) / 32) * 4;
    const qint64 maskStride = ((qint64(width) + 31) / 32) * 4;
    const qint64 maskDataOffset = pixelDataOffset + colorStride * height;

    const qint64 pixelDataLength = colorStride * height;
    const qint64 maskDataLength = maskStride * height;

    if (pixelDataOffset < 0
        || maskDataOffset < 0
        || pixelDataOffset + pixelDataLength > resData.size()
        || maskDataOffset + maskDataLength > resData.size())
    {
        qDebug() << "Data does not contain enough bytes for pixel or mask data.";
        return Get_ICON_Win1_Win2(resData);
    }

    // Extract palette
    QVector<QRgb> palette;
    if (bitCount <= 8)
    {
        palette.reserve(paletteEntries);
        const int paletteStart = biSize;

        for (int j = 0; j < paletteEntries; ++j)
        {
            const int entryOffset = paletteStart + j * 4;
            if (entryOffset + 3 >= resData.size())
                break;

            const uchar blue = static_cast<uchar>(resData.at(entryOffset));
            const uchar green = static_cast<uchar>(resData.at(entryOffset + 1));
            const uchar red = static_cast<uchar>(resData.at(entryOffset + 2));
            palette.append(qRgba(red, green, blue, 255));
        }
    }

    // Allocate and copy large image buffers
    const QByteArray pixelData = resData.mid(pixelDataOffset, pixelDataLength);
    const QByteArray maskData = resData.mid(maskDataOffset, maskDataLength);

    const Optional<Img> image = RT_BITMAP::GenerateBitmapFromData(
        pixelData,
        maskData,
        width,
        height,
        bitCount,
        palette);

    return image.has_value() ? image->Bitmap : EmptyImage();
}

} // namespace

QVector<QImage> RT_ICON::decode(const QByteArray& data)
{
    const QImage image = Get(data);
    return image.isNull() ? QVector<QImage>{} : QVector<QImage>{image};
}

ResourcePreview RT_ICON::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.images = decode(entry.data);

    if (preview.images.isEmpty())
        preview.error = QStringLiteral("Icon preview failed");

    return preview;
}

} // namespace resources
} // namespace peare
