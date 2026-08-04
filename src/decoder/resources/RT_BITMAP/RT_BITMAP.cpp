#include "RT_BITMAP.h"

#include <QBuffer>
#include <QDebug>
#include <QImageReader>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <utility>

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

qint32 ReadInt32(const QByteArray& data, qsizetype offset)
{
    return static_cast<qint32>(ReadUInt32(data, offset));
}

bool IsRangeValid(const QByteArray& data, quint64 offset, quint64 count)
{
    return offset <= static_cast<quint64>(data.size())
        && count <= static_cast<quint64>(data.size()) - offset;
}

QVector<QRgb> ReadPalette(
    const QByteArray& data,
    qsizetype offset,
    int colorCount,
    int entrySize)
{
    QVector<QRgb> palette;

    if (colorCount < 0
        || entrySize < 3
        || !IsRangeValid(
            data,
            static_cast<quint64>(offset),
            static_cast<quint64>(colorCount) * entrySize))
    {
        return palette;
    }

    palette.reserve(colorCount);

    for (int i = 0; i < colorCount; ++i)
    {
        const qsizetype entryOffset = offset + static_cast<qsizetype>(i) * entrySize;
        const uchar blue = static_cast<uchar>(data.at(entryOffset));
        const uchar green = static_cast<uchar>(data.at(entryOffset + 1));
        const uchar red = static_cast<uchar>(data.at(entryOffset + 2));

        palette.append(qRgba(red, green, blue, 255));
    }

    return palette;
}


struct FaxCode { const char* bits; int run; };

static const FaxCode kWhiteFaxCodes[] = {
{"00110101",0},{"000111",1},{"0111",2},{"1000",3},{"1011",4},{"1100",5},{"1110",6},{"1111",7},
{"10011",8},{"10100",9},{"00111",10},{"01000",11},{"001000",12},{"000011",13},{"110100",14},{"110101",15},
{"101010",16},{"101011",17},{"0100111",18},{"0001100",19},{"0001000",20},{"0010111",21},{"0000011",22},{"0000100",23},
{"0101000",24},{"0101011",25},{"0010011",26},{"0100100",27},{"0011000",28},{"00000010",29},{"00000011",30},{"00011010",31},
{"00011011",32},{"00010010",33},{"00010011",34},{"00010100",35},{"00010101",36},{"00010110",37},{"00010111",38},{"00101000",39},
{"00101001",40},{"00101010",41},{"00101011",42},{"00101100",43},{"00101101",44},{"00000100",45},{"00000101",46},{"00001010",47},
{"00001011",48},{"01010010",49},{"01010011",50},{"01010100",51},{"01010101",52},{"00100100",53},{"00100101",54},{"01011000",55},
{"01011001",56},{"01011010",57},{"01011011",58},{"01001010",59},{"01001011",60},{"00110010",61},{"00110011",62},{"00110100",63},
{"11011",64},{"10010",128},{"010111",192},{"0110111",256},{"00110110",320},{"00110111",384},{"01100100",448},{"01100101",512},
{"01101000",576},{"01100111",640},{"011001100",704},{"011001101",768},{"011010010",832},{"011010011",896},{"011010100",960},{"011010101",1024},
{"011010110",1088},{"011010111",1152},{"011011000",1216},{"011011001",1280},{"011011010",1344},{"011011011",1408},{"010011000",1472},{"010011001",1536},
{"010011010",1600},{"011000",1664},{"010011011",1728},{"00000001000",1792},{"00000001100",1856},{"00000001101",1920},{"000000010010",1984},{"000000010011",2048},
{"000000010100",2112},{"000000010101",2176},{"000000010110",2240},{"000000010111",2304},{"000000011100",2368},{"000000011101",2432},{"000000011110",2496},{"000000011111",2560}
};

static const FaxCode kBlackFaxCodes[] = {
{"0000110111",0},{"010",1},{"11",2},{"10",3},{"011",4},{"0011",5},{"0010",6},{"00011",7},
{"000101",8},{"000100",9},{"0000100",10},{"0000101",11},{"0000111",12},{"00000100",13},{"00000111",14},{"000011000",15},
{"0000010111",16},{"0000011000",17},{"0000001000",18},{"00001100111",19},{"00001101000",20},{"00001101100",21},{"00000110111",22},{"00000101000",23},
{"00000010111",24},{"00000011000",25},{"000011001010",26},{"000011001011",27},{"000011001100",28},{"000011001101",29},{"000001101000",30},{"000001101001",31},
{"000001101010",32},{"000001101011",33},{"000011010010",34},{"000011010011",35},{"000011010100",36},{"000011010101",37},{"000011010110",38},{"000011010111",39},
{"000001101100",40},{"000001101101",41},{"000011011010",42},{"000011011011",43},{"000001010100",44},{"000001010101",45},{"000001010110",46},{"000001010111",47},
{"000001100100",48},{"000001100101",49},{"000001010010",50},{"000001010011",51},{"000000100100",52},{"000000110111",53},{"000000111000",54},{"000000100111",55},
{"000000101000",56},{"000001011000",57},{"000001011001",58},{"000000101011",59},{"000000101100",60},{"000001011010",61},{"000001100110",62},{"000001100111",63},
{"0000001111",64},{"000011001000",128},{"000011001001",192},{"000001011011",256},{"000000110011",320},{"000000110100",384},{"000000110101",448},{"0000001101100",512},
{"0000001101101",576},{"0000001001010",640},{"0000001001011",704},{"0000001001100",768},{"0000001001101",832},{"0000001110010",896},{"0000001110011",960},{"0000001110100",1024},
{"0000001110101",1088},{"0000001110110",1152},{"0000001110111",1216},{"0000001010010",1280},{"0000001010011",1344},{"0000001010100",1408},{"0000001010101",1472},{"0000001011010",1536},
{"0000001011011",1600},{"0000001100100",1664},{"0000001100101",1728},{"00000001000",1792},{"00000001100",1856},{"00000001101",1920},{"000000010010",1984},{"000000010011",2048},
{"000000010100",2112},{"000000010101",2176},{"000000010110",2240},{"000000010111",2304},{"000000011100",2368},{"000000011101",2432},{"000000011110",2496},{"000000011111",2560}
};

class FaxBitReader {
public:
    explicit FaxBitReader(const QByteArray& data) : data_(data) {}
    bool readBit(int& bit) {
        if (bitPos_ >= data_.size() * 8) return false;
        const uchar byte = uchar(data_.at(bitPos_ / 8));
        bit = (byte >> (7 - (bitPos_ & 7))) & 1;
        ++bitPos_;
        return true;
    }
    qsizetype position() const { return bitPos_; }
    void setPosition(qsizetype p) { bitPos_ = p; }
private:
    const QByteArray& data_;
    qsizetype bitPos_ = 0;
};

int DecodeFaxCode(FaxBitReader& reader, bool black, bool& eol)
{
    eol = false;
    char bits[14] = {};
    int length = 0;
    while (length < 13) {
        int bit = 0;
        if (!reader.readBit(bit)) return -2;
        bits[length++] = bit ? '1' : '0';
        bits[length] = 0;
        if (length == 12 && std::strcmp(bits, "000000000001") == 0) {
            eol = true;
            return 0;
        }
        const FaxCode* table = black ? kBlackFaxCodes : kWhiteFaxCodes;
        const size_t count = black
            ? sizeof(kBlackFaxCodes) / sizeof(kBlackFaxCodes[0])
            : sizeof(kWhiteFaxCodes) / sizeof(kWhiteFaxCodes[0]);
        for (size_t i = 0; i < count; ++i) {
            if (int(std::strlen(table[i].bits)) == length && std::strcmp(bits, table[i].bits) == 0)
                return table[i].run;
        }
    }
    return -1;
}

QByteArray DecompressHuffman1D(const QByteArray& compressedData, int width, int height)
{
    if (width <= 0 || height <= 0) return {};
    const int stride = ((width + 31) / 32) * 4;
    QByteArray output(stride * height, char(0));
    FaxBitReader reader(compressedData);

    for (int row = 0; row < height; ++row) {
        int x = 0;
        bool black = false;
        int guard = 0;
        while (x < width && ++guard < width * 8 + 4096) {
            bool eol = false;
            const qsizetype before = reader.position();
            int totalRun = 0;
            while (true) {
                const int run = DecodeFaxCode(reader, black, eol);
                if (eol) {
                    if (x == 0) { totalRun = 0; continue; }
                    break;
                }
                if (run < 0) return {};
                totalRun += run;
                if (run < 64) break;
                if (totalRun > width + 2560) return {};
            }
            if (eol) break;
            if (reader.position() == before) return {};
            const int end = std::min(width, x + totalRun);
            if (black) {
                const int outRow = height - 1 - row;
                for (int px = x; px < end; ++px)
                    output[outRow * stride + px / 8] = char(uchar(output.at(outRow * stride + px / 8)) | uchar(0x80 >> (px & 7)));
            }
            x = end;
            black = !black;
        }
        if (x < width) return {};
    }
    return output;
}

QVector<QRgb> FallbackPalette(int bitCount)
{
    if (bitCount == 1)
    {
        return {
            qRgba(0, 0, 0, 255),
            qRgba(255, 255, 255, 255)
        };
    }

    if (bitCount == 2)
    {
        // Safe fallback for malformed indexed resources without a color table.
        return {
            qRgba(0, 0, 0, 255),
            qRgba(64, 64, 64, 255),
            qRgba(192, 192, 192, 255),
            qRgba(255, 255, 255, 255)
        };
    }

    if (bitCount == 4)
    {
        // Palette EGA Windows as fallback with bitCount=4 when it is missing.
        return {
            qRgba(0, 0, 0, 255),
            qRgba(0, 0, 128, 255),
            qRgba(0, 128, 0, 255),
            qRgba(0, 128, 128, 255),
            qRgba(128, 0, 0, 255),
            qRgba(128, 0, 128, 255),
            qRgba(128, 128, 0, 255),
            qRgba(192, 192, 192, 255),
            qRgba(128, 128, 128, 255),
            qRgba(0, 0, 255, 255),
            qRgba(0, 255, 0, 255),
            qRgba(0, 255, 255, 255),
            qRgba(255, 128, 128, 255),
            qRgba(255, 192, 203, 255),
            qRgba(255, 255, 0, 255),
            qRgba(255, 255, 255, 255)
        };
    }

    return {};
}

Optional<Img> GenerateBitmapFromDataImpl(
    const QByteArray& pixelData,
    const QByteArray& maskData,
    int width,
    int height,
    int bitCount,
    QVector<QRgb> palette);

Optional<Img> TryDecodeBitmap(const QByteArray& data, const QByteArray& fullData);
Optional<Img> Decode_BITMAP(const QByteArray& bmpData, const QByteArray& resData);
Optional<Img> Decode_RT_POINTER_V1(const QByteArray& CIresData, const QByteArray& bitmapArray);
Optional<Img> Decode_RT_POINTER_V2(const QByteArray& CIresData, const QByteArray& bitmapArray);
Optional<Img> Decode_BITMAP_Win1_Win2(const QByteArray& resourceData);
Optional<Img> Decode_BITMAP_WINDOWS_DIB(const QByteArray& data);
Optional<Img> Decode_BITMAP_OS2_V1(const QByteArray& data, const QByteArray& resData);
Optional<Img> Decode_BITMAP_OS2_ArrayPart(const QByteArray& bmpData, const QByteArray& resData);
Optional<Img> Decode_BITMAP_OS2_V2(const QByteArray& data, const QByteArray& resData);

