#include "GifRecovery.h"

#include <QVector>
#include <QtGlobal>
#include <climits>

namespace peare {
namespace resources {
namespace {

constexpr int kMaximumHeaderSearch = 64 * 1024;
constexpr quint64 kMaximumPixels = 64ULL * 1024ULL * 1024ULL;
constexpr qint64 kMaximumSuffixSearchBits = 16LL * 1024LL * 1024LL;

quint16 readLe16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) |
           (quint16(quint8(data.at(offset + 1))) << 8);
}

int headerOffset(const QByteArray& data)
{
    if (data.startsWith(QByteArrayLiteral("GIF87a")) ||
        data.startsWith(QByteArrayLiteral("GIF89a")))
        return 0;

    const int limit = qMin(data.size(), kMaximumHeaderSearch);
    const QByteArray prefix = data.left(limit);
    const int oldOffset = prefix.indexOf(QByteArrayLiteral("GIF87a"));
    const int newOffset = prefix.indexOf(QByteArrayLiteral("GIF89a"));
    if (oldOffset < 0) return newOffset;
    if (newOffset < 0) return oldOffset;
    return qMin(oldOffset, newOffset);
}

bool readColorTable(const QByteArray& data, int& position, int count,
                    QVector<QRgb>& palette)
{
    if (count <= 0 || position < 0 || position > data.size() - count * 3)
        return false;
    palette.clear();
    palette.reserve(count);
    for (int index = 0; index < count; ++index) {
        palette.append(qRgb(quint8(data.at(position)),
                            quint8(data.at(position + 1)),
                            quint8(data.at(position + 2))));
        position += 3;
    }
    return true;
}

bool skipSubBlocks(const QByteArray& data, int& position)
{
    while (position < data.size()) {
        const int length = quint8(data.at(position++));
        if (length == 0)
            return true;
        if (position > data.size() - length) {
            position = data.size();
            return false;
        }
        position += length;
    }
    return false;
}

struct BlockData {
    QByteArray bytes;
    QVector<int> sourceOffsets;
    bool terminated = false;
    int endPosition = 0;
    int blockCount = 0;
    int largeBlockCount = 0;
};

BlockData collectSubBlocks(const QByteArray& data, int& position,
                           bool trackOffsets = false,
                           int maximumBytes = INT_MAX)
{
    BlockData result;
    while (position < data.size()) {
        const int length = quint8(data.at(position++));
        if (length == 0) {
            result.terminated = true;
            break;
        }
        ++result.blockCount;
        if (length >= 240)
            ++result.largeBlockCount;
        const int remainingBudget = maximumBytes - result.bytes.size();
        if (remainingBudget <= 0)
            break;
        const int available = qMin(length, qMin(data.size() - position,
                                                remainingBudget));
        result.bytes.append(data.constData() + position, available);
        if (trackOffsets) {
            result.sourceOffsets.reserve(result.sourceOffsets.size() + available);
            for (int index = 0; index < available; ++index)
                result.sourceOffsets.append(position + index);
        }
        position += available;
        if (available < length)
            break;
    }
    result.endPosition = position;
    return result;
}

BlockData findResynchronizedBlocks(const QByteArray& data, int damageOffset)
{
    BlockData best;
    if (damageOffset < 0 || damageOffset >= data.size())
        return best;

    const int searchEnd = qMin(data.size(), damageOffset + 8192);
    int bestRatio = -1;
    int bestBytes = -1;
    int examinedCandidates = 0;
    for (int candidate = damageOffset + 1; candidate < searchEnd; ++candidate) {
        if (quint8(data.at(candidate)) < 240)
            continue;
        if (++examinedCandidates > 256)
            break;
        int position = candidate;
        BlockData blocks = collectSubBlocks(data, position, false,
                                            16 * 1024 * 1024);
        if (blocks.blockCount < 8 || blocks.bytes.size() < 4096)
            continue;
        if (blocks.largeBlockCount * 10 < blocks.blockCount * 9)
            continue;
        const bool reachesPhysicalTail =
            blocks.endPosition >= data.size() - 1 || blocks.terminated;
        if (!reachesPhysicalTail)
            continue;

        const int ratio = (blocks.largeBlockCount * 10000) / blocks.blockCount;
        if (ratio >= 9900 && blocks.bytes.size() >= 16 * 1024)
            return blocks;
        if (ratio > bestRatio || (ratio == bestRatio && blocks.bytes.size() > bestBytes)) {
            best = blocks;
            bestRatio = ratio;
            bestBytes = blocks.bytes.size();
        }
    }
    return best;
}

int readCode(const QByteArray& data, qint64 bitPosition, int codeSize)
{
    const qint64 bytePosition = bitPosition >> 3;
    const int shift = int(bitPosition & 7);
    quint32 value = 0;
    for (int index = 0; index < 3; ++index) {
        const qint64 position = bytePosition + index;
        if (position < data.size())
            value |= quint32(quint8(data.at(int(position)))) << (index * 8);
    }
    return int((value >> shift) & ((1u << codeSize) - 1u));
}

enum class LzwStatus {
    EndCode,
    PixelLimit,
    InvalidCode,
    EndOfData,
    CodeLimit
};

struct LzwResult {
    QVector<quint8> pixels;
    qint64 nextBit = 0;
    qint64 errorBit = -1;
    LzwStatus status = LzwStatus::EndOfData;
};

LzwResult decodeLzw(const QByteArray& compressed, int minimumCodeSize,
                    qint64 startBit, int maximumPixels,
                    int maximumCodes = 0)
{
    LzwResult result;
    result.nextBit = startBit;
    if (minimumCodeSize < 2 || minimumCodeSize > 8 || maximumPixels <= 0)
        return result;

    const int clearCode = 1 << minimumCodeSize;
    const int endCode = clearCode + 1;
    int prefix[4096];
    quint8 suffix[4096];
    quint8 stack[4097];
    for (int index = 0; index < clearCode; ++index) {
        prefix[index] = -1;
        suffix[index] = quint8(index);
    }

    int codeSize = minimumCodeSize + 1;
    int available = clearCode + 2;
    int oldCode = -1;
    quint8 first = 0;
    int codeCount = 0;
    qint64 bitPosition = startBit;
    result.pixels.reserve(qMin(maximumPixels, 1024 * 1024));

    while (bitPosition + codeSize <= qint64(compressed.size()) * 8) {
        const qint64 codeBit = bitPosition;
        const int code = readCode(compressed, bitPosition, codeSize);
        bitPosition += codeSize;
        ++codeCount;

        if (maximumCodes > 0 && codeCount > maximumCodes) {
            result.status = LzwStatus::CodeLimit;
            result.nextBit = bitPosition;
            return result;
        }

        if (code == clearCode) {
            codeSize = minimumCodeSize + 1;
            available = clearCode + 2;
            oldCode = -1;
            continue;
        }
        if (code == endCode) {
            result.status = LzwStatus::EndCode;
            result.nextBit = bitPosition;
            return result;
        }

        if (oldCode < 0) {
            if (code < 0 || code >= clearCode) {
                result.status = LzwStatus::InvalidCode;
                result.errorBit = codeBit;
                result.nextBit = bitPosition;
                return result;
            }
            result.pixels.append(quint8(code));
            first = quint8(code);
            oldCode = code;
            if (result.pixels.size() >= maximumPixels) {
                result.status = LzwStatus::PixelLimit;
                result.nextBit = bitPosition;
                return result;
            }
            continue;
        }

        int currentCode = code;
        const int inputCode = code;
        int stackSize = 0;
        if (currentCode == available) {
            stack[stackSize++] = first;
            currentCode = oldCode;
        } else if (currentCode < 0 || currentCode > available ||
                   currentCode >= 4096) {
            result.status = LzwStatus::InvalidCode;
            result.errorBit = codeBit;
            result.nextBit = bitPosition;
            return result;
        }

        while (currentCode >= clearCode) {
            if (currentCode >= available || currentCode >= 4096 ||
                stackSize >= 4096) {
                result.status = LzwStatus::InvalidCode;
                result.errorBit = codeBit;
                result.nextBit = bitPosition;
                return result;
            }
            stack[stackSize++] = suffix[currentCode];
            currentCode = prefix[currentCode];
            if (currentCode < 0) {
                result.status = LzwStatus::InvalidCode;
                result.errorBit = codeBit;
                result.nextBit = bitPosition;
                return result;
            }
        }

        first = suffix[currentCode];
        stack[stackSize++] = first;
        while (stackSize > 0 && result.pixels.size() < maximumPixels)
            result.pixels.append(stack[--stackSize]);

        if (available < 4096) {
            prefix[available] = oldCode;
            suffix[available] = first;
            ++available;
            if (available == (1 << codeSize) && codeSize < 12)
                ++codeSize;
        }
        oldCode = inputCode;

        if (result.pixels.size() >= maximumPixels) {
            result.status = LzwStatus::PixelLimit;
            result.nextBit = bitPosition;
            return result;
        }
    }

    result.status = LzwStatus::EndOfData;
    result.nextBit = bitPosition;
    return result;
}

struct SuffixResult {
    QVector<quint8> pixels;
    bool found = false;
};

SuffixResult findRecoverableSuffix(const QByteArray& compressed,
                                   int minimumCodeSize,
                                   qint64 searchStartBit,
                                   int maximumPixels)
{
    SuffixResult best;
    if (maximumPixels < 1024 || searchStartBit < 0)
        return best;

    const int clearCode = 1 << minimumCodeSize;
    const qint64 totalBits = qint64(compressed.size()) * 8;
    const qint64 searchEnd = qMin(totalBits - 36,
                                  searchStartBit + kMaximumSuffixSearchBits);
    int fullCandidates = 0;

    for (qint64 bit = searchStartBit + 1; bit < searchEnd; ++bit) {
        for (int precedingSize = minimumCodeSize + 1;
             precedingSize <= 12; ++precedingSize) {
            if (readCode(compressed, bit, precedingSize) != clearCode)
                continue;

            const qint64 segmentStart = bit + precedingSize;
            LzwResult quick = decodeLzw(compressed, minimumCodeSize,
                                        segmentStart,
                                        qMin(maximumPixels, 4096), 2048);
            const bool promising =
                quick.pixels.size() >= 1024 ||
                ((quick.status == LzwStatus::EndCode ||
                  quick.status == LzwStatus::EndOfData) &&
                 quick.pixels.size() >= 256);
            if (!promising)
                continue;
            if (++fullCandidates > 256)
                return best;

            LzwResult full = decodeLzw(compressed, minimumCodeSize,
                                       segmentStart, maximumPixels);
            const bool reachesTail =
                full.status == LzwStatus::EndCode ||
                (full.status == LzwStatus::EndOfData &&
                 full.nextBit >= totalBits - 32);
            if (!reachesTail || full.pixels.size() < 1024)
                continue;
            if (!best.found || full.pixels.size() > best.pixels.size()) {
                best.pixels = full.pixels;
                best.found = true;
            }
        }
    }
    return best;
}

QVector<int> interlacedRows(int height)
{
    QVector<int> rows;
    rows.reserve(height);
    const int starts[] = {0, 4, 2, 1};
    const int steps[] = {8, 8, 4, 2};
    for (int pass = 0; pass < 4; ++pass)
        for (int row = starts[pass]; row < height; row += steps[pass])
            rows.append(row);
    return rows;
}

struct FrameDecode {
    QImage image;
    bool complete = false;
    int recoveredPixels = 0;
    int totalPixels = 0;
};

FrameDecode decodeFirstFrame(const QByteArray& data)
{
    FrameDecode result;
    if (data.size() < 13 ||
        (!data.startsWith(QByteArrayLiteral("GIF87a")) &&
         !data.startsWith(QByteArrayLiteral("GIF89a"))))
        return result;

    const int screenWidth = readLe16(data, 6);
    const int screenHeight = readLe16(data, 8);
    const quint8 screenPacked = quint8(data.at(10));
    const int backgroundIndex = quint8(data.at(11));
    if (screenWidth <= 0 || screenHeight <= 0 ||
        quint64(screenWidth) * quint64(screenHeight) > kMaximumPixels)
        return result;

    int position = 13;
    QVector<QRgb> globalPalette;
    if ((screenPacked & 0x80u) != 0) {
        const int count = 1 << ((screenPacked & 0x07u) + 1);
        if (!readColorTable(data, position, count, globalPalette))
            return result;
    }

    int transparentIndex = -1;
    while (position < data.size()) {
        const quint8 marker = quint8(data.at(position++));
        if (marker == 0x3B)
            return result;

        if (marker == 0x21) {
            if (position >= data.size())
                return result;
            const quint8 label = quint8(data.at(position++));
            if (label == 0xF9 && position < data.size()) {
                const int blockSize = quint8(data.at(position++));
                if (blockSize == 4 && position <= data.size() - 4) {
                    const quint8 packed = quint8(data.at(position));
                    if ((packed & 0x01u) != 0)
                        transparentIndex = quint8(data.at(position + 3));
                    position += 4;
                    if (position < data.size() && data.at(position) == 0)
                        ++position;
                } else {
                    if (position > data.size() - blockSize)
                        return result;
                    position += blockSize;
                    if (position < data.size() && data.at(position) == 0)
                        ++position;
                }
            } else if (!skipSubBlocks(data, position)) {
                return result;
            }
            continue;
        }

        if (marker != 0x2C)
            return result;
        if (position > data.size() - 9)
            return result;

        const int left = readLe16(data, position);
        const int top = readLe16(data, position + 2);
        const int width = readLe16(data, position + 4);
        const int height = readLe16(data, position + 6);
        const quint8 packed = quint8(data.at(position + 8));
        position += 9;
        if (width <= 0 || height <= 0 ||
            quint64(width) * quint64(height) > kMaximumPixels)
            return result;

        QVector<QRgb> palette = globalPalette;
        if ((packed & 0x80u) != 0) {
            const int count = 1 << ((packed & 0x07u) + 1);
            if (!readColorTable(data, position, count, palette))
                return result;
        }
        if (palette.isEmpty() || position >= data.size())
            return result;

        const int minimumCodeSize = quint8(data.at(position++));
        BlockData blocks = collectSubBlocks(data, position, true);
        const int expectedPixels = width * height;
        LzwResult prefix = decodeLzw(blocks.bytes, minimumCodeSize,
                                     0, expectedPixels);

        QVector<quint8> raster(expectedPixels, 0);
        QVector<quint8> valid(expectedPixels, 0);
        const int prefixCount = qMin(prefix.pixels.size(), expectedPixels);
        for (int index = 0; index < prefixCount; ++index) {
            raster[index] = prefix.pixels.at(index);
            valid[index] = 1;
        }

        int suffixCount = 0;
        if (prefixCount < expectedPixels && prefix.errorBit >= 0) {
            SuffixResult suffix = findRecoverableSuffix(
                blocks.bytes, minimumCodeSize, prefix.errorBit,
                expectedPixels - prefixCount);

            const int errorByte = int(prefix.errorBit / 8);
            if (errorByte >= 0 && errorByte < blocks.sourceOffsets.size()) {
                const int damageOffset = blocks.sourceOffsets.at(errorByte);
                BlockData resynchronized = findResynchronizedBlocks(data, damageOffset);
                if (!resynchronized.bytes.isEmpty()) {
                    SuffixResult physicalSuffix = findRecoverableSuffix(
                        resynchronized.bytes, minimumCodeSize, 0,
                        expectedPixels - prefixCount);
                    if (physicalSuffix.found &&
                        (!suffix.found || physicalSuffix.pixels.size() > suffix.pixels.size()))
                        suffix = physicalSuffix;
                }
            }

            if (suffix.found && prefixCount + suffix.pixels.size() <= expectedPixels) {
                int suffixStart = expectedPixels - suffix.pixels.size();
                if (suffixStart < prefixCount)
                    suffixStart = prefixCount;
                suffixCount = qMin(suffix.pixels.size(), expectedPixels - suffixStart);
                for (int index = 0; index < suffixCount; ++index) {
                    raster[suffixStart + index] = suffix.pixels.at(index);
                    valid[suffixStart + index] = 1;
                }
            }
        }

        const int canvasWidth = qMax(screenWidth, left + width);
        const int canvasHeight = qMax(screenHeight, top + height);
        if (quint64(canvasWidth) * quint64(canvasHeight) > kMaximumPixels)
            return result;

        result.image = QImage(canvasWidth, canvasHeight, QImage::Format_ARGB32);
        if (result.image.isNull())
            return FrameDecode();
        result.image.fill(Qt::transparent);

        // For a complete non-transparent frame, honour the logical background.
        if (prefixCount >= expectedPixels && transparentIndex < 0 &&
            backgroundIndex >= 0 && backgroundIndex < globalPalette.size())
            result.image.fill(globalPalette.at(backgroundIndex));

        const bool interlaced = (packed & 0x40u) != 0;
        const QVector<int> rowMap = interlaced ? interlacedRows(height)
                                               : QVector<int>();
        for (int sourceIndex = 0; sourceIndex < raster.size(); ++sourceIndex) {
            if (!valid.at(sourceIndex))
                continue;
            const int sourceRow = sourceIndex / width;
            const int x = sourceIndex % width;
            if (sourceRow >= height)
                break;
            const int y = interlaced ? rowMap.value(sourceRow, sourceRow)
                                     : sourceRow;
            const int paletteIndex = raster.at(sourceIndex);
            if (paletteIndex == transparentIndex)
                continue;
            if (paletteIndex < 0 || paletteIndex >= palette.size())
                continue;
            result.image.setPixel(left + x, top + y, palette.at(paletteIndex));
        }

        result.recoveredPixels = prefixCount + suffixCount;
        result.totalPixels = expectedPixels;
        result.complete = prefixCount >= expectedPixels &&
                          (prefix.status == LzwStatus::EndCode ||
                           prefix.status == LzwStatus::PixelLimit);
        return result;
    }
    return result;
}

} // namespace

