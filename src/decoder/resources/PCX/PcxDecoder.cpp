#include "PcxDecoder.h"

#include <QtGlobal>
#include <QVector>
#include <climits>

namespace peare {
namespace resources {
namespace {

// Ported and hardened from Raul Merelli's Apache-2.0 C# PCX importer.
// The original implementation supplied the decoding model for palettes,
// planar pixels, scan-line padding and ZSoft RLE.

constexpr int kHeaderSize = 128;
constexpr quint64 kMaximumPixels = 64ULL * 1024ULL * 1024ULL;

quint16 readLe16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) |
           (quint16(quint8(data.at(offset + 1))) << 8);
}

struct Header {
    quint8 version = 0;
    quint8 encoding = 0;
    quint8 bitsPerPixel = 0;
    quint16 xMin = 0;
    quint16 yMin = 0;
    quint16 xMax = 0;
    quint16 yMax = 0;
    quint16 hDpi = 0;
    quint16 vDpi = 0;
    quint8 planes = 0;
    quint16 bytesPerLine = 0;
    quint16 paletteInfo = 0;
    int width = 0;
    int height = 0;
};

bool parseHeader(const QByteArray& data, Header& header, QString* error)
{
    if (data.size() < kHeaderSize) {
        if (error) *error = QStringLiteral("PCX header is truncated");
        return false;
    }
    if (quint8(data.at(0)) != 0x0A) {
        if (error) *error = QStringLiteral("Not a ZSoft PCX image");
        return false;
    }

    header.version = quint8(data.at(1));
    header.encoding = quint8(data.at(2));
    header.bitsPerPixel = quint8(data.at(3));
    header.xMin = readLe16(data, 4);
    header.yMin = readLe16(data, 6);
    header.xMax = readLe16(data, 8);
    header.yMax = readLe16(data, 10);
    header.hDpi = readLe16(data, 12);
    header.vDpi = readLe16(data, 14);
    header.planes = quint8(data.at(65));
    header.bytesPerLine = readLe16(data, 66);
    header.paletteInfo = readLe16(data, 68);

    if (header.version != 0 && header.version != 2 &&
        header.version != 3 && header.version != 4 && header.version != 5) {
        if (error) *error = QStringLiteral("Unsupported PCX version");
        return false;
    }
    if (header.encoding > 1) {
        if (error) *error = QStringLiteral("Unsupported PCX encoding");
        return false;
    }
    if (header.bitsPerPixel != 1 && header.bitsPerPixel != 2 &&
        header.bitsPerPixel != 4 && header.bitsPerPixel != 8) {
        if (error) *error = QStringLiteral("Unsupported PCX bits-per-plane value");
        return false;
    }
    if (header.planes < 1 || header.planes > 4 || header.bytesPerLine == 0 ||
        header.xMax < header.xMin || header.yMax < header.yMin) {
        if (error) *error = QStringLiteral("Invalid PCX dimensions or plane layout");
        return false;
    }

    header.width = int(header.xMax) - int(header.xMin) + 1;
    header.height = int(header.yMax) - int(header.yMin) + 1;
    const quint64 pixels = quint64(header.width) * quint64(header.height);
    if (header.width <= 0 || header.height <= 0 || pixels > kMaximumPixels) {
        if (error) *error = QStringLiteral("PCX dimensions are outside safe limits");
        return false;
    }

    const quint64 minimumPlaneBytes =
        (quint64(header.width) * header.bitsPerPixel + 7u) / 8u;
    if (header.bytesPerLine < minimumPlaneBytes) {
        if (error) *error = QStringLiteral("PCX scan line is shorter than the image width");
        return false;
    }

    const bool supportedLayout =
        header.planes == 1 ||
        (header.bitsPerPixel == 1 && header.planes <= 4) ||
        (header.bitsPerPixel == 8 && (header.planes == 3 || header.planes == 4));
    if (!supportedLayout) {
        if (error) *error = QStringLiteral("Unsupported PCX plane layout");
        return false;
    }
    return true;
}

QVector<QRgb> defaultPalette16()
{
    QVector<QRgb> palette;
    palette.reserve(16);
    palette << qRgb(0, 0, 0) << qRgb(0, 0, 255)
            << qRgb(0, 255, 0) << qRgb(0, 255, 255)
            << qRgb(255, 0, 0) << qRgb(255, 0, 255)
            << qRgb(255, 255, 0) << qRgb(255, 255, 255);
    while (palette.size() < 16)
        palette.append(qRgb(0, 0, 0));
    return palette;
}

QVector<QRgb> headerPalette(const QByteArray& data, quint8 version, bool* empty)
{
    QVector<QRgb> palette;
    palette.reserve(16);
    bool allZero = true;
    for (int index = 0; index < 16; ++index) {
        const int offset = 16 + index * 3;
        const int red = quint8(data.at(offset));
        const int green = quint8(data.at(offset + 1));
        const int blue = quint8(data.at(offset + 2));
        if (red != 0 || green != 0 || blue != 0)
            allZero = false;
        palette.append(qRgb(red, green, blue));
    }

    // PC Paintbrush 2.8 without palette information and the Windows variant
    // conventionally use the built-in EGA palette, matching the old importer.
    if (version == 3 || version == 4 || allZero)
        palette = defaultPalette16();
    if (empty) *empty = allZero;
    return palette;
}

QVector<QRgb> trailingPalette256(const QByteArray& data, int* imageDataEnd)
{
    QVector<QRgb> palette;
    if (data.size() < kHeaderSize + 769)
        return palette;

    const int marker = data.size() - 769;
    if (quint8(data.at(marker)) != 0x0C)
        return palette;

    palette.reserve(256);
    for (int index = 0; index < 256; ++index) {
        const int offset = marker + 1 + index * 3;
        palette.append(qRgb(quint8(data.at(offset)),
                            quint8(data.at(offset + 1)),
                            quint8(data.at(offset + 2))));
    }
    if (imageDataEnd) *imageDataEnd = marker;
    return palette;
}

bool decodeScanLines(const QByteArray& data, const Header& header, int dataEnd,
                     QByteArray& decoded, int* completeRows, QString* error)
{
    const quint64 rowSize64 = quint64(header.planes) * header.bytesPerLine;
    const quint64 expected64 = rowSize64 * quint64(header.height);
    if (rowSize64 == 0 || expected64 > quint64(INT_MAX)) {
        if (error) *error = QStringLiteral("PCX decoded raster is too large");
        return false;
    }

    const int rowSize = int(rowSize64);
    const int expected = int(expected64);
    decoded.clear();
    decoded.reserve(expected);

    int position = kHeaderSize;
    if (header.encoding == 0) {
        const int available = qMax(0, dataEnd - position);
        decoded.append(data.constData() + position, qMin(expected, available));
    } else {
        while (position < dataEnd && decoded.size() < expected) {
            const quint8 token = quint8(data.at(position++));
            int count = 1;
            quint8 value = token;
            if ((token & 0xC0u) == 0xC0u) {
                count = int(token & 0x3Fu);
                if (count == 0 || position >= dataEnd) {
                    if (error) *error = QStringLiteral("Invalid or truncated PCX RLE run");
                    break;
                }
                value = quint8(data.at(position++));
            }
            const int appendCount = qMin(count, expected - decoded.size());
            decoded.append(appendCount, char(value));
        }
    }

    const int rows = decoded.size() / rowSize;
    if (completeRows) *completeRows = rows;
    if (rows <= 0) {
        if (error && error->isEmpty())
            *error = QStringLiteral("PCX pixel data is truncated");
        return false;
    }

    // A partially damaged final image is still useful. Missing rows remain
    // transparent, while complete rows are decoded exactly.
    if (decoded.size() < expected)
        decoded.resize(rows * rowSize);
    return true;
}

quint8 packedIndex(const uchar* plane, int x, int bitsPerPixel)
{
    const int bitOffset = x * bitsPerPixel;
    const int byteIndex = bitOffset / 8;
    const int shift = 8 - bitsPerPixel - (bitOffset % 8);
    return quint8((plane[byteIndex] >> shift) & ((1u << bitsPerPixel) - 1u));
}

QRgb paletteColor(const QVector<QRgb>& palette, int index)
{
    return index >= 0 && index < palette.size() ? palette.at(index)
                                                 : qRgb(0, 0, 0);
}

} // namespace