QByteArray DecompressRLE4(
    const QByteArray& compressedData,
    int width,
    int height,
    const QVector<QRgb>& palette);

QByteArray DecompressRLE8(
    const QByteArray& compressedData,
    int width,
    int height);

QByteArray DecompressRLE24(
    const QByteArray& compressedData,
    int width,
    int height);

int GetRowStartIndex(int y, int height, int uncompressedBytesPerRow);
void Set4BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar pixelValue,
    int width,
    int uncompressedBytesPerRow);

void Set8BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar pixelValue,
    int width,
    int uncompressedBytesPerRow);

void Set24BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar blue,
    uchar green,
    uchar red,
    int width,
    int uncompressedBytesPerRow);

void CopyLarge(
    const QByteArray& source,
    qint64 sourceOffset,
    QByteArray& destination,
    int destOffset,
    qint64 count);

// This is an entrypoint also for RT_POINTER. A bitmap array can be the base for both the types.
// This return as list of Img for compatibility with OS/2 Bitmap Array
QVector<Img> Get(const QByteArray& resData)
{
    // We'll use a list of tuples containing the index and image
    // so we can reorder at the end.
    QVector<std::tuple<int, Img>> parallelResults;

    try
    {
        // Check for OS/2 BITMAPARRAYHEADER (starts with 'BA')
        if (resData.size() >= 2
            && static_cast<uchar>(resData.at(0)) == 0x42
            && static_cast<uchar>(resData.at(1)) == 0x41) // 'BA'
        {
            // This list will store the original index along with the segment details.
            // Item1: OriginalIndex, Item2: Offset, Item3: BmpOffset, Item4: CurrentBmpSize
            QVector<std::tuple<int, int, int, int>> bitmapSegmentsWithIndex;
            int offset = 0;
            int originalIndex = 0;

            // Sequentially identify all bitmap segments by their index.
            while (offset < resData.size())
            {
                if (offset + 14 > resData.size())
                {
                    qDebug() << "[DEBUG] Not enough data for 'BA' header at offset"
                             << offset << ". Aborting.";
                    break;
                }

                if (static_cast<uchar>(resData.at(offset)) != 0x42
                    || static_cast<uchar>(resData.at(offset + 1)) != 0x41)
                {
                    qDebug() << "[DEBUG] Invalid 'BA' signature at offset"
                             << offset << ". Aborting.";
                    break;
                }

                const int nextOffset = ReadInt32(resData, offset + 6);
                const int bmpOffset = offset + 14;

                if (bmpOffset >= resData.size())
                {
                    qDebug() << "[DEBUG] BMP data offset"
                             << bmpOffset << "beyond end of data. Aborting.";
                    break;
                }

                int currentBmpSize =
                    (nextOffset > 0
                     && nextOffset > offset
                     && nextOffset < resData.size())
                        ? nextOffset - bmpOffset
                        : resData.size() - bmpOffset;

                if (bmpOffset + currentBmpSize > resData.size())
                {
                    qDebug() << "[DEBUG] Calculated BMP size"
                             << currentBmpSize
                             << "from offset"
                             << bmpOffset
                             << "exceeds data length. Adjusting.";

                    currentBmpSize = resData.size() - bmpOffset;
                }

                // Add currentOriginalIndex along with the segment details.
                bitmapSegmentsWithIndex.append(
                    std::make_tuple(
                        originalIndex,
                        offset,
                        bmpOffset,
                        currentBmpSize));

                ++originalIndex;

                if (nextOffset <= offset || nextOffset >= resData.size())
                {
                    qDebug() << "[DEBUG] Next offset"
                             << nextOffset
                             << "is invalid or end of data. Stopping loop.";
                    break;
                }

                offset = nextOffset;
            }

            // The C# implementation uses Parallel.ForEach here.
            // The Qt port keeps the same segment independence and result ordering.
            // Decoding is performed sequentially to avoid adding a Qt Concurrent link dependency.
            for (const auto& segment : bitmapSegmentsWithIndex)
            {
                const int currentOriginalIndex = std::get<0>(segment);
                const int currentOffset = std::get<1>(segment);
                const int currentBmpOffset = std::get<2>(segment);
                const int currentBmpSize = std::get<3>(segment);

                const QByteArray bmpData =
                    resData.mid(currentBmpOffset, currentBmpSize);

                const Optional<Img> image =
                    TryDecodeBitmap(bmpData, resData);

                const bool imageDecoded = image.has_value();

                if (imageDecoded)
                {
                    // Add a tuple (currentOriginalIndex, bitmap) in the result list.
                    parallelResults.append(
                        std::make_tuple(
                            currentOriginalIndex,
                            *image));
                }

                const QString status =
                    imageDecoded
                        ? QStringLiteral("loaded successfully")
                        : QStringLiteral("failed to load");

                qDebug().noquote()
                    << QStringLiteral(
                           "Bitmap or Pointer %1 at offset %2 "
                           "(Original Index: %3)")
                           .arg(status)
                           .arg(currentOffset)
                           .arg(currentOriginalIndex);
            }

            std::sort(
                parallelResults.begin(),
                parallelResults.end(),
                [](const std::tuple<int, Img>& left,
                   const std::tuple<int, Img>& right)
                {
                    return std::get<0>(left) < std::get<0>(right);
                });

            QVector<Img> result;
            result.reserve(parallelResults.size());

            for (const auto& item : parallelResults)
                result.append(std::get<1>(item));

            return result;
        }
        else // Not an OS/2 BITMAPARRAYHEADER, try to decode as a single bitmap
        {
            const Optional<Img> image =
                TryDecodeBitmap(resData, resData);

            const bool imageDecoded = image.has_value();

            if (imageDecoded)
            {
                parallelResults.append(
                    std::make_tuple(0, *image)); // Add with index 0
            }

            QVector<Img> result;

            // Extract single bitmap.
            for (const auto& item : parallelResults)
                result.append(std::get<1>(item));

            return result;
        }
    }
    catch (const std::exception& exception)
    {
        qWarning() << "Failed to process bitmap data:"
                   << exception.what();
    }
    catch (...)
    {
        qWarning() << "Failed to process bitmap data.";
    }

    return {};
}

Optional<Img> TryDecodeBitmap(
    const QByteArray& data,
    const QByteArray& fullData)
{
    Optional<Img> image = Decode_BITMAP(data, fullData);

    if (image.has_value())
        return image;

    image = Decode_RT_POINTER_V1(data, fullData);

    if (image.has_value())
    {
        qDebug() << "Data was pointer. Decoded with Decode_RT_POINTER.";
        return image;
    }

    image = Decode_RT_POINTER_V2(data, fullData);

    if (image.has_value())
    {
        qDebug() << "Data was pointer. Decoded with Decode_RT_POINTER_V2.";
        return image;
    }

    image = Decode_BITMAP_Win1_Win2(data);

    if (image.has_value())
    {
        qDebug() << "Data was bitmap. Decoded with Decode_BITMAP_Win1_Win2.";
        return image;
    }

    return Optional<Img>();
}

Optional<Img> Decode_BITMAP_Win1_Win2(
    const QByteArray& resourceData)
{
    // Check for null input data
    if (resourceData.isNull())
    {
        qWarning() << "Error: Resource data cannot be null.";
        return Optional<Img>();
    }

    // Check for minimum header size
    if (resourceData.size() < 16) // Minimum header is 16 bytes
    {
        qWarning() << "Error: Resource data is too short to contain the header.";
        return Optional<Img>();
    }

    // Read key values from the header (Little Endian)
    const quint32 resourceId = ReadUInt32(resourceData, 0); // Offset 0-3
    const quint16 width = ReadUInt16(resourceData, 4); // Offset 4-5
    const quint16 height = ReadUInt16(resourceData, 6); // Offset 6-7
    const quint16 bytesPerLine = ReadUInt16(resourceData, 8); // Offset 8-9 (Stride)

    qDebug().noquote()
        << QStringLiteral(
               "Header Detected: ResourceID=0x%1, Width=%2, "
               "Height=%3, BytesPerLine=%4")
               .arg(resourceId, 0, 16)
               .arg(width)
               .arg(height)
               .arg(bytesPerLine);

    // Calculate the expected pixel data size based on the header
    int expectedPixelDataSize = height * bytesPerLine;
    const int headerSize = 16; // Fixed header size

    // Adjust expected pixel data size if the provided data is shorter
    if (resourceData.size() < headerSize + expectedPixelDataSize)
    {
        // Theoretical minimum for 1bpp
        if (resourceData.size() - headerSize
            < ((width + 7) / 8) * height)
        {
            qWarning().noquote()
                << QStringLiteral(
                       "Warning: Dump data seems insufficient for a "
                       "%1x%2 1bpp image with stride %3. Attempting "
                       "to decode the %4 available bytes anyway.")
                       .arg(width)
                       .arg(height)
                       .arg(bytesPerLine)
                       .arg(resourceData.size() - headerSize);
        }

        // Adapt size to what is actually available
        expectedPixelDataSize = resourceData.size() - headerSize;
    }

    // Ensure width and height are valid before creating Bitmap
    if (width == 0 || height == 0)
    {
        qWarning() << "Error: Invalid image dimensions. Width:"
                   << width << "Height:" << height;
        return Optional<Img>();
    }

    // Create the monochrome bitmap
    QImage bitmap(width, height, QImage::Format_Mono);

    if (bitmap.isNull())
    {
        qWarning() << "Error creating bitmap with dimensions"
                   << width << "x" << height;
        return Optional<Img>();
    }

    // Set the palette for black and white (0 = black, 1 = white)
    bitmap.setColorTable({
        qRgba(0, 0, 0, 255),
        qRgba(255, 255, 255, 255)
    });

    const int destinationStride = bitmap.bytesPerLine();
    int sourceOffset = headerSize;

    for (int y = 0; y < height; ++y)
    {
        // Calculate bytes to copy for the current line, ensuring we don't read beyond resourceData bounds
        int bytesToCopy =
            std::min<int>(
                bytesPerLine,
                resourceData.size() - sourceOffset);

        // Also ensure we don't write beyond the bitmap's stride for the current line
        bytesToCopy =
            std::min(bytesToCopy, destinationStride);

        if (bytesToCopy <= 0)
        {
            qWarning() << "Warning: No more valid data to copy for row"
                       << y
                       << ". Remaining resource data length:"
                       << resourceData.size() - sourceOffset;
            break; // No more valid data to copy
        }

        // Copy data from source buffer to bitmap buffer
        std::memcpy(
            bitmap.scanLine(y),
            resourceData.constData() + sourceOffset,
            static_cast<size_t>(bytesToCopy));

        sourceOffset += bytesPerLine; // Advance in the source data buffer based on its stride
    }

    Img image;
    image.Bitmap = bitmap;
    image.BitCount = 1;
    image.Size = QSize(width, height);
    return image;
}

