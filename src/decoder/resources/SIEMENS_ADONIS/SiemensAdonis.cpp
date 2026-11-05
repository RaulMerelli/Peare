#include "SiemensAdonis.h"

#include "../SIEMENS_MWIMAGE/SiemensMwImage.h"

#include <QTextStream>
#include <QtEndian>

namespace peare { namespace resources {
namespace {

constexpr quint32 kRuntimeBase = 0x20100000U;
constexpr int kMwImageHeaderSize = 44;
constexpr int kMaxDimension = 2048;
constexpr int kMaxImages = 4096;

struct ImageTraits {
    bool bigEndian = false;
};

struct MwImageHeader {
    quint32 width = 0;
    quint32 height = 0;
    quint32 planes = 0;
    quint32 bpp = 0;
    quint32 pitch = 0;
    quint32 field5 = 0;
    quint32 field6 = 0;
    quint32 paletteSize = 0;
    quint32 palettePointer = 0;
    quint32 bitsPointer = 0;
};

quint32 read32(const QByteArray& data, qsizetype off, bool bigEndian)
{
    if (off < 0 || off + 4 > data.size()) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + off);
    return bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
}

bool looksLikeVectorWord(quint32 word)
{
    return (word & 0xFFFFF000U) == 0xE59FF000U ||
           (word & 0xFFFFF000U) == 0xE51FF000U;
}

bool detectTraits(const QByteArray& data, ImageTraits* out)
{
    if (data.size() < 0x100) return false;
    for (int endian = 0; endian < 2; ++endian) {
        const bool bigEndian = endian != 0;
        int vectorWords = 0;
        for (int off = 0; off < 0x20; off += 4)
            if (looksLikeVectorWord(read32(data, off, bigEndian))) ++vectorWords;
        if (vectorWords < 5) continue;

        int basePointers = 0;
        const int limit = qMin(data.size(), 0x400);
        for (int off = 0; off + 4 <= limit; off += 4) {
            const quint32 value = read32(data, off, bigEndian);
            if (value >= kRuntimeBase && value < 0x28000000U) ++basePointers;
        }
        if (basePointers < 4) continue;
        if (out) out->bigEndian = bigEndian;
        return true;
    }
    return false;
}

bool containsMarker(const QByteArray& data)
{
    return data.contains(QByteArrayLiteral("ADONIS")) ||
           data.contains(QByteArrayLiteral("SMART_OS"));
}

MwImageHeader readMwImageHeader(const QByteArray& data, qsizetype off, bool bigEndian)
{
    MwImageHeader h;
    h.width = read32(data, off, bigEndian);
    h.height = read32(data, off + 4, bigEndian);
    h.planes = read32(data, off + 8, bigEndian);
    h.bpp = read32(data, off + 12, bigEndian);
    h.pitch = read32(data, off + 16, bigEndian);
    h.field5 = read32(data, off + 20, bigEndian);
    h.field6 = read32(data, off + 24, bigEndian);
    h.paletteSize = read32(data, off + 28, bigEndian);
    h.palettePointer = read32(data, off + 36, bigEndian);
    h.bitsPointer = read32(data, off + 40, bigEndian);
    return h;
}

bool validMwImage(const QByteArray& data, qsizetype off, const ImageTraits& traits,
                  MwImageHeader* outHeader, qsizetype* outBitsOffset, quint64* outPixelSize)
{
    if (off < 0 || off + kMwImageHeaderSize > data.size()) return false;
    const MwImageHeader h = readMwImageHeader(data, off, traits.bigEndian);
    if (h.width < 4 || h.width > kMaxDimension || h.height < 4 || h.height > kMaxDimension)
        return false;
    if (h.planes != 1 || (h.bpp != 1 && h.bpp != 2 && h.bpp != 4 && h.bpp != 8 &&
                          h.bpp != 16 && h.bpp != 24 && h.bpp != 32))
        return false;
    const quint64 minimumPitch = (quint64(h.width) * h.bpp + 7U) / 8U;
    if (h.pitch < minimumPitch || h.pitch > minimumPitch + 64U || h.pitch > 8192U)
        return false;
    if (h.paletteSize != 0 || h.palettePointer != 0 || h.bitsPointer < kRuntimeBase)
        return false;
    if (h.field5 > 4U || h.field6 > 16U) return false;
    const quint64 bitsOffset = quint64(h.bitsPointer) - kRuntimeBase;
    const quint64 pixelSize = quint64(h.pitch) * h.height;
    if (bitsOffset >= quint64(data.size()) || pixelSize > quint64(data.size()) - bitsOffset)
        return false;
    if (outHeader) *outHeader = h;
    if (outBitsOffset) *outBitsOffset = qsizetype(bitsOffset);
    if (outPixelSize) *outPixelSize = pixelSize;
    return true;
}

QString hex32(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

QString makeInfoText(const QByteArray& data, const ImageTraits& traits, int imageCount)
{
    QString text;
    QTextStream stream(&text);
    stream << "Format: Siemens ADONIS / SMART_OS flat ARM image\n";
    stream << "Architecture: ARM (A32)\n";
    stream << "Byte order: " << (traits.bigEndian ? "big-endian" : "little-endian") << "\n";
    stream << "Runtime base: " << hex32(kRuntimeBase) << "\n";
    stream << "Image size: " << data.size() << " bytes\n";
    stream << "Embedded MWIMAGEHDR resources: " << imageCount << "\n";
    const qsizetype adonis = data.indexOf(QByteArrayLiteral("ADONIS"));
    const qsizetype smartOs = data.indexOf(QByteArrayLiteral("SMART_OS"));
    if (adonis >= 0) stream << "ADONIS marker: file+" << hex32(quint32(adonis)) << "\n";
    if (smartOs >= 0) stream << "SMART_OS marker: file+" << hex32(quint32(smartOs)) << "\n";
    return text;
}

} // namespace

bool SiemensAdonis::recognizes(const QByteArray& data) noexcept
{
    try {
        ImageTraits traits;
        return detectTraits(data, &traits) && containsMarker(data);
    } catch (...) {
        return false;
    }
}

ResourcePreview SiemensAdonis::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    ImageTraits traits;
    if (!detectTraits(entry.data, &traits) || !containsMarker(entry.data)) {
        result.error = QStringLiteral("Invalid Siemens ADONIS flat ARM image");
        return result;
    }