bool GifRecovery::LooksLike(const QByteArray& data) noexcept
{
    try {
        return headerOffset(data) >= 0;
    } catch (...) {
        return false;
    }
}

bool GifRecovery::TryDecode(const QByteArray& data, QImage& image,
                            QString* description) noexcept
{
    image = QImage();
    if (description) description->clear();

    try {
        const int offset = headerOffset(data);
        if (offset < 0)
            return false;
        const QByteArray gif = offset == 0 ? data : data.mid(offset);

        FrameDecode recovered = decodeFirstFrame(gif);
        const QImage qtImage = QImage::fromData(gif, "GIF");

        if (recovered.complete && !qtImage.isNull()) {
            image = qtImage;
            if (description && offset > 0)
                *description = QStringLiteral("GIF decoded after skipping %1 leading bytes")
                    .arg(offset);
            return true;
        }

        if (!recovered.image.isNull() && recovered.recoveredPixels > 0) {
            image = recovered.image;
            if (description) {
                const int percentage = recovered.totalPixels > 0
                    ? int((quint64(recovered.recoveredPixels) * 100u) /
                          quint64(recovered.totalPixels))
                    : 0;
                *description = QStringLiteral("Recovered damaged GIF (%1% of first frame%2)")
                    .arg(percentage)
                    .arg(offset > 0
                         ? QStringLiteral(", skipped %1 leading bytes").arg(offset)
                         : QString());
            }
            return true;
        }

        if (!qtImage.isNull()) {
            image = qtImage;
            if (description)
                *description = offset > 0
                    ? QStringLiteral("GIF decoded after skipping %1 leading bytes").arg(offset)
                    : QStringLiteral("GIF decoded by Qt");
            return true;
        }
        return false;
    } catch (...) {
        image = QImage();
        if (description) description->clear();
        return false;
    }
}

} // namespace resources
} // namespace peare