Optional<Img> Decode_BITMAP(
    const QByteArray& originalBmpData,
    const QByteArray& resData)
{
    try
    {
        // ModuleResources.DumpRaw(bmpData);

        QByteArray bmpData = originalBmpData;

        // A raw Windows RT_BITMAP starts directly with a DIB header. Check
        // the unambiguous 12-byte core and 40+-byte info headers before the
        // more permissive OS/2 legacy probes.
        if (bmpData.size() >= 4) {
            const int dibHeaderSize = ReadInt32(bmpData, 0);
            if (dibHeaderSize == 12 || (dibHeaderSize >= 40 && dibHeaderSize <= bmpData.size())) {
                Optional<Img> windowsDib = Decode_BITMAP_WINDOWS_DIB(bmpData);
                if (windowsDib.has_value())
                    return windowsDib;
            }
        }

        Optional<Img> bmpOS2 =
            Decode_BITMAP_OS2_V1(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug() << "Data was bitmap. Decoded with Decode_BITMAP_OS2_V1.";
            return bmpOS2;
        }

        bmpOS2 = Decode_BITMAP_OS2_ArrayPart(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug() << "Data was bitmap. Decoded with Decode_BITMAP_OS2_ArrayPart.";
            return bmpOS2;
        }

        bmpOS2 = Decode_BITMAP_OS2_V2(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug() << "Data was bitmap. Decoded with Decode_BITMAP_OS2_V2.";
            return bmpOS2;
        }

        QString dataStripped;

        // Check if it's a full BMP file (starts with 'BM')
        if (bmpData.size() >= 14
            && static_cast<uchar>(bmpData.at(0)) == 0x42
            && static_cast<uchar>(bmpData.at(1)) == 0x4D)
        {
            const quint32 bfOffBits = ReadUInt32(bmpData, 10);

            // The GDI+ direct decode attempt from the C# implementation is intentionally omitted.
            // Qt has no equivalent that reproduces GDI+'s acceptance of malformed legacy BMP streams.

            if (bfOffBits > static_cast<quint32>(bmpData.size()))
            {
                // Invalid bfOffBits, remove file header and proceed
                dataStripped =
                    QStringLiteral("after stripping the first 14 bytes ");

                bmpData = bmpData.mid(14);
            }
            else
            {
                // Remove file header and proceed with DIB.
                dataStripped =
                    QStringLiteral("after stripping the first 14 bytes ");

                bmpData = bmpData.mid(14);
            }
        }

        // Try again after removing the BMP signature
        bmpOS2 = Decode_BITMAP_OS2_V1(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug().noquote()
                << QStringLiteral(
                       "Data was bitmap. Decoded %1with Decode_BITMAP_OS2_V1.")
                       .arg(dataStripped);
            return bmpOS2;
        }

        bmpOS2 = Decode_BITMAP_OS2_ArrayPart(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug().noquote()
                << QStringLiteral(
                       "Data was bitmap. Decoded %1with Decode_BITMAP_OS2_ArrayPart.")
                       .arg(dataStripped);
            return bmpOS2;
        }

        bmpOS2 = Decode_BITMAP_OS2_V2(bmpData, resData);

        if (bmpOS2.has_value())
        {
            qDebug().noquote()
                << QStringLiteral(
                       "Data was bitmap. Decoded %1with Decode_BITMAP_OS2_V2.")
                       .arg(dataStripped);
            return bmpOS2;
        }

        // Windows-style DIB (BITMAPINFOHEADER).
        // Decode indexed formats ourselves because GDI+ does not reliably support 2 bpp DIBs.
        Optional<Img> windowsDib =
            Decode_BITMAP_WINDOWS_DIB(bmpData);

        if (windowsDib.has_value())
        {
            qDebug() << "Data was bitmap. Decoded with Windows-style DIB decoder.";
            return windowsDib;
        }

        // The GDI+ fallback for formats not handled by the internal decoder
        // intentionally disappears in the Qt port, as requested.
        return Optional<Img>();
    }
    catch (const std::exception& exception)
    {
        qWarning() << "[DEBUG] TryParseBMP failed:"
                   << exception.what();
        return Optional<Img>();
    }
    catch (...)
    {
        qWarning() << "[DEBUG] TryParseBMP failed.";
        return Optional<Img>();
    }
}

Optional<Img> Decode_BITMAP_WINDOWS_DIB(
    const QByteArray& data)
{
    try
    {
        constexpr quint32 BI_RGB = 0;
        constexpr quint32 BI_RLE8 = 1;
        constexpr quint32 BI_RLE4 = 2;
        constexpr quint32 BI_BITFIELDS = 3;

        if (data.size() < 12)
            return Optional<Img>();

        const int headerSize = ReadInt32(data, 0);

        // Windows 2.x and early Windows 3.x resources may contain a raw
        // BITMAPCOREHEADER DIB. RT_BITMAP does not include a 14-byte BMP file
        // header, and its palette entries are RGBTRIPLEs rather than RGBQUADs.
        if (headerSize == 12)
        {
            const int width = ReadUInt16(data, 4);
            const int height = ReadUInt16(data, 6);
            const quint16 planes = ReadUInt16(data, 8);
            const quint16 bitCount = ReadUInt16(data, 10);
            if (width <= 0 || height <= 0 || planes != 1 ||
                (bitCount != 1 && bitCount != 2 && bitCount != 4 &&
                 bitCount != 8 && bitCount != 24))
                return Optional<Img>();

            const int paletteEntries = bitCount <= 8 ? (1 << bitCount) : 0;
            const qint64 pixelOffset = 12 + qint64(paletteEntries) * 3;
            const qint64 stride = ((qint64(width) * bitCount + 31) / 32) * 4;
            const qint64 imageSize = stride * height;
            if (pixelOffset < 12 || imageSize <= 0 ||
                pixelOffset > data.size() || imageSize > data.size() - pixelOffset ||
                imageSize > std::numeric_limits<int>::max())
                return Optional<Img>();

            QVector<QRgb> palette;
            if (paletteEntries > 0)
                palette = ReadPalette(data, 12, paletteEntries, 3);
            const QByteArray pixelData = data.mid(int(pixelOffset), int(imageSize));
            return GenerateBitmapFromDataImpl(pixelData, {}, width, height,
                                                bitCount, palette);
        }

        if (data.size() < 40 || headerSize < 40 || headerSize > data.size())
            return Optional<Img>();

        const int width = ReadInt32(data, 4);
        const int signedHeight = ReadInt32(data, 8);
        const quint16 planes = ReadUInt16(data, 12);
        const quint16 bitCount = ReadUInt16(data, 14);
        const quint32 compression = ReadUInt32(data, 16);
        const quint32 colorsUsed = ReadUInt32(data, 32);

        if (width <= 0
            || signedHeight == 0
            || planes != 1
            || (compression != BI_RGB
                && compression != BI_RLE8
                && compression != BI_RLE4
                && compression != BI_BITFIELDS))
        {
            return Optional<Img>();
        }

        if (bitCount != 1
            && bitCount != 2
            && bitCount != 4
            && bitCount != 8
            && bitCount != 16
            && bitCount != 24
            && bitCount != 32)
        {
            return Optional<Img>();
        }

        // Windows DIB compression combinations are fixed by the format:
        // BI_RLE4 is valid only for 4-bpp indexed images and BI_RLE8 only
        // for 8-bpp indexed images. Compressed top-down DIBs are invalid.
        if ((compression == BI_RLE4 && bitCount != 4)
            || (compression == BI_RLE8 && bitCount != 8)
            || ((compression == BI_RLE4 || compression == BI_RLE8)
                && signedHeight < 0))
        {
            return Optional<Img>();
        }

        const int height = std::abs(signedHeight);
        const bool topDown = signedHeight < 0;

        int paletteEntries = 0;

        if (bitCount <= 8)
        {
            const int maximumPaletteEntries = 1 << bitCount;

            paletteEntries =
                colorsUsed != 0
                    ? static_cast<int>(colorsUsed)
                    : maximumPaletteEntries;

            if (paletteEntries < 0
                || paletteEntries > maximumPaletteEntries)
            {
                return Optional<Img>();
            }
        }

        int masksSize = 0;
        quint32 redMask = 0;
        quint32 greenMask = 0;
        quint32 blueMask = 0;
        quint32 alphaMask = 0;

        if (compression == BI_BITFIELDS)
        {
            if (bitCount != 16 && bitCount != 32)
                return Optional<Img>();

            if (headerSize >= 52)
            {
                redMask = ReadUInt32(data, 40);
                greenMask = ReadUInt32(data, 44);
                blueMask = ReadUInt32(data, 48);
                if (headerSize >= 56)
                    alphaMask = ReadUInt32(data, 52);
            }
            else
            {
                if (data.size() < headerSize + 12)
                    return Optional<Img>();
                redMask = ReadUInt32(data, headerSize);
                greenMask = ReadUInt32(data, headerSize + 4);
                blueMask = ReadUInt32(data, headerSize + 8);
                masksSize = 12;
            }

            if (redMask == 0 || greenMask == 0 || blueMask == 0)
                return Optional<Img>();
        }

        const qint64 paletteEnd =
            static_cast<qint64>(headerSize)
            + masksSize
            + static_cast<qint64>(paletteEntries) * 4;

        if (paletteEnd > data.size())
            return Optional<Img>();

        QVector<QRgb> palette;

        if (paletteEntries > 0)
        {
            palette =
                ReadPalette(
                    data,
                    headerSize,
                    paletteEntries,
                    4);
        }

        const int stride =
            ((width * bitCount + 31) / 32) * 4;

        if (compression == BI_RLE4 || compression == BI_RLE8)
        {
            const quint32 declaredImageSize = ReadUInt32(data, 20);
            const int availableCompressedSize = data.size() - static_cast<int>(paletteEnd);

            if (availableCompressedSize <= 0)
                return Optional<Img>();

            int compressedSize = availableCompressedSize;
            if (declaredImageSize != 0
                && declaredImageSize <= static_cast<quint32>(availableCompressedSize))
            {
                compressedSize = static_cast<int>(declaredImageSize);
            }

            const QByteArray compressedData =
                data.mid(static_cast<int>(paletteEnd), compressedSize);

            QByteArray pixelData;
            if (compression == BI_RLE4)
                pixelData = DecompressRLE4(compressedData, width, height, palette);
            else
                pixelData = DecompressRLE8(compressedData, width, height);

            const qint64 expectedSize = static_cast<qint64>(stride) * height;
            if (pixelData.size() != expectedSize)
                return Optional<Img>();

            return GenerateBitmapFromDataImpl(
                pixelData,
                {},
                width,
                height,
                bitCount,
                palette);
        }

        const qint64 calculatedImageSize =
            static_cast<qint64>(stride) * height;

        if (calculatedImageSize <= 0
            || calculatedImageSize > std::numeric_limits<int>::max())
        {
            return Optional<Img>();
        }

        int pixelOffset = static_cast<int>(paletteEnd);

        if (static_cast<qint64>(pixelOffset) + calculatedImageSize
            > data.size())
        {
            // biSizeImage is frequently wrong in old resources. Prefer the size
            // calculated from width, height and bit depth and anchor it at the end.
            const int endAnchoredOffset =
                data.size() - static_cast<int>(calculatedImageSize);

            if (endAnchoredOffset < pixelOffset)
                return Optional<Img>();

            pixelOffset = endAnchoredOffset;
        }

        QByteArray pixelData =
            data.mid(pixelOffset, calculatedImageSize);

        if (topDown)
        {
            // GenerateBitmapFromData expects bottom-up DIB rows.
            QByteArray bottomUpData(pixelData.size(), '\0');

            for (int y = 0; y < height; ++y)
            {
                std::memcpy(
                    bottomUpData.data()
                        + static_cast<qint64>(height - 1 - y) * stride,
                    pixelData.constData()
                        + static_cast<qint64>(y) * stride,
                    static_cast<size_t>(stride));
            }

            pixelData = bottomUpData;
        }

        if (compression == BI_BITFIELDS)
        {
            auto component = [](quint32 value, quint32 mask, int defaultValue) -> int
            {
                if (mask == 0)
                    return defaultValue;
                int shift = 0;
                quint32 shiftedMask = mask;
                while ((shiftedMask & 1u) == 0u)
                {
                    shiftedMask >>= 1;
                    ++shift;
                }
                const quint32 maximum = shiftedMask;
                const quint32 raw = (value & mask) >> shift;
                return maximum == 0 ? defaultValue
                                    : int((quint64(raw) * 255u + maximum / 2u) / maximum);
            };

            QImage image(width, height, QImage::Format_ARGB32);
            if (image.isNull())
                return Optional<Img>();

            const int bytesPerPixel = bitCount / 8;
            for (int y = 0; y < height; ++y)
            {
                const int sourceY = height - 1 - y;
                const char* sourceRow = pixelData.constData() + qint64(sourceY) * stride;
                QRgb* destination = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < width; ++x)
                {
                    quint32 value = 0;
                    if (bytesPerPixel == 2)
                    {
                        value = quint32(uchar(sourceRow[x * 2]))
                              | (quint32(uchar(sourceRow[x * 2 + 1])) << 8);
                    }
                    else
                    {
                        value = quint32(uchar(sourceRow[x * 4]))
                              | (quint32(uchar(sourceRow[x * 4 + 1])) << 8)
                              | (quint32(uchar(sourceRow[x * 4 + 2])) << 16)
                              | (quint32(uchar(sourceRow[x * 4 + 3])) << 24);
                    }
                    destination[x] = qRgba(component(value, redMask, 0),
                                            component(value, greenMask, 0),
                                            component(value, blueMask, 0),
                                            component(value, alphaMask, 255));
                }
            }

            Img result;
            result.Bitmap = image;
            result.BitCount = bitCount;
            result.Size = QSize(width, height);
            return result;
        }

        return GenerateBitmapFromDataImpl(
            pixelData,
            {},
            width,
            height,
            bitCount,
            palette);
    }
    catch (...)
    {
        qWarning() << "[DEBUG] Decode_BITMAP_WINDOWS_DIB failed.";
        return Optional<Img>();
    }
}

