// Windows CE framing is handled here; compressed payloads are decoded by
// Peare's internal classic LZX implementation.
#include "WinceCompression.h"

#include <peare/lzx_frontends.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace peare {
namespace wince {
namespace {

quint16 le16(const QByteArray& data, int off)
{
    if (off < 0 || off + 2 > data.size()) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le24(const QByteArray& data, int off)
{
    if (off < 0 || off + 3 > data.size()) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16);
}

quint32 le32(const QByteArray& data, int off)
{
    if (off < 0 || off + 4 > data.size()) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QByteArray decompressCe3Block(const QByteArray& source, quint32 outputSize)
{
    QByteArray output(int(outputSize), '\0');
    int si = 0;
    quint32 di = 0;
    while (si < source.size() && di < outputSize) {
        const quint8 flags = quint8(source.at(si++));
        for (int bit = 0; bit < 8 && si < source.size() && di < outputSize; ++bit) {
            const quint8 token = quint8(source.at(si++));
            if ((flags & (1u << bit)) == 0) {
                output[int(di++)] = char(token);
                continue;
            }

            const quint8 low = token & 0x0f;
            const quint8 high = token >> 4;
            quint32 length = 0;
            qint64 sourcePos = 0;
            if (low == 1) {
                length = 2;
                sourcePos = qint64(di) - high - 2;
            } else {
                if (si >= source.size()) return output.left(int(di));
                const quint8 next = quint8(source.at(si++));
                sourcePos = (quint32(next) << 4) | high;
                if (low == 0) {
                    if (si >= source.size()) return output.left(int(di));
                    length = quint8(source.at(si++)) + 17;
                } else {
                    length = low + 1;
                }
            }

            for (quint32 k = 0; k < length && di < outputSize; ++k) {
                const qint64 p = sourcePos + k;
                output[int(di++)] = (p >= 0 && quint64(p) < outputSize)
                    ? output.at(int(p)) : char(0);
            }
        }
    }
    output.resize(int(di));
    return output;
}

} // namespace

QByteArray decompressCe1Lzw(const QByteArray& source, quint32 outputSize)
{
    const int clearCode = 256;
    int width = 9;
    int mask = 511;
    int nextCode = 257;
    std::map<int, int> prefix;
    std::map<int, int> suffix;
    quint64 bitBuffer = 0;
    int bitCount = 0;
    int pos = 0;
    QByteArray output;
    output.reserve(int(outputSize));

    auto readCode = [&]() -> int {
        while (bitCount < width) {
            if (pos >= source.size()) return -1;
            bitBuffer |= quint64(quint8(source.at(pos++))) << bitCount;
            bitCount += 8;
        }
        const int code = int(bitBuffer & quint64(mask));
        bitBuffer >>= width;
        bitCount -= width;
        return code;
    };

    auto emit = [&](int code, QByteArray* out, int* first) -> bool {
        QByteArray stack;
        while (code >= 257) {
            const auto pi = prefix.find(code);
            const auto si = suffix.find(code);
            if (pi == prefix.end() || si == suffix.end()) return false;
            stack.append(char(si->second));
            code = pi->second;
        }
        if (code < 0 || code > 255) return false;
        stack.append(char(code));
        std::reverse(stack.begin(), stack.end());
        if (first) *first = quint8(stack.at(0));
        out->append(stack);
        return true;
    };

    int previous = -1;
    while (quint32(output.size()) < outputSize) {
        const int code = readCode();
        if (code < 0) break;
        if (code == clearCode) {
            width = 9;
            mask = 511;
            nextCode = 257;
            previous = -1;
            prefix.clear();
            suffix.clear();
            continue;
        }
        if (previous < 0) {
            if (!emit(code, &output, nullptr)) return QByteArray();
            previous = code;
            continue;
        }

        int first = 0;
        if (code < nextCode) {
            if (!emit(code, &output, &first)) return QByteArray();
        } else if (code == nextCode) {
            QByteArray previousBytes;
            int previousFirst = 0;
            if (!emit(previous, &previousBytes, &previousFirst)) return QByteArray();
            previousBytes.append(char(previousFirst));
            first = previousFirst;
            output.append(previousBytes);
        } else {
            return QByteArray();
        }

        if (nextCode < 4096) {
            prefix[nextCode] = previous;
            suffix[nextCode] = first;
            const int old = nextCode;
            ++nextCode;
            if (old == mask && mask < 0x0fff) {
                mask += nextCode;
                ++width;
            }
        }
        previous = code;
    }

    if (quint32(output.size()) < outputSize) output.append(int(outputSize - output.size()), '\0');
    output.resize(int(outputSize));
    return output;
}

QByteArray decompressCe3Bin(const QByteArray& source, quint32 outputSize)
{
    if (source.size() < 3) return QByteArray();
    const quint32 declaredSize = le24(source, 0);
    if (declaredSize == 0 || declaredSize > outputSize + 0x1000u) return QByteArray();
    const quint32 blockSize = 0x1000;
    const quint32 tableEntries = ((declaredSize - 1) / blockSize) + 2;
    const quint64 tableSize = quint64(tableEntries) * 3;
    if (tableSize > quint64(source.size())) return QByteArray();

    QByteArray output;
    output.reserve(int(outputSize));
    quint32 previousEnd = quint32(tableSize);
    for (quint32 i = 1; i < tableEntries && quint32(output.size()) < outputSize; ++i) {
        const quint32 end = le24(source, int(i * 3));
        if (end < previousEnd || end > quint32(source.size())) return QByteArray();
        const quint32 wanted = std::min(blockSize, outputSize - quint32(output.size()));
        const QByteArray block = source.mid(int(previousEnd), int(end - previousEnd));
        QByteArray decoded = decompressCe3Block(block, wanted);
        if (decoded.isEmpty() && wanted != 0) return QByteArray();
        if (quint32(decoded.size()) < wanted) decoded.append(int(wanted - decoded.size()), '\0');
        output.append(decoded.left(int(wanted)));
        previousEnd = end;
    }
    if (quint32(output.size()) != outputSize) return QByteArray();
    return output;
}

QByteArray decompressCeLzx(const QByteArray& source, quint32 outputSize)
{
    if (source.size() < 3 || outputSize == 0) return QByteArray();
    const quint32 declaredSize = le24(source, 0);
    if (declaredSize == 0 || declaredSize > outputSize + 0x1000u) return QByteArray();
    const quint32 blockSize = 0x1000;
    const quint32 tableEntries = ((declaredSize - 1) / blockSize) + 2;
    const quint64 tableSize = quint64(tableEntries) * 3;
    if (tableSize > quint64(source.size())) return QByteArray();

    QByteArray output;
    output.reserve(int(outputSize));
    quint32 previousEnd = quint32(tableSize);
    for (quint32 i = 1; i < tableEntries && quint32(output.size()) < outputSize; ++i) {
        const quint32 end = le24(source, int(i * 3));
        if (end < previousEnd || end > quint32(source.size()) || end - previousEnd < 16)
            return QByteArray();
        const QByteArray block = source.mid(int(previousEnd), int(end - previousEnd));
        const quint32 windowBits = le32(block, 0);
        const quint32 decodedSize = le32(block, 4);
        if (windowBits < 15 || windowBits > 21 || decodedSize == 0 ||
            decodedSize > outputSize - quint32(output.size()))
            return QByteArray();

        peare_lzx_cab_decoder* decoder = nullptr;
        if (peare_lzx_cab_create(size_t(1u) << windowBits, &decoder) != PEARE_LZX_OK)
            return QByteArray();
        QByteArray decoded(int(decodedSize), '\0');
        const peare_lzx_status status = peare_lzx_cab_decompress(
            decoder, block.constData() + 16, size_t(block.size() - 16),
            decoded.data(), size_t(decoded.size()));
        peare_lzx_cab_destroy(decoder);
        if (status != PEARE_LZX_OK) return QByteArray();
        output.append(decoded);
        previousEnd = end;
    }
    if (quint32(output.size()) != outputSize) return QByteArray();
    return output;
}

QByteArray decompressCeRom(const QByteArray& source, quint32 outputSize)
{
    QByteArray decoded = decompressCeLzx(source, outputSize);
    if (quint32(decoded.size()) == outputSize) return decoded;
    decoded = decompressCe3Bin(source, outputSize);
    if (quint32(decoded.size()) == outputSize) return decoded;
    return QByteArray();
}

QByteArray decompressImgfsXpress(const QByteArray& source, quint32 outputSize)
{
    QByteArray output(int(outputSize), '\0');
    int si = 0;
    quint32 di = 0;
    int nibbleIndex = -1;
    while (si < source.size() && di < outputSize) {
        if (si + 4 > source.size()) break;
        const quint32 flags = le32(source, si);
        si += 4;
        for (int bit = 31; bit >= 0 && si < source.size() && di < outputSize; --bit) {
            if ((flags & (quint32(1) << bit)) == 0) {
                output[int(di++)] = source.at(si++);
                continue;
            }
            if (si + 2 > source.size()) return QByteArray();
            const quint16 value = le16(source, si);
            si += 2;
            const quint32 matchOffset = (value >> 3) + 1;
            quint32 matchLength = value & 7;
            if (matchLength == 7) {
                if (nibbleIndex < 0) {
                    if (si >= source.size()) return QByteArray();
                    nibbleIndex = si;
                    matchLength = quint8(source.at(si++)) & 0x0f;
                } else {
                    matchLength = quint8(source.at(nibbleIndex)) >> 4;
                    nibbleIndex = -1;
                }
                if (matchLength == 15) {
                    if (si >= source.size()) return QByteArray();
                    matchLength = quint8(source.at(si++));
                    if (matchLength == 255) {
                        if (si + 2 > source.size()) return QByteArray();
                        matchLength = le16(source, si);
                        si += 2;
                        if (matchLength == 0) {
                            if (si + 4 > source.size()) return QByteArray();
                            matchLength = le32(source, si);
                            si += 4;
                        }
                        if (matchLength < 22) return QByteArray();
                        matchLength -= 22;
                    }
                    matchLength += 15;
                }
                matchLength += 7;
            }
            matchLength += 3;
            if (matchOffset > di) return QByteArray();
            const quint32 copyFrom = di - matchOffset;
            for (quint32 k = 0; k < matchLength && di < outputSize; ++k)
                output[int(di++)] = output.at(int(copyFrom + k));
        }
    }
    if (di != outputSize) return QByteArray();
    return output;
}

} // namespace wince
} // namespace peare