bool PcxDecoder::LooksLike(const QByteArray& data) noexcept
{
    try {
        Header header;
        return parseHeader(data, header, nullptr);
    } catch (...) {
        return false;
    }
}

bool PcxDecoder::TryDecode(const QByteArray& data, QImage& image,
                           QString* error) noexcept
{
    image = QImage();
    if (error) error->clear();

    try {
        Header header;
        if (!parseHeader(data, header, error))
            return false;

        bool headerPaletteWasEmpty = false;
        const QVector<QRgb> palette16 =
            headerPalette(data, header.version, &headerPaletteWasEmpty);
        int imageDataEnd = data.size();
        QVector<QRgb> palette256;
        if (header.planes == 1 && header.bitsPerPixel == 8)
            palette256 = trailingPalette256(data, &imageDataEnd);

        QByteArray raster;
        int completeRows = 0;
        if (!decodeScanLines(data, header, imageDataEnd, raster,
                             &completeRows, error))
            return false;

        image = QImage(header.width, header.height, QImage::Format_ARGB32);
        if (image.isNull()) {
            if (error) *error = QStringLiteral("Unable to allocate the PCX image");
            return false;
        }
        image.fill(Qt::transparent);

        const int rowSize = int(header.planes) * int(header.bytesPerLine);
        const bool grayscale = header.paletteInfo == 2;

        for (int y = 0; y < completeRows; ++y) {
            const uchar* row = reinterpret_cast<const uchar*>(raster.constData() + y * rowSize);
            QRgb* target = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < header.width; ++x) {
                QRgb color = qRgb(0, 0, 0);

                if (header.planes == 1) {
                    if (header.bitsPerPixel == 8) {
                        const int value = row[x];
                        if (!palette256.isEmpty())
                            color = palette256.at(value);
                        else if (!grayscale && !headerPaletteWasEmpty)
                            color = paletteColor(palette16, value / 16);
                        else
                            color = qRgb(value, value, value);
                    } else if (header.bitsPerPixel == 1) {
                        // Match the original C# importer exactly: its
                        // generateBitmap() creates a Format1bppIndexed Bitmap
                        // and copies the decoded PCX bits without assigning the
                        // header palette. System.Drawing therefore uses the
                        // bitmap's default black/white palette.
                        const int index = packedIndex(row, x, 1);
                        color = index == 0 ? qRgb(0, 0, 0)
                                           : qRgb(255, 255, 255);
                    } else {
                        const int index = packedIndex(row, x, header.bitsPerPixel);
                        color = paletteColor(palette16, index);
                    }
                } else if (header.bitsPerPixel == 1) {
                    int index = 0;
                    for (int plane = 0; plane < header.planes; ++plane) {
                        const uchar* planeBytes = row + plane * header.bytesPerLine;
                        const int bit = (planeBytes[x / 8] >> (7 - (x & 7))) & 1;
                        index |= bit << plane;
                    }
                    color = paletteColor(palette16, index);
                } else if (header.planes == 3 && header.bitsPerPixel == 8) {
                    color = qRgb(row[x],
                                 row[header.bytesPerLine + x],
                                 row[header.bytesPerLine * 2 + x]);
                } else if (header.planes == 4 && header.bitsPerPixel == 8) {
                    // Four 8-bit PCX planes are conventionally CMYK.
                    const int cyan = row[x];
                    const int magenta = row[header.bytesPerLine + x];
                    const int yellow = row[header.bytesPerLine * 2 + x];
                    const int black = row[header.bytesPerLine * 3 + x];
                    const int red = ((255 - cyan) * (255 - black) + 127) / 255;
                    const int green = ((255 - magenta) * (255 - black) + 127) / 255;
                    const int blue = ((255 - yellow) * (255 - black) + 127) / 255;
                    color = qRgb(red, green, blue);
                }

                target[x] = color;
            }
        }

        if (header.hDpi > 0)
            image.setDotsPerMeterX(qRound(double(header.hDpi) / 0.0254));
        if (header.vDpi > 0)
            image.setDotsPerMeterY(qRound(double(header.vDpi) / 0.0254));

        return true;
    } catch (...) {
        image = QImage();
        if (error) *error = QStringLiteral("Unexpected error while decoding PCX");
        return false;
    }
}

} // namespace resources
} // namespace peare
