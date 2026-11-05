#include "SiemensMwImage.h"

#include <QImage>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace peare { namespace resources {
namespace {

constexpr int kHeaderSize = 44;

quint32 read32(const QByteArray& data, int off, bool be)
{
    if (off < 0 || off + 4 > data.size()) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + off);
    return be ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
}

struct Header {
    bool be = false;
    int width = 0;
    int height = 0;
    int bpp = 0;
    int pitch = 0;
};

bool parseHeader(const QByteArray& data, bool be, Header* result)
{
    if (data.size() < kHeaderSize) return false;
    const quint32 width = read32(data, 0, be);
    const quint32 height = read32(data, 4, be);
    const quint32 planes = read32(data, 8, be);
    const quint32 bpp = read32(data, 12, be);
    const quint32 pitch = read32(data, 16, be);
    const quint32 paletteSize = read32(data, 28, be);
    const quint32 palettePointer = read32(data, 36, be);
    if (width < 1 || width > 4096 || height < 1 || height > 4096 || planes != 1)
        return false;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 &&
        bpp != 16 && bpp != 24 && bpp != 32)
        return false;
    const quint64 minimumPitch = (quint64(width) * bpp + 7U) / 8U;
    if (pitch < minimumPitch || pitch > minimumPitch + 64U || pitch > 16384U)
        return false;
    if (paletteSize != 0 || palettePointer != 0) return false;
    const quint64 bytes = quint64(pitch) * height;
    if (bytes > quint64(data.size() - kHeaderSize)) return false;
    if (result) {
        result->be = be;
        result->width = int(width);
        result->height = int(height);
        result->bpp = int(bpp);
        result->pitch = int(pitch);
    }
    return true;
}

bool parseHeader(const QByteArray& data, Header* result)
{
    Header le, be;
    const bool okLe = parseHeader(data, false, &le);
    const bool okBe = parseHeader(data, true, &be);
    if (okLe == okBe) {
        if (!okLe) return false;
        // Siemens images are unambiguous in practice; when both byte orders look
        // plausible choose the one whose width/height are smaller.
        *result = (le.width + le.height <= be.width + be.height) ? le : be;
        return true;
    }
    *result = okLe ? le : be;
    return true;
}

int indexedPixel(const uchar* row, int x, int bpp)
{
    if (bpp == 8) return row[x];
    if (bpp == 4) return (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F;
    if (bpp == 2) return (row[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03;
    return (row[x >> 3] >> (7 - (x & 7))) & 0x01;
}

QImage decodeIndexed(const QByteArray& data, const Header& h)
{
    QImage image(h.width, h.height, QImage::Format_Grayscale8);
    if (image.isNull()) return {};
    const uchar* pixels = reinterpret_cast<const uchar*>(data.constData() + kHeaderSize);
    const int maxValue = (1 << h.bpp) - 1;
    for (int y = 0; y < h.height; ++y) {
        const uchar* src = pixels + y * h.pitch;
        uchar* dst = image.scanLine(y);
        for (int x = 0; x < h.width; ++x) {
            const int v = indexedPixel(src, x, h.bpp);
            dst[x] = h.bpp == 8 ? uchar(v) : uchar((v * 255 + maxValue / 2) / maxValue);
        }
    }
    return image;
}

QImage decodeTrueColor(const QByteArray& data, const Header& h)
{
    QImage image(h.width, h.height, QImage::Format_ARGB32);
    if (image.isNull()) return {};
    const uchar* pixels = reinterpret_cast<const uchar*>(data.constData() + kHeaderSize);
    for (int y = 0; y < h.height; ++y) {
        const uchar* src = pixels + y * h.pitch;
        QRgb* dst = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < h.width; ++x) {
            if (h.bpp == 16) {
                const uchar* p = src + x * 2;
                const quint16 v = h.be ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
                const int r = ((v >> 11) & 31) * 255 / 31;
                const int g = ((v >> 5) & 63) * 255 / 63;
                const int b = (v & 31) * 255 / 31;
                dst[x] = qRgb(r, g, b);
            } else if (h.bpp == 24) {
                const uchar* p = src + x * 3;
                dst[x] = qRgb(p[0], p[1], p[2]);
            } else {
                const uchar* p = src + x * 4;
                dst[x] = qRgba(p[0], p[1], p[2], p[3]);
            }
        }
    }
    return image;
}

} // namespace

ResourcePreview SiemensMwImage::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    Header h;
    if (!parseHeader(entry.data, &h)) {
        result.error = QStringLiteral("Invalid Siemens/Microwindows MWIMAGEHDR resource");
        return result;
    }

    QImage image = h.bpp <= 8 ? decodeIndexed(entry.data, h) : decodeTrueColor(entry.data, h);
    if (image.isNull()) {
        result.error = QStringLiteral("Cannot decode Siemens/Microwindows image");
        return result;
    }
    result.images.append(image);
    result.imageLabels.append(QStringLiteral("%1x%2, %3 bpp").arg(h.width).arg(h.height).arg(h.bpp));
    result.conversionImages.append(image);
    result.image = image;
    return result;
}

} } // namespace peare::resources