Optional<Img> Decode_RT_POINTER_V1(
    const QByteArray& CIresData,
    const QByteArray& bitmapArray)
{
    qsizetype position = 0;

    auto readUInt16 = [&]() -> quint16
    {
        const quint16 value = ReadUInt16(CIresData, position);
        position += 2;
        return value;
    };

    auto readUInt32 = [&]() -> quint32
    {
        const quint32 value = ReadUInt32(CIresData, position);
        position += 4;
        return value;
    };

    auto readByte = [&]() -> uchar
    {
        if (position >= CIresData.size())
            return 0;

        return static_cast<uchar>(CIresData.at(position++));
    };

    // === First block (mask) ===
    const quint16 header = readUInt16();

    // "CI", "IC", "CP", "PT" are basically the same format, for a different use.
    // The main difference is that the bitmap data block might be missing and
    // there is only the first block (mask) that contains also the bitmap data.
    // IC icon (OS/2 1.x)
    // CI icon (OS/2 2.x+)
    // PT pointer
    if (header != 0x4943
        && header != 0x4349
        && header != 0x5043
        && header != 0x5450)
    {
        return Optional<Img>();
    }

    (void) readUInt32(); // fileSize
    const quint16 xHotspotMask = readUInt16(); // xHotspot
    const quint16 yHotspotMask = readUInt16(); // yHotspot
    const quint32 bitmapOffsetMask = readUInt32();

    Q_UNUSED(xHotspotMask)
    Q_UNUSED(yHotspotMask)

    if (readUInt32() != 12)
        return Optional<Img>(); // BitmapCoreHeader size

    const quint16 widthMask = readUInt16();
    const quint16 heightMask = readUInt16(); // This is double the height of the bitmap!
    const quint16 planesMask = readUInt16(); // planes
    quint16 bppMask = readUInt16(); // bpp (should be 1)

    if (bppMask < 1)
    {
        // Invalid bpp detected.
        qWarning() << "Invalid bpp detected. bpp has manually set to 1.";
        bppMask = 1;
    }

    if (planesMask != 1
        || (bppMask != 1
            && bppMask != 4
            && bppMask != 8))
    {
        return Optional<Img>();
    }

    const int numColorsMask = 1 << bppMask;

    // --- COLOR PALETTE ---
    QVector<QRgb> paletteMask;
    paletteMask.reserve(numColorsMask);

    for (int i = 0; i < numColorsMask; ++i)
    {
        const uchar blue = readByte();
        const uchar green = readByte();
        const uchar red = readByte();

        paletteMask.append(
            qRgba(red, green, blue, 255));
    }

    // === Second block (color image) ===
    if (position + 2 <= CIresData.size()
        && ReadUInt16(CIresData, position) == header)
    {
        position += 2;

        // Second block found!
        (void) readUInt32(); // fileSize
        const quint16 xHotspot = readUInt16();
        const quint16 yHotspot = readUInt16();
        const quint32 bitmapOffset = readUInt32();

        Q_UNUSED(xHotspot)
        Q_UNUSED(yHotspot)

        if (readUInt32() != 12)
            return Optional<Img>();

        const quint16 width = readUInt16();
        const quint16 height = readUInt16();
        const quint16 planes = readUInt16();
        const quint16 bpp = readUInt16();

        // Pointers can be 1, 2, 4, 8, 24 bpp
        if (planes != 1
            || (bpp != 1
                && bpp != 2
                && bpp != 4
                && bpp != 8
                && bpp != 24))
        {
            return Optional<Img>(); // Unsupported color format
        }

        int numColors = 1 << bpp;

        if (bpp == 24)
            numColors = 0; // 24-bit images usually don't have a palette

        QVector<QRgb> palette;
        palette.reserve(numColors);

        for (int i = 0; i < numColors; ++i)
        {
            const uchar blue = readByte();
            const uchar green = readByte();
            const uchar red = readByte();

            palette.append(
                qRgba(red, green, blue, 255));
        }

        // === Calculate size ===
        const int stride =
            ((width * bpp + 31) / 32) * 4;

        const int strideMask =
            ((widthMask * bppMask + 31) / 32) * 4;

        // We use the real weight, already given by the bitmap, not from the mask.
        // From the mask it would be heightMask / 2
        const int bitmapSize = stride * height;
        const int maskSize = strideMask * height;

        // === Read data from bitmapArray ===
        if (static_cast<quint64>(bitmapOffsetMask) + maskSize
            > static_cast<quint64>(bitmapArray.size()))
        {
            return Optional<Img>();
        }

        QByteArray colorData(bitmapSize, '\0');
        QByteArray maskData(maskSize, '\0');

        if (static_cast<quint64>(bitmapOffset) + bitmapSize
            > static_cast<quint64>(bitmapArray.size()))
        {
            // Out of offset. Must fallback to the mask as bitmap data.
            qWarning() << "Invalid bitmapOffset detected. Using bitmapOffsetMask.";

            if (!IsRangeValid(bitmapArray, bitmapOffsetMask, bitmapSize))
                return Optional<Img>();

            std::memcpy(
                colorData.data(),
                bitmapArray.constData() + bitmapOffsetMask,
                static_cast<size_t>(bitmapSize));
        }
        else
        {
            std::memcpy(
                colorData.data(),
                bitmapArray.constData() + bitmapOffset,
                static_cast<size_t>(bitmapSize));
        }

        // We skip the first half that we don't care about.
        if (!IsRangeValid(
                bitmapArray,
                static_cast<quint64>(bitmapOffsetMask) + maskSize,
                maskSize))
        {
            return Optional<Img>();
        }

        std::memcpy(
            maskData.data(),
            bitmapArray.constData() + bitmapOffsetMask + maskSize,
            static_cast<size_t>(maskSize));

        return GenerateBitmapFromDataImpl(
            colorData,
            maskData,
            width,
            height,
            bpp,
            palette);
    }
    else
    {
        // Second block not found.
        // We use the bitmap data from the first half of mask data.

        const int stride =
            ((widthMask * bppMask + 31) / 32) * 4;

        const int maskStride =
            ((widthMask + 31) / 32) * 4; // 1bpp mask

        const int realHeight = heightMask / 2;
        const int bitmapSize = stride * realHeight;
        const int maskSize = maskStride * realHeight;

        if (static_cast<quint64>(bitmapOffsetMask)
                + bitmapSize
                + maskSize
            > static_cast<quint64>(bitmapArray.size()))
        {
            return Optional<Img>();
        }

        const QByteArray colorData =
            bitmapArray.mid(bitmapOffsetMask, bitmapSize);

        const QByteArray maskData =
            bitmapArray.mid(
                bitmapOffsetMask + bitmapSize,
                maskSize);

        return GenerateBitmapFromDataImpl(
            colorData,
            maskData,
            widthMask,
            realHeight,
            bppMask,
            paletteMask);
    }
}