    for (qsizetype off = 0; off + kMwImageHeaderSize <= entry.data.size(); off += 4) {
        MwImageHeader header;
        qsizetype bitsOffset = 0;
        quint64 pixelSize = 0;
        if (!validMwImage(entry.data, off, traits, &header, &bitsOffset, &pixelSize))
            continue;

        ResourceEntry imageEntry;
        imageEntry.type = QStringLiteral("SIEMENS_MWIMAGE");
        imageEntry.name = QStringLiteral("image_%1_%2x%3.mwi")
            .arg(result.images.size(), 2, 10, QLatin1Char('0'))
            .arg(header.width).arg(header.height);
        imageEntry.format = entry.format;
        imageEntry.dataOffset = quint64(off);
        imageEntry.data = entry.data.mid(int(off), kMwImageHeaderSize);
        imageEntry.data += entry.data.mid(int(bitsOffset), int(pixelSize));

        const ResourcePreview decoded = SiemensMwImage::preview(imageEntry);
        if (decoded.images.isEmpty()) continue;
        result.images += decoded.images;
        result.imageLabels += decoded.imageLabels;
        result.conversionImages += decoded.conversionImages;
        if (result.image.isNull()) result.image = decoded.image;
        if (result.images.size() >= kMaxImages) break;
    }

    result.text = makeInfoText(entry.data, traits, result.images.size());
    return result;
}

} } // namespace peare::resources
