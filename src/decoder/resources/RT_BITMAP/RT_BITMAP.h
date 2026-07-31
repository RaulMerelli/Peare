#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "../../Optional.h"

#include <QSize>
#include <QVector>


namespace peare {
namespace resources {

struct Img
{
    QImage Bitmap;
    int BitCount = 0;
    QSize Size;
};

class RT_BITMAP {
public:
    static ResourcePreview preview(const ResourceEntry& entry);
    static QVector<QImage> get(const QByteArray& data);
    static QVector<Img> getDetailed(const QByteArray& data);

    static Optional<Img> GenerateBitmapFromData(
        const QByteArray& pixelData,
        const QByteArray& maskData,
        int width,
        int height,
        int bitCount,
        QVector<QRgb> palette);
};

} // namespace resources
} // namespace peare