Optional<Img> Decode_RT_POINTER_V2(
    const QByteArray& CIresData,
    const QByteArray& bitmapArray)
{
    qsizetype position = 0;

    auto readUInt16 = [&]() -> quint16
    {
        const quint16 value = ReadUInt16(CIresData, position);
        position += 2;
        return value;
    };

    auto readUInt32 = [&]() -> quint32
    {
        const quint32 value = ReadUInt32(CIresData, position);
        position += 4;
        return value;
    };

    const quint16 header = readUInt16();

    // "CI", "IC", "CP", "PT" are basically the same format, for a different use.
    // The main difference is that the bitmap data block might be missing and
    // there is only the first block (mask) that contains also the bitmap data.
    // IC icon (OS/2 1.x)
    // CI icon (OS/2 2.x+)
    // PT pointer
    if (header != 0x4943
        && header != 0x4349
        && header != 0x5043
        && header != 0x5450)
    {
        return Optional<Img>();
    }

    // Skip fileSize and read hotspot and bitmapOffset for the mask
    (void) readUInt32(); // fileSize (total size of the resource)
    const quint16 xHotspotMask = readUInt16(); // xHotspot
    const quint16 yHotspotMask = readUInt16(); // yHotspot
    const quint32 bitmapOffsetMask = readUInt32(); // Offset to the actual bitmap data (for the mask)

    Q_UNUSED(xHotspotMask)
    Q_UNUSED(yHotspotMask)

    // Read BITMAPINFOHEADER2 for the mask
    const int maskInfoOffset =
        static_cast<int>(position); // Current position is the start of BITMAPINFOHEADER2

    const quint32 cbFixMask = readUInt32();
    const quint32 widthMask = readUInt32();
    const quint32 heightMask = readUInt32(); // This height is typically double the actual height for icons/pointers (mask + image)
    const quint16 planesMask = readUInt16();
    const quint16 bppMask = readUInt16(); // Should be 1 bpp for the mask
    const quint32 compressionMask = readUInt32(); // ulCompression (should be 0 for uncompressed)
    const quint32 cbImageMask = readUInt32(); // Size of the raw pixel data for mask
    const quint32 xpelsPerMeterMask = readUInt32();
    const quint32 ypelsPerMeterMask = readUInt32();
    const quint32 cclrUsedMask = readUInt32(); // Number of colors in the color table (for mask, usually 2 for black/white)
    const quint32 clrImportantMask = readUInt32();

    Q_UNUSED(compressionMask)
    Q_UNUSED(cbImageMask)
    Q_UNUSED(xpelsPerMeterMask)
    Q_UNUSED(ypelsPerMeterMask)
    Q_UNUSED(clrImportantMask)

    // Validate mask properties
    if (planesMask != 1 || bppMask != 1) // Mask should typically be 1 bpp and 1 plane
    {
        qWarning()
            << "Warning: Mask properties unexpected "
               "(planes != 1 or bpp != 1). Attempting to proceed.";
        // return null; // Or handle more gracefully
    }

    // Determine the number of colors in the palette for the mask
    int numColorsMask =
        cclrUsedMask != 0
            ? static_cast<int>(cclrUsedMask)
            : (1 << (bppMask * planesMask));

    if (numColorsMask == 0)
        numColorsMask = 2; // For 1bpp, at least 2 colors (black/white)

    const int colorTableOffsetMask =
        14 + static_cast<int>(cbFixMask);

    Q_UNUSED(maskInfoOffset)

    // --- COLOR PALETTE for the mask ---
    const QVector<QRgb> paletteMask =
        ReadPalette(
            CIresData,
            colorTableOffsetMask,
            numColorsMask,
            4);

    position =
        colorTableOffsetMask
        + static_cast<qsizetype>(numColorsMask) * 4;

    // --- Second block (color image) ---
    if (position + 2 <= CIresData.size()
        && ReadUInt16(CIresData, position) == header)
    {
        position += 2;

        // Second block found! This is the actual color image data.
        (void) readUInt32(); // fileSize
        const quint16 xHotspot = readUInt16();
        const quint16 yHotspot = readUInt16();
        const quint32 bitmapOffset = readUInt32(); // Offset to the actual bitmap data (for the color image)

        Q_UNUSED(xHotspot)
        Q_UNUSED(yHotspot)

        // Read BITMAPINFOHEADER2 for the color image
        const int colorInfoOffset = static_cast<int>(position);
        const quint32 cbFix = readUInt32();
        const quint32 width = readUInt32();
        const quint32 height = readUInt32(); // This should be the actual height
        const quint16 planes = readUInt16();
        const quint16 bpp = readUInt16();
        const quint32 compression = readUInt32();
        const quint32 cbImage = readUInt32(); // Size of the raw pixel data for color image
        const quint32 xpelsPerMeter = readUInt32();
        const quint32 ypelsPerMeter = readUInt32();
        const quint32 cclrUsed = readUInt32(); // Number of colors in the color table
        const quint32 clrImportant = readUInt32();

        Q_UNUSED(compression)
        Q_UNUSED(cbImage)
        Q_UNUSED(xpelsPerMeter)
        Q_UNUSED(ypelsPerMeter)
        Q_UNUSED(clrImportant)

        // Validate color image properties
        // Pointers can be 1, 2, 4, 8, 24 bpp
        if (planes != 1
            || (bpp != 1
                && bpp != 2
                && bpp != 4
                && bpp != 8
                && bpp != 24))
        {
            return Optional<Img>(); // Unsupported color format
        }

        int numColors =
            cclrUsed != 0
                ? static_cast<int>(cclrUsed)
                : (1 << (bpp * planes));

        if (bpp == 24)
            numColors = 0; // 24-bit images usually don't have a palette

        const int colorTableOffset =
            colorInfoOffset + static_cast<int>(cbFix);

        // --- COLOR PALETTE for the color image ---
        const QVector<QRgb> palette =
            ReadPalette(
                CIresData,
                colorTableOffset,
                numColors,
                4);

        // --- Read data from bitmapArray ---
        // The actual pixel data for both mask and color image is typically in bitmapArray,
        // and the offsets (`bitmapOffsetMask`, `bitmapOffset`) point to their start within `bitmapArray`.

        const int stride =
            ((static_cast<int>(width) * bpp + 31) / 32) * 4;

        const int strideMask =
            ((static_cast<int>(widthMask) * bppMask + 31) / 32) * 4;

        // We use the real weight, already given by the bitmap, not from the mask.
        // From the mask it would be heightMask / 2
        const int bitmapSize =
            stride * static_cast<int>(height);

        const int maskSize =
            strideMask * static_cast<int>(height);

        QByteArray colorData(bitmapSize, '\0');
        QByteArray maskData(maskSize, '\0');

        if (static_cast<quint64>(bitmapOffset) + bitmapSize
            > static_cast<quint64>(bitmapArray.size()))
        {
            // Out of offset. Must fallback to the mask as bitmap data.
            qWarning() << "Invalid bitmapOffset detected. Using bitmapOffsetMask.";

            if (!IsRangeValid(bitmapArray, bitmapOffsetMask, bitmapSize))
                return Optional<Img>();

            std::memcpy(
                colorData.data(),
                bitmapArray.constData() + bitmapOffsetMask,
                static_cast<size_t>(bitmapSize));
        }
        else
        {
            std::memcpy(
                colorData.data(),
                bitmapArray.constData() + bitmapOffset,
                static_cast<size_t>(bitmapSize));
        }

        // We skip the first half that we don't care about.
        if (!IsRangeValid(
                bitmapArray,
                static_cast<quint64>(bitmapOffsetMask) + maskSize,
                maskSize))
        {
            return Optional<Img>();
        }

        std::memcpy(
            maskData.data(),
            bitmapArray.constData() + bitmapOffsetMask + maskSize,
            static_cast<size_t>(maskSize));

        return GenerateBitmapFromDataImpl(
            colorData,
            maskData,
            static_cast<int>(width),
            static_cast<int>(height),
            bpp,
            palette);
    }
    else
    {
        // Second block not found.
        // We use the bitmap data from the first half of mask data.

        const int stride =
            ((static_cast<int>(widthMask) * bppMask + 31) / 32) * 4;

        const int maskStride =
            ((static_cast<int>(widthMask) + 31) / 32) * 4; // 1bpp mask

        const int realHeight =
            static_cast<int>(heightMask) / 2;

        const int bitmapSize = stride * realHeight;
        const int maskSize = maskStride * realHeight;

        if (static_cast<quint64>(bitmapOffsetMask)
                + bitmapSize
                + maskSize
            > static_cast<quint64>(bitmapArray.size()))
        {
            return Optional<Img>();
        }

        const QByteArray colorData =
            bitmapArray.mid(bitmapOffsetMask, bitmapSize);

        const QByteArray maskData =
            bitmapArray.mid(
                bitmapOffsetMask + bitmapSize,
                maskSize);

        return GenerateBitmapFromDataImpl(
            colorData,
            maskData,
            static_cast<int>(widthMask),
            realHeight,
            bppMask,
            paletteMask);
    }
}

Optional<Img> Decode_BITMAP_OS2_V2(
    const QByteArray& data,
    const QByteArray& resData)
{
    if (data.size() < 54)
        return Optional<Img>();

    const quint16 usType = ReadUInt16(data, 0);

    if (usType != 0x4D42) // 'BM' in little-endian
        return Optional<Img>();

    // Offsets for BITMAPFILEHEADER2 fields
    constexpr int BFH2_OFFBITS_OFFSET = 10;
    constexpr int BFH2_BMP2_OFFSET = 14; // Start of BITMAPINFOHEADER2 within BITMAPFILEHEADER2

    // Offsets for BITMAPINFOHEADER2 fields (relative to the start of BITMAPINFOHEADER2)
    constexpr int BIH2_CBFIX_OFFSET = 0;
    constexpr int BIH2_CX_OFFSET = 4;
    constexpr int BIH2_CY_OFFSET = 8;
    constexpr int BIH2_CPLANES_OFFSET = 12;
    constexpr int BIH2_CBITCOUNT_OFFSET = 14;
    constexpr int BIH2_ULCOMPRESSION_OFFSET = 16;
    constexpr int BIH2_CBIMAGE_OFFSET = 20;
    constexpr int BIH2_CCLRUSED_OFFSET = 32;

    // Read BITMAPFILEHEADER2
    const quint32 pixelDataOffset =
        ReadUInt32(data, BFH2_OFFBITS_OFFSET); // offset to the pel data from the beginning of the *file*

    const int bitmapInfoOffset =
        BFH2_BMP2_OFFSET; // BITMAPINFOHEADER2 starts at BFH2_BMP2_OFFSET relative to the start of 'data'

    // Read BITMAPINFOHEADER2 fields
    const quint32 cbFix =
        ReadUInt32(data, bitmapInfoOffset + BIH2_CBFIX_OFFSET); // Size of the structure.

    const quint32 width =
        ReadUInt32(data, bitmapInfoOffset + BIH2_CX_OFFSET); // Width

    const quint32 height =
        ReadUInt32(data, bitmapInfoOffset + BIH2_CY_OFFSET); // Height

    const quint16 cPlanes =
        ReadUInt16(data, bitmapInfoOffset + BIH2_CPLANES_OFFSET); // Color planes (usually 1)

    const quint16 bitCount =
        ReadUInt16(data, bitmapInfoOffset + BIH2_CBITCOUNT_OFFSET); // Bits per pixel

    const quint32 ulCompression =
        ReadUInt32(data, bitmapInfoOffset + BIH2_ULCOMPRESSION_OFFSET); // Compression scheme

    const quint32 cbImage =
        ReadUInt32(data, bitmapInfoOffset + BIH2_CBIMAGE_OFFSET); // Size of the compressed/uncompressed pel data

    const quint32 cclrUsed =
        ReadUInt32(data, bitmapInfoOffset + BIH2_CCLRUSED_OFFSET); // Number of colors in the color table

    // Validate cPlanes. The docs say "I've never seen this set to anything other than 1."
    if (cPlanes != 1)
    {
        qWarning() << "Warning: cPlanes is"
                   << cPlanes << ". Expected 1.";
        // You can choose to throw, log or ignore based on error tolerance.
    }

    // Determine the number of colors in the palette
    int numColors;

    if (bitCount <= 8) // Paletted images usually have bitCount 1, 2, 4, 8
    {
        numColors =
            cclrUsed != 0
                ? static_cast<int>(cclrUsed)
                : (1 << bitCount);
    }
    else // For 16, 24-bit, 32-bit images, cclrUsed gives the actual number of colors or 0 if no palette.
    {
        numColors = static_cast<int>(cclrUsed);
    }

    QVector<QRgb> palette;

    if (numColors > 0)
    {
        const int colorTableOffset =
            bitmapInfoOffset + static_cast<int>(cbFix);

        // Populate the palette
        palette =
            ReadPalette(
                data,
                colorTableOffset,
                numColors,
                4);
    }

    // --- Extract the raw/compressed pixel data from resData ---
    // This is the *compressed* or *uncompressed* data as it appears in the file.
    // If cbImage is 0, it means uncompressed and the size should be calculated.
    // For compressed data, cbImage should contain the actual size of the compressed data.
    int actualCompressedDataSize =
        static_cast<int>(
            cbImage == 0
                ? resData.size() - pixelDataOffset
                : cbImage);

    if (static_cast<quint64>(pixelDataOffset)
            + actualCompressedDataSize
        > static_cast<quint64>(resData.size()))
    {
        // Adjust if header says more than available data
        actualCompressedDataSize =
            resData.size() - static_cast<int>(pixelDataOffset);
    }

    if (actualCompressedDataSize < 0
        || !IsRangeValid(
            resData,
            pixelDataOffset,
            actualCompressedDataSize))
    {
        return Optional<Img>();
    }

    const QByteArray compressedOrRawData =
        resData.mid(
            pixelDataOffset,
            actualCompressedDataSize);

    // --- Handle Compression ---
    QByteArray decompressedPixelData;

    qDebug() << "Bitmap is compressd:" << ulCompression;

    switch (ulCompression)
    {
        case 0: // BCA_UNCOMP - Uncompressed
            decompressedPixelData = compressedOrRawData;
            break;

        case 1: // BCA_HUFFMAN1D - CCITT T.4 1D Huffman, monochrome only
            if (bitCount != 1)
                return Optional<Img>();
            decompressedPixelData = DecompressHuffman1D(
                compressedOrRawData,
                static_cast<int>(width),
                static_cast<int>(height));
            break;

        case 2: // BCA_RLE4 - Run-length encoded, 4 bits/pixel
            decompressedPixelData =
                DecompressRLE4(
                    compressedOrRawData,
                    static_cast<int>(width),
                    static_cast<int>(height),
                    palette);
            break;

        case 3: // BCA_RLE8 - RLE, 8 bits/pixel
            decompressedPixelData =
                DecompressRLE8(
                    compressedOrRawData,
                    static_cast<int>(width),
                    static_cast<int>(height));
            break;

        case 4: // BCA_RLE24 - RLE, 24 bits/pixel
            decompressedPixelData =
                DecompressRLE24(
                    compressedOrRawData,
                    static_cast<int>(width),
                    static_cast<int>(height));
            break;

        default:
            qWarning() << "Error: Unsupported compression type:"
                       << ulCompression;
            return Optional<Img>();
    }

    if (decompressedPixelData.isNull())
    {
        qWarning()
            << "Error: Decompression failed or was not possible "
               "for the given format.";
        return Optional<Img>();
    }

    // The GenerateBitmapFromData function expects uncompressed, padded pixel data.
    return GenerateBitmapFromDataImpl(
        decompressedPixelData,
        {},
        static_cast<int>(width),
        static_cast<int>(height),
        bitCount,
        palette);
}

