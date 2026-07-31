#include "XBE_LOGO_RLE.h"

#include <QImage>

namespace peare {
namespace resources {
namespace {
ResourcePreview failure(const QString& message) { ResourcePreview r; r.error = message; return r; }
}

ResourcePreview XBE_LOGO_RLE::preview(const ResourceEntry& entry)
{
    constexpr int width = 100;
    constexpr int height = 17;
    constexpr int pixelCount = width * height;

    QByteArray pixels(pixelCount, char(0));
    int output = 0;
    for (qsizetype i = 0; i < entry.data.size() && output < pixelCount; ++i) {
        const quint8 first = quint8(uchar(entry.data.at(int(i))));
        int length = 0;
        quint8 level = 0;
        if (first & 0x01u) {
            length = (first >> 1) & 0x07;
            level = (first >> 4) & 0x0f;
        } else {
            if (i + 1 >= entry.data.size()) return failure(QStringLiteral("Truncated XBE logo RLE record"));
            const quint16 value = quint16(first) | (quint16(uchar(entry.data.at(int(++i)))) << 8);
            if (value & 0x0002u) return failure(QStringLiteral("Invalid XBE logo RLE record"));
            length = (value >> 2) & 0x03ff;
            level = (value >> 12) & 0x0f;
        }
        if (length <= 0 || output + length > pixelCount)
            return failure(QStringLiteral("Invalid XBE logo RLE run length"));
        const char gray = char(level << 4);
        for (int n = 0; n < length; ++n) pixels[output++] = gray;
    }
    if (output != pixelCount)
        return failure(QStringLiteral("XBE logo RLE does not contain 1700 pixels"));

    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull()) return failure(QStringLiteral("Unable to allocate XBE logo image"));
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int gray = quint8(pixels.at(y * width + x));
            row[x] = qRgba(gray, gray, gray, 255);
        }
    }

    ResourcePreview result;
    result.images.append(image);
    result.imageLabels.append(QStringLiteral("Xbox logo"));
    result.conversionImages.append(image);
    result.image = image;
    return result;
}

} // namespace resources
} // namespace peare
