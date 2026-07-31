#include "Win12MonochromeResource.h"

#include <QDebug>
#include <QtEndian>
#include <limits>

namespace peare {
namespace resources {
namespace {

quint16 ReadUInt16(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;

    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(data.constData() + offset));
}

} // namespace

bool Win12MonochromeResource::LooksLike(const QByteArray& data)
{
    int headerOffset = -1;
    return TryLocateHeader(data, 0, headerOffset);
}

bool Win12MonochromeResource::TryDecode(
    const QByteArray& data,
    quint8 expectedFigure,
    Img& image)
{
    image = Img{};

    int headerOffset = -1;
    if (!TryLocateHeader(data, expectedFigure, headerOffset))
        return false;

    const quint8 figure = static_cast<quint8>(data.at(headerOffset));
    const quint16 width = ReadUInt16(data, headerOffset + 6);
    const quint16 height = ReadUInt16(data, headerOffset + 8);
    const quint16 bytesPerLine = ReadUInt16(data, headerOffset + 10);

    const qint64 planeSize64 = qint64(bytesPerLine) * height;
    if (planeSize64 > std::numeric_limits<int>::max())
        return false;

    const int planeSize = static_cast<int>(planeSize64);
    const int andOffset = headerOffset + HeaderSize;
    const int xorOffset = andOffset + planeSize;

    QImage bitmap(width, height, QImage::Format_ARGB32);
    if (bitmap.isNull())
        return false;

    for (int y = 0; y < height; ++y)
    {
        const int rowOffset = y * bytesPerLine;
        QRgb* scanLine = reinterpret_cast<QRgb*>(bitmap.scanLine(y));

        for (int x = 0; x < width; ++x)
        {
            const int byteIndex = rowOffset + (x >> 3);
            const int bitIndex = 7 - (x & 7);

            const bool andBit =
                ((static_cast<quint8>(data.at(andOffset + byteIndex)) >> bitIndex) & 1) != 0;
            const bool xorBit =
                ((static_cast<quint8>(data.at(xorOffset + byteIndex)) >> bitIndex) & 1) != 0;

            QRgb pixel;
            if (!andBit)
            {
                // AND=0: output does not depend on the destination.
                pixel = xorBit ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 255);
            }
            else if (!xorBit)
            {
                // AND=1, XOR=0: preserve destination => transparent.
                pixel = qRgba(0, 0, 0, 0);
            }
            else
            {
                // AND=1, XOR=1: invert destination. ARGB has no
                // destination-dependent pixel, therefore use a visible
                // neutral preview value without changing Img's API.
                pixel = qRgba(128, 128, 128, 255);
            }

            scanLine[x] = pixel;
        }
    }

    image.Bitmap = bitmap;
    image.BitCount = bitmap.depth(); // This describes the returned bitmap, not the source planes.
    image.Size = QSize(width, height);

    qDebug().noquote()
        << QStringLiteral("Decoded Windows 1.x/2.x %1: %2x%3, stride=%4, headerOffset=%5.")
               .arg(figure == FigureCursor ? QStringLiteral("cursor") : QStringLiteral("icon"))
               .arg(width)
               .arg(height)
               .arg(bytesPerLine)
               .arg(headerOffset);

    return true;
}

bool Win12MonochromeResource::TryLocateHeader(
    const QByteArray& data,
    quint8 expectedFigure,
    int& headerOffset)
{
    headerOffset = -1;

    // Normal Win1/Win2 resource payload.
    if (IsValidHeaderAt(data, 0, expectedFigure))
    {
        headerOffset = 0;
        return true;
    }

    // Be tolerant of cursor payloads that have already acquired the
    // later four-byte hotspot prefix before the historical header.
    // This is not the canonical Win1/Win2 layout, but accepting it makes
    // extraction paths and standalone resource dumps interoperable.
    if (IsValidHeaderAt(data, 4, expectedFigure))
    {
        headerOffset = 4;
        return true;
    }

    return false;
}

bool Win12MonochromeResource::IsValidHeaderAt(
    const QByteArray& data,
    int offset,
    quint8 expectedFigure)
{
    if (offset < 0 || data.size() - offset < HeaderSize)
        return false;

    const quint8 figure = static_cast<quint8>(data.at(offset));
    const quint8 independent = static_cast<quint8>(data.at(offset + 1));

    if (figure != FigureCursor && figure != FigureIcon)
        return false;
    if (expectedFigure != 0 && figure != expectedFigure)
        return false;
    if (independent != 0 && independent != 1)
        return false;

    const quint16 width = ReadUInt16(data, offset + 6);
    const quint16 height = ReadUInt16(data, offset + 8);
    const quint16 bytesPerLine = ReadUInt16(data, offset + 10);
    const quint16 colorPlanes = ReadUInt16(data, offset + 12);

    if (width == 0 || height == 0
        || width > MaximumDimension || height > MaximumDimension
        || bytesPerLine == 0 || colorPlanes != 0)
    {
        return false;
    }

    const int minimumStride = (width + 7) / 8;
    if (bytesPerLine < minimumStride || (bytesPerLine & 1) != 0)
        return false;

    const qint64 planeSize = qint64(bytesPerLine) * height;
    const qint64 requiredSize = qint64(offset) + HeaderSize + planeSize * 2;
    return requiredSize <= data.size();
}

} // namespace resources
} // namespace peare