QByteArray DecompressRLE4(
    const QByteArray& compressedData,
    int width,
    int height,
    const QVector<QRgb>& palette)
{
    Q_UNUSED(palette)

    // RLE4: 4 bits per pixel. Each byte contains two 4-bit pixels.
    // A common RLE scheme for 4bpp uses special codes.
    // E.g., for Windows BMP RLE4:
    // - Byte 1: Count (N)
    // - Byte 2: Two pixels (e.g., AABB for 4 bits each)
    //   -> N pairs of (A, B) pixels are repeated.
    // - If Byte 1 == 0:
    //   - Byte 2 == 0: End of scanline (EOL)
    //   - Byte 2 == 1: End of bitmap (EOB)
    //   - Byte 2 == 2: Delta (Byte 3 = X, Byte 4 = Y offset) - less common for basic RLE
    //   - Byte 2 > 2: Absolute mode: Byte 2 (N) is number of *pixels* to read, followed by N/2 bytes.
    //     The N bytes are padded to a word (16-bit) boundary.

    const int uncompressedBytesPerRow =
        (((width * 4 + 7) / 8) + 3) / 4 * 4; // Padded to 4-byte boundary

    if (width <= 0
        || height <= 0
        || static_cast<qint64>(uncompressedBytesPerRow) * height
            > std::numeric_limits<int>::max())
    {
        return QByteArray();
    }

    QByteArray decompressed(
        uncompressedBytesPerRow * height,
        '\0');

    qsizetype position = 0;
    int currentX = 0;
    int currentY = height - 1; // Bitmaps are typically stored bottom-up

    int iteration = 0;

    try
    {
        while (position < compressedData.size()
               && currentY >= 0)
        {
            if (position + 2 > compressedData.size())
                break;

            const uchar count =
                static_cast<uchar>(compressedData.at(position++));

            if (count == 0x00) // Escape character
            {
                const uchar command =
                    static_cast<uchar>(compressedData.at(position++));

                ++iteration;
                Q_UNUSED(iteration)

                // GenerateBitmapFromDataImpl(decompressed, {}, width, height, 4, palette)
                //     .Bitmap.save(QString::number(iteration) + ".BMP");

                switch (command)
                {
                    case 0x00: // End of Line (EOL)
                        currentX = 0;
                        --currentY;

                        // Align to next row's start in decompressed array (if not already there)
                        // This implicit padding is handled by currentX=0 and the `bytesPerOutputRow` calculation
                        // as we move to the next row.
                        break;

                    case 0x01: // End of Bitmap (EOB)
                        return decompressed;

                    case 0x02: // Delta
                    {
                        if (position + 2 > compressedData.size())
                            return decompressed;

                        const int dx =
                            static_cast<uchar>(compressedData.at(position++));

                        const int dy =
                            static_cast<uchar>(compressedData.at(position++));

                        currentX += dx;
                        currentY -= dy; // Move up for OS/2 bitmaps
                        break;
                    }

                    default: // Absolute Mode: 'command' is the number of pixels
                    {
                        const int numPixelsToRead = command;
                        const int numBytesToRead =
                            (numPixelsToRead + 1) / 2; // Each byte holds 2 pixels (4-bit)

                        if (position + numBytesToRead
                            > compressedData.size())
                        {
                            return decompressed;
                        }

                        for (int i = 0; i < numBytesToRead; ++i)
                        {
                            const uchar twoPixels =
                                static_cast<uchar>(
                                    compressedData.at(position++));

                            if (currentX < width)
                            {
                                Set4BitPixel(
                                    decompressed,
                                    currentX,
                                    currentY,
                                    static_cast<uchar>(
                                        (twoPixels >> 4) & 0x0F),
                                    width,
                                    uncompressedBytesPerRow);

                                ++currentX;
                            }

                            if (currentX < width
                                && (i * 2 + 1) < numPixelsToRead)
                            {
                                Set4BitPixel(
                                    decompressed,
                                    currentX,
                                    currentY,
                                    static_cast<uchar>(
                                        twoPixels & 0x0F),
                                    width,
                                    uncompressedBytesPerRow);

                                ++currentX;
                            }
                        }

                        // Absolute Mode padding: Data is padded to a word boundary (16-bit, 2 bytes)
                        if (numBytesToRead % 2 != 0)
                        {
                            if (position < compressedData.size())
                                ++position; // Read padding byte
                        }

                        break;
                    }
                }
            }
            else // Encoded Mode: 'count' pixels, followed by the two 4-bit values (one byte)
            {
                if (position >= compressedData.size())
                    break;

                const uchar twoPixels =
                    static_cast<uchar>(
                        compressedData.at(position++));

                const uchar pixel1 =
                    static_cast<uchar>((twoPixels >> 4) & 0x0F);

                const uchar pixel2 =
                    static_cast<uchar>(twoPixels & 0x0F);

                for (int i = 0; i < count; ++i)
                {
                    if (currentX < width)
                    {
                        Set4BitPixel(
                            decompressed,
                            currentX,
                            currentY,
                            i % 2 == 0 ? pixel1 : pixel2,
                            width,
                            uncompressedBytesPerRow);

                        ++currentX;
                    }
                    else
                    {
                        // If we hit width limit, move to next row (should ideally be EOL before this)
                        // This might indicate an invalid stream or that the 'count' spans multiple lines.
                        // Assuming 'count' applies to current line.
                        break;
                    }
                }
            }
        }
    }
    catch (...)
    {
        qWarning()
            << "Error during RLE4 decompression.";
        return QByteArray();
    }

    return decompressed;
}

QByteArray DecompressRLE8(
    const QByteArray& compressedData,
    int width,
    int height)
{
    // RLE8: 8 bits per pixel (1 byte per pixel).
    // Similar RLE scheme to RLE4, but values are 1 byte.
    // - Byte 1: Count (N)
    // - Byte 2: Pixel value (P)
    //   -> N occurrences of P are repeated.
    // - If Byte 1 == 0:
    //   - Byte 2 == 0: End of scanline (EOL)
    //   - Byte 2 == 1: End of bitmap (EOB)
    //   - Byte 2 == 2: Delta (Byte 3 = X, Byte 4 = Y offset)
    //   - Byte 2 > 2: Absolute mode: Byte 2 (N) is number of *pixels* to read, followed by N bytes.
    //     The N bytes are padded to a word (16-bit) boundary.

    const int uncompressedBytesPerRow =
        ((width + 3) / 4) * 4; // Padded to 4-byte boundary

    if (width <= 0
        || height <= 0
        || static_cast<qint64>(uncompressedBytesPerRow) * height
            > std::numeric_limits<int>::max())
    {
        return QByteArray();
    }

    QByteArray decompressed(
        uncompressedBytesPerRow * height,
        '\0');

    qsizetype position = 0;
    int currentX = 0;
    int currentY = height - 1; // Bitmaps are typically stored bottom-up

    while (position < compressedData.size()
           && currentY >= 0)
    {
        if (position + 2 > compressedData.size())
            break;

        const uchar count =
            static_cast<uchar>(compressedData.at(position++));

        const uchar commandOrValue =
            static_cast<uchar>(compressedData.at(position++));

        if (count == 0x00) // Escape character
        {
            switch (commandOrValue)
            {
                case 0x00: // End of Line (EOL)
                    currentX = 0;
                    --currentY;
                    break;

                case 0x01: // End of Bitmap (EOB)
                    return decompressed;

                case 0x02: // Delta
                {
                    if (position + 2 > compressedData.size())
                        return decompressed;

                    const int dx =
                        static_cast<uchar>(compressedData.at(position++));

                    const int dy =
                        static_cast<uchar>(compressedData.at(position++));

                    currentX += dx;
                    currentY -= dy; // Move up for OS/2 bitmaps
                    break;
                }

                default: // Absolute Mode: 'command' is the number of pixels
                {
                    const int numPixelsToRead = commandOrValue;

                    if (position + numPixelsToRead
                        > compressedData.size())
                    {
                        return decompressed;
                    }

                    for (int i = 0; i < numPixelsToRead; ++i)
                    {
                        const uchar pixelValue =
                            static_cast<uchar>(
                                compressedData.at(position++));

                        if (currentX < width)
                        {
                            Set8BitPixel(
                                decompressed,
                                currentX,
                                currentY,
                                pixelValue,
                                width,
                                uncompressedBytesPerRow);

                            ++currentX;
                        }
                    }

                    // Absolute Mode padding: Data is padded to a word boundary (16-bit, 2 bytes)
                    if (numPixelsToRead % 2 != 0
                        && position < compressedData.size())
                    {
                        ++position; // Read padding byte
                    }

                    break;
                }
            }
        }
        else // Encoded Mode: 'count' pixels, followed by pixel value
        {
            for (int i = 0; i < count; ++i)
            {
                if (currentX < width)
                {
                    Set8BitPixel(
                        decompressed,
                        currentX,
                        currentY,
                        commandOrValue,
                        width,
                        uncompressedBytesPerRow);

                    ++currentX;
                }
                else
                {
                    break; // Reached end of line, should be EOL next
                }
            }
        }
    }

    return decompressed;
}

QByteArray DecompressRLE24(
    const QByteArray& compressedData,
    int width,
    int height)
{
    // RLE24: 24 bits per pixel (3 bytes per pixel).
    // RLE for 24bpp is simpler, usually just run-length encoding.
    // - Byte 1: Count (N)
    // - Bytes 2,3,4: BGR color value
    //   -> N occurrences of BGR color are repeated.
    // There might be special codes (0x00 followed by command), but often 24bpp is just simple runs.
    // We'll implement the simple run mode, as explicit OS/2 RLE24 documentation is scarce.
    // If a file uses complex RLE24 (like 0x00 escape codes), this would need adjustment.

    const int uncompressedBytesPerRow =
        ((width * 3 + 3) / 4) * 4; // Padded to 4-byte boundary

    if (width <= 0
        || height <= 0
        || static_cast<qint64>(uncompressedBytesPerRow) * height
            > std::numeric_limits<int>::max())
    {
        return QByteArray();
    }

    QByteArray decompressed(
        uncompressedBytesPerRow * height,
        '\0');

    qsizetype position = 0;
    int currentX = 0;
    int currentY = height - 1; // Bitmaps are typically stored bottom-up

    while (position < compressedData.size()
           && currentY >= 0)
    {
        const uchar count =
            static_cast<uchar>(compressedData.at(position++));

        if (count == 0x00)
        {
            // This is a common pattern for special codes in RLE.
            // For 24bpp, the common sub-codes are EOL, EOB, Delta.
            // We'll use a pragmatic approach, similar to RLE4/RLE8 special codes.
            if (position >= compressedData.size())
                break;

            const uchar command =
                static_cast<uchar>(compressedData.at(position++));

            switch (command)
            {
                case 0x00: // End of Line (EOL)
                    currentX = 0;
                    --currentY;
                    break;

                case 0x01: // End of Bitmap (EOB)
                    return decompressed;

                case 0x02: // Delta
                {
                    if (position + 2 > compressedData.size())
                        return decompressed;

                    const int dx =
                        static_cast<uchar>(compressedData.at(position++));

                    const int dy =
                        static_cast<uchar>(compressedData.at(position++));

                    currentX += dx;
                    currentY -= dy;
                    break;
                }

                default: // Absolute Mode: 'command' is the number of pixels
                {
                    // For 24bpp absolute mode, it's 'command' * 3 bytes.
                    const int numPixelsToRead = command;

                    if (position
                            + static_cast<qint64>(numPixelsToRead) * 3
                        > compressedData.size())
                    {
                        return decompressed;
                    }

                    for (int i = 0; i < numPixelsToRead; ++i)
                    {
                        const uchar blue =
                            static_cast<uchar>(compressedData.at(position++));

                        const uchar green =
                            static_cast<uchar>(compressedData.at(position++));

                        const uchar red =
                            static_cast<uchar>(compressedData.at(position++));

                        if (currentX < width)
                        {
                            Set24BitPixel(
                                decompressed,
                                currentX,
                                currentY,
                                blue,
                                green,
                                red,
                                width,
                                uncompressedBytesPerRow);

                            ++currentX;
                        }
                    }

                    // Absolute Mode padding: Data is padded to a word boundary (16-bit)
                    // For 24bpp, this means padding to an even number of bytes for the data block.
                    // If (numPixelsToRead * 3) is odd, add a padding byte.
                    if ((numPixelsToRead * 3) % 2 != 0
                        && position < compressedData.size())
                    {
                        ++position; // Read padding byte
                    }

                    break;
                }
            }
        }
        else // Encoded Mode: 'count' pixels, followed by BGR value
        {
            if (position + 3 > compressedData.size())
                break;

            const uchar blue =
                static_cast<uchar>(compressedData.at(position++));

            const uchar green =
                static_cast<uchar>(compressedData.at(position++));

            const uchar red =
                static_cast<uchar>(compressedData.at(position++));

            for (int i = 0; i < count; ++i)
            {
                if (currentX < width)
                {
                    Set24BitPixel(
                        decompressed,
                        currentX,
                        currentY,
                        blue,
                        green,
                        red,
                        width,
                        uncompressedBytesPerRow);

                    ++currentX;
                }
                else
                {
                    break; // Reached end of line, should be EOL next
                }
            }
        }
    }

    return decompressed;
}

// Helper to calculate the starting index of a row in the decompressed array.
// Bitmap data is usually stored bottom-up, so row 0 in the image is height-1 in the array.
int GetRowStartIndex(
    int y,
    int height,
    int uncompressedBytesPerRow)
{
    return (height - 1 - y) * uncompressedBytesPerRow;
}

// Set a 4-bit pixel (packed in bytes)
void Set4BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar pixelValue,
    int width,
    int uncompressedBytesPerRow)
{
    Q_UNUSED(width)

    const int height =
        static_cast<int>(
            std::ceil(
                static_cast<double>(data.size())
                / uncompressedBytesPerRow));

    const int rowStart =
        GetRowStartIndex(
            y,
            height,
            uncompressedBytesPerRow);

    const int byteIndex =
        rowStart + (x / 2); // Each byte holds two 4-bit pixels

    if (byteIndex < 0 || byteIndex >= data.size())
        return;

    uchar value =
        static_cast<uchar>(data.at(byteIndex));

    if (x % 2 == 0) // First pixel in the byte (upper 4 bits)
    {
        value =
            static_cast<uchar>(
                (value & 0x0F)
                | ((pixelValue & 0x0F) << 4));
    }
    else // Second pixel in the byte (lower 4 bits)
    {
        value =
            static_cast<uchar>(
                (value & 0xF0)
                | (pixelValue & 0x0F));
    }

    data[byteIndex] = static_cast<char>(value);
}

// Set an 8-bit pixel
void Set8BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar pixelValue,
    int width,
    int uncompressedBytesPerRow)
{
    Q_UNUSED(width)

    const int height =
        static_cast<int>(
            std::ceil(
                static_cast<double>(data.size())
                / uncompressedBytesPerRow));

    const int rowStart =
        GetRowStartIndex(
            y,
            height,
            uncompressedBytesPerRow);

    const int index = rowStart + x;

    if (index >= 0 && index < data.size())
        data[index] = static_cast<char>(pixelValue);
}

// Set a 24-bit pixel (BGR order)
void Set24BitPixel(
    QByteArray& data,
    int x,
    int y,
    uchar blue,
    uchar green,
    uchar red,
    int width,
    int uncompressedBytesPerRow)
{
    Q_UNUSED(width)

    const int height =
        static_cast<int>(
            std::ceil(
                static_cast<double>(data.size())
                / uncompressedBytesPerRow));

    const int rowStart =
        GetRowStartIndex(
            y,
            height,
            uncompressedBytesPerRow);

    const int pixelStart =
        rowStart + (x * 3);

    if (pixelStart < 0
        || pixelStart + 2 >= data.size())
    {
        return;
    }

    data[pixelStart] = static_cast<char>(blue);
    data[pixelStart + 1] = static_cast<char>(green);
    data[pixelStart + 2] = static_cast<char>(red);
}

Optional<Img> Decode_BITMAP_OS2_ArrayPart(
    const QByteArray& bmpData,
    const QByteArray& resData)
{
    try
    {
        if (bmpData.size() < 26
            || static_cast<uchar>(bmpData.at(0)) != 0x42
            || static_cast<uchar>(bmpData.at(1)) != 0x4D)
        {
            qDebug() << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: Not a bitmap.";
            return Optional<Img>();
        }

        const int offset = 14;

        const quint32 bcSize =
            ReadUInt32(bmpData, offset + 0);

        const quint16 width =
            ReadUInt16(bmpData, offset + 4);

        const quint16 height =
            ReadUInt16(bmpData, offset + 6);

        const quint16 planes =
            ReadUInt16(bmpData, offset + 8);

        const quint16 bitCount =
            ReadUInt16(bmpData, offset + 10);

        // Validate planes (always 1 for standard bitmaps)
        if (planes != 1)
        {
            qDebug()
                << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: "
                   "Unsupported planes count:"
                << planes;
            return Optional<Img>();
        }

        // --- Calculate Palette Information ---
        int numColors = 0;

        if (bitCount <= 8) // Indexed color formats (1, 2, 4, 8 bpp)
        {
            numColors = 1 << bitCount;
        }
        else if (bitCount == 16 || bitCount == 24) // Direct-color formats have no palette
        {
            numColors = 0;
        }
        else
        {
            qDebug()
                << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: "
                   "Unsupported bit depth:"
                << bitCount;
            return Optional<Img>();
        }

        const int paletteOffsetFromBCHStart =
            offset + static_cast<int>(bcSize);

        const int paletteSize =
            numColors * 3; // OS/2 palettes use 3 bytes (BGR) per entry

        // Check 'data' length for palette. 'data' contains BCH and palette.
        if (numColors > 0
            && bmpData.size()
                < paletteOffsetFromBCHStart + paletteSize)
        {
            qDebug()
                << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: "
                   "'data' (BCH + palette) too short for full palette.";
            return Optional<Img>();
        }

        QVector<QRgb> palette;

        if (numColors > 0)
        {
            palette =
                ReadPalette(
                    bmpData,
                    paletteOffsetFromBCHStart,
                    numColors,
                    3);
        }

        // --- Locate Pixel Data in 'resData' ---
        if (resData.size() < 14) // Ensure resData has at least the full BITMAPFILEHEADER
        {
            qDebug()
                << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: "
                   "'resData' too short to read BITMAPFILEHEADER.";
            return Optional<Img>();
        }

        const quint32 bfOffBits =
            ReadUInt32(bmpData, 10);

        // Calculate expected size of pixel data
        const int bitsPerLine =
            width * bitCount;

        const int stride =
            ((bitsPerLine + 31) / 32) * 4; // Scanline padded to 4-byte boundary

        const int totalPixelBytes =
            stride * height;

        // Ensure 'resData' is large enough to contain the pixel data
        if (static_cast<quint64>(bfOffBits)
                + totalPixelBytes
            > static_cast<quint64>(resData.size()))
        {
            qDebug()
                << "[DEBUG] Decode_BITMAP_OS2_V1_Alt: "
                   "'resData' too short for pixel data. Expected"
                << bfOffBits + totalPixelBytes
                << "got"
                << resData.size();
            return Optional<Img>();
        }

        // Extract the pixel data into a new array from 'resData'.
        const QByteArray bitmapData =
            resData.mid(
                bfOffBits,
                totalPixelBytes);

        return GenerateBitmapFromDataImpl(
            bitmapData,
            {},
            width,
            height,
            bitCount,
            palette);
    }
    catch (...)
    {
        // Re-using original debug message structure
        qWarning()
            << "[DEBUG] TryParseOS2V1 failed.";
        return Optional<Img>();
    }
}

Optional<Img> Decode_BITMAP_OS2_V1(
    const QByteArray& data,
    const QByteArray& resData)
{
    Q_UNUSED(resData)

    try
    {
        if (data.size() < 22)
            return Optional<Img>();

        const quint16 bcSize =
            ReadUInt16(data, 10);

        const quint16 width =
            ReadUInt16(data, 14);

        const quint16 height =
            ReadUInt16(data, 16);

        const quint16 planes =
            ReadUInt16(data, 18);

        const quint16 bitCount =
            ReadUInt16(data, 20);

        if (bitCount > 8)
            return Optional<Img>();

        const int numColors =
            1 << bitCount;

        const int paletteSize =
            numColors * 3; // 3 bytes per color (RGB)

        const int paletteOffset =
            bcSize - paletteSize;

        if (planes != 1
            || (bitCount != 1
                && bitCount != 2
                && bitCount != 4
                && bitCount != 8))
        {
            qDebug()
                << "[DEBUG] Unsupported OS/2 v1 planes or BPP: planes="
                << planes
                << ", bpp="
                << bitCount;
            return Optional<Img>();
        }

        if (data.size() < paletteOffset + paletteSize)
        {
            qDebug()
                << "[DEBUG] Data too short for palette (header size:"
                << bcSize << ")";
            return Optional<Img>();
        }

        const QVector<QRgb> palette =
            ReadPalette(
                data,
                paletteOffset,
                numColors,
                3);

        const int pixelOffset =
            paletteOffset + paletteSize;

        const int bitsPerLine =
            width * bitCount;

        const int stride =
            ((bitsPerLine + 31) / 32) * 4; // Scanline padded to 4-byte boundary

        const int totalPixelBytes =
            stride * height;

        if (data.size() < pixelOffset + totalPixelBytes)
        {
            qDebug()
                << "[DEBUG] Data too short for pixel rows (header size:"
                << bcSize << ")";
            return Optional<Img>();
        }

        const QByteArray bitmapData =
            data.mid(
                pixelOffset,
                totalPixelBytes);

        return GenerateBitmapFromDataImpl(
            bitmapData,
            {},
            width,
            height,
            bitCount,
            palette);
    }
    catch (...)
    {
        qWarning()
            << "[DEBUG] TryParseOS2V1 failed.";
        return Optional<Img>();
    }
}

Optional<Img> GenerateBitmapFromDataImpl(
    const QByteArray& pixelData,
    const QByteArray& maskData,
    int width,
    int height,
    int bitCount,
    QVector<QRgb> palette)
{
    // Unified function to decode the bitmap given the data.
    // It is made to work with everything, Windows Bitmaps, Icons, Cursors and OS/2 Bitmaps, Icons, Pointers etc.
    const int colorStride =
        ((width * bitCount + 31) / 32) * 4;

    const int maskStride =
        ((width + 31) / 32) * 4;

    if (width <= 0
        || height <= 0
        || colorStride <= 0
        || maskStride <= 0)
    {
        return Optional<Img>();
    }

    if (palette.isEmpty())
        palette = FallbackPalette(bitCount);

    QImage bitmap(
        width,
        height,
        QImage::Format_ARGB32);

    if (bitmap.isNull())
        return Optional<Img>();

    const bool applyMask =
        !maskData.isEmpty();

    for (int y = 0; y < height; ++y)
    {
        const int invY =
            height - 1 - y;

        QRgb* destination =
            reinterpret_cast<QRgb*>(
                bitmap.scanLine(y));

        for (int x = 0; x < width; ++x)
        {
            QRgb color =
                qRgba(255, 0, 255, 255);

            const qint64 pixelOffset =
                static_cast<qint64>(invY) * colorStride;

            if (bitCount == 32)
            {
                const qint64 offset =
                    pixelOffset + static_cast<qint64>(x) * 4;

                if (offset + 3 < pixelData.size())
                {
                    const uchar blue =
                        static_cast<uchar>(pixelData.at(offset));

                    const uchar green =
                        static_cast<uchar>(pixelData.at(offset + 1));

                    const uchar red =
                        static_cast<uchar>(pixelData.at(offset + 2));

                    const uchar alpha =
                        static_cast<uchar>(pixelData.at(offset + 3));

                    color =
                        qRgba(red, green, blue, alpha);
                }
            }
            else if (bitCount == 24)
            {
                const qint64 offset =
                    pixelOffset + static_cast<qint64>(x) * 3;

                if (offset + 2 < pixelData.size())
                {
                    const uchar blue =
                        static_cast<uchar>(pixelData.at(offset));

                    const uchar green =
                        static_cast<uchar>(pixelData.at(offset + 1));

                    const uchar red =
                        static_cast<uchar>(pixelData.at(offset + 2));

                    color =
                        qRgba(red, green, blue, 255);
                }
            }
            else if (bitCount == 16)
            {
                // BI_RGB and the OS/2 direct-colour format use the historical
                // 5-5-5 layout when no explicit bit masks are present.
                const qint64 offset =
                    pixelOffset + static_cast<qint64>(x) * 2;

                if (offset + 1 < pixelData.size())
                {
                    const quint16 value =
                        quint16(uchar(pixelData.at(offset))) |
                        (quint16(uchar(pixelData.at(offset + 1))) << 8);
                    const int red5 = (value >> 10) & 0x1F;
                    const int green5 = (value >> 5) & 0x1F;
                    const int blue5 = value & 0x1F;
                    color = qRgba((red5 * 255 + 15) / 31,
                                  (green5 * 255 + 15) / 31,
                                  (blue5 * 255 + 15) / 31, 255);
                }
            }
            else if (bitCount == 8
                     && !palette.isEmpty())
            {
                const qint64 offset =
                    pixelOffset + x;

                if (offset < pixelData.size())
                {
                    const int index =
                        static_cast<uchar>(
                            pixelData.at(offset));

                    if (index < palette.size())
                        color = palette.at(index);
                }
            }
            else if (bitCount == 2
                     && !palette.isEmpty())
            {
                const qint64 offset =
                    pixelOffset + (x / 4);

                if (offset < pixelData.size())
                {
                    const uchar value =
                        static_cast<uchar>(
                            pixelData.at(offset));

                    const int shift =
                        6 - ((x % 4) * 2);

                    const int index =
                        (value >> shift) & 0x03;

                    if (index < palette.size())
                        color = palette.at(index);
                }
            }
            else if (bitCount == 4
                     && !palette.isEmpty())
            {
                const qint64 offset =
                    pixelOffset + (x / 2);

                if (offset < pixelData.size())
                {
                    const uchar value =
                        static_cast<uchar>(
                            pixelData.at(offset));

                    const int index =
                        x % 2 == 0
                            ? value >> 4
                            : value & 0x0F;

                    if (index < palette.size())
                        color = palette.at(index);
                }
            }
            else if (bitCount == 1
                     && !palette.isEmpty())
            {
                const qint64 offset =
                    pixelOffset + (x / 8);

                if (offset < pixelData.size())
                {
                    const uchar value =
                        static_cast<uchar>(
                            pixelData.at(offset));

                    const int index =
                        (value >> (7 - (x % 8))) & 1;

                    if (index < palette.size())
                        color = palette.at(index);
                }
            }

            // AND mask (optional)
            if (applyMask)
            {
                const qint64 maskByteIndex =
                    static_cast<qint64>(invY) * maskStride
                    + (x / 8);

                const int maskBit =
                    7 - (x % 8);

                if (maskByteIndex < maskData.size())
                {
                    const bool isTransparent =
                        (static_cast<uchar>(
                             maskData.at(maskByteIndex))
                         & (1 << maskBit))
                        != 0;

                    if (isTransparent)
                    {
                        color =
                            qRgba(
                                qRed(color),
                                qGreen(color),
                                qBlue(color),
                                0);
                    }
                }
            }

            destination[x] = color;
        }
    }

    Img image;
    image.BitCount = bitCount;
    image.Size = QSize(width, height);
    image.Bitmap = bitmap;
    return image;
}

void CopyLarge(
    const QByteArray& source,
    qint64 sourceOffset,
    QByteArray& destination,
    int destOffset,
    qint64 count)
{
    constexpr int chunkSize =
        1024 * 1024; // 1MB chunks

    while (count > 0)
    {
        const int thisChunk =
            static_cast<int>(
                std::min<qint64>(
                    count,
                    chunkSize));

        if (sourceOffset > std::numeric_limits<int>::max())
        {
            throw std::overflow_error(
                "Source offset exceeds supported range.");
        }

        if (!IsRangeValid(
                source,
                sourceOffset,
                thisChunk))
        {
            throw std::out_of_range(
                "Source range exceeds source data.");
        }

        if (destOffset < 0
            || static_cast<qint64>(destOffset) + thisChunk
                > destination.size())
        {
            throw std::out_of_range(
                "Destination range exceeds destination data.");
        }

        std::memcpy(
            destination.data() + destOffset,
            source.constData() + sourceOffset,
            static_cast<size_t>(thisChunk));

        sourceOffset += thisChunk;
        destOffset += thisChunk;
        count -= thisChunk;
    }
}

} // namespace

Optional<Img> RT_BITMAP::GenerateBitmapFromData(
    const QByteArray& pixelData,
    const QByteArray& maskData,
    int width,
    int height,
    int bitCount,
    QVector<QRgb> palette)
{
    return GenerateBitmapFromDataImpl(
        pixelData,
        maskData,
        width,
        height,
        bitCount,
        std::move(palette));
}

QVector<Img> RT_BITMAP::getDetailed(
    const QByteArray& data)
{
    return Get(data);
}

QVector<QImage> RT_BITMAP::get(
    const QByteArray& data)
{
    const QVector<Img> images =
        getDetailed(data);

    QVector<QImage> result;
    result.reserve(images.size());

    for (const Img& image : images)
        result.append(image.Bitmap);

    return result;
}

ResourcePreview RT_BITMAP::preview(
    const ResourceEntry& entry)
{
    ResourcePreview result;

    const QVector<QImage> images =
        get(entry.data);

    if (images.isEmpty())
        return result;

    // MainWindow renders ResourcePreview::images.  Keeping only the legacy
    // single-image field made the bitmap preview show the count but no image.
    result.images = images;
    result.imageLabels.reserve(images.size());
    for (int i = 0; i < images.size(); ++i)
    {
        result.imageLabels.push_back(
            images.size() == 1
                ? QStringLiteral("Bitmap")
                : QStringLiteral("Bitmap %1").arg(i + 1));
    }
    result.text =
        QStringLiteral("%1 bitmap image(s)")
            .arg(images.size());

    return result;
}

} // namespace resources
} // namespace peare
