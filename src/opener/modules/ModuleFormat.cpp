#include "ModuleFormat.h"

#include <QFile>

#include <cstring>

namespace peare {
namespace {

quint16 readLe16(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) {
        return 0;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 readLe32(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) {
        return 0;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return quint32(p[0]) |
           (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) |
           (quint32(p[3]) << 24);
}

} // namespace

ModuleFormatInfo ModuleFormatDetector::detectFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {ModuleFormat::Unknown, 0, {}, file.errorString()};
    }

    // ISO 9660 first, cheaply: the first volume descriptor carries "CD001" at
    // sector 16 + 1 (byte 0x8001). Checked with a 5-byte positioned read so a
    // multi-GB disc image is never fully loaded just to sniff it, and never
    // mistaken for firmware by the loose FWF heuristic below.
    if (file.size() >= 0x8001 + 5) {
        char magic[5];
        if (file.seek(0x8001) && file.read(magic, 5) == 5 &&
            std::memcmp(magic, "CD001", 5) == 0) {
            return {ModuleFormat::ISO9660, 0, QStringLiteral("ISO 9660 image"), {}};
        }
        file.seek(0);
    }

    const QByteArray data = file.readAll();
    if (data.size() < 2) {
        return {ModuleFormat::Unknown, 0, {}, QStringLiteral("File troppo piccolo")};
    }


    if (data.size() >= 64 && readLe32(data, data.size() - 4) == 0x03031998U) {
        return {ModuleFormat::SIEMENS_IMG, 0, QStringLiteral("Siemens ProSave IMG firmware container"), {}};
    }

    const bool fwfHeader = data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0x03 &&
        static_cast<unsigned char>(data[1]) == 0xA1;
    if (fwfHeader || data.contains("InPlaceBlob") ||
        data.contains("FirmwareFile") || data.contains("FWF_")) {
        return {ModuleFormat::SIEMENS_FWF, 0, QStringLiteral("Siemens FWF OMS firmware archive"), {}};
    }

    if (data.size() >= 8 && data.left(8) == QByteArray("SZDD\x88\xF0\x27\x33", 8)) {
        return {ModuleFormat::SZDD, 0, QStringLiteral("Microsoft Compress SZDD archive"), {}};
    }

    if (data.size() >= 8 && std::memcmp(data.constData(), "MSWIM\0\0\0", 8) == 0) {
        return {ModuleFormat::WIM, 0, QStringLiteral("Windows Imaging (WIM) image"), {}};
    }

    if (data.size() >= 4) {
        const QByteArray magic = data.left(4);
        const unsigned char* signature = reinterpret_cast<const unsigned char*>(magic.constData());
        if (signature[0] == 0xA5 && signature[1] == 0x96 &&
            ((signature[2] == 0x0A && signature[3] == 0x00) ||
             (signature[2] == 0x0A && signature[3] == 0x0A) ||
             (signature[2] == 0x00 && signature[3] == 0x14) ||
             (signature[2] == 0x14 && signature[3] == 0x0A) ||
             (signature[2] == 0xFF && signature[3] == 0xFF) ||
             (signature[2] == 0xFE && signature[3] == 0xFF))) {
            return {ModuleFormat::OS2_PACK, 0, QStringLiteral("IBM/Microsoft OS/2 PACK archive"), {}};
        }
        if (magic == QByteArrayLiteral("XBEH")) {
            return {ModuleFormat::XBE, 0, QStringLiteral("Original Xbox Executable (XBE)"), {}};
        }
        if (magic == QByteArrayLiteral("XEX1")) {
            return {ModuleFormat::XEX, 0, QStringLiteral("Xbox 360 Executable (XEX1)"), {}};
        }
        if (magic == QByteArrayLiteral("XEX2")) {
            return {ModuleFormat::XEX, 0, QStringLiteral("Xbox 360 Executable (XEX2)"), {}};
        }
        if (magic == QByteArrayLiteral("XUIZ")) {
            return {ModuleFormat::XUIZ, 0, QStringLiteral("Xbox 360 XUIZ archive"), {}};
        }
        if (magic == QByteArrayLiteral("LIVE") || magic == QByteArrayLiteral("PIRS")) {
            return {ModuleFormat::LIVE_PIRS, 0, QStringLiteral("Xbox 360 STFS LIVE/PIRS container"), {}};
        }
        if (magic == QByteArrayLiteral("CON ")) {
            return {ModuleFormat::CON, 0, QStringLiteral("Xbox 360 STFS CON container"), {}};
        }
    }

    if (readLe16(data, 0) != 0x5A4D) {
        return {ModuleFormat::Unknown, 0, {}, QStringLiteral("Firma MZ, XBEH, XEX1 o XEX2 assente")};
    }

    ModuleFormatInfo info;
    info.format = ModuleFormat::DosMZ;
    info.description = QStringLiteral("DOS MZ executable");

    if (data.size() < 0x40) {
        return info;
    }

    const quint32 newHeaderOffset = readLe32(data, 0x3C);
    info.headerOffset = newHeaderOffset;

    if (newHeaderOffset >= quint32(data.size()) || newHeaderOffset + 2 > quint32(data.size())) {
        return info;
    }

    const quint16 signature16 = readLe16(data, newHeaderOffset);
    if (signature16 == 0x454E) {
        info.format = ModuleFormat::NE;
        info.description = QStringLiteral("New Executable (NE)");
        return info;
    }
    if (signature16 == 0x454C) {
        info.format = ModuleFormat::LE;
        info.description = QStringLiteral("Linear Executable (LE)");
        return info;
    }
    if (signature16 == 0x584C) {
        info.format = ModuleFormat::LX;
        info.description = QStringLiteral("Linear Executable (LX)");
        return info;
    }

    if (newHeaderOffset + 4 <= quint32(data.size()) && readLe32(data, newHeaderOffset) == 0x00004550) {
        info.format = ModuleFormat::PE;
        info.description = QStringLiteral("Portable Executable (PE)");
        return info;
    }

    return info;
}

ModuleFormatInfo ModuleFormatDetector::detectBuffer(const QByteArray &data)
{
    if (data.size() >= 0x8001 + 5 &&
        std::memcmp(data.constData() + 0x8001, "CD001", 5) == 0)
        return {ModuleFormat::ISO9660, 0, QStringLiteral("ISO 9660 image"), {}};

    if (data.size() >= 8 && data.left(8) == QByteArray("SZDD\x88\xF0\x27\x33", 8))
        return {ModuleFormat::SZDD, 0, QStringLiteral("Microsoft Compress SZDD archive"), {}};

    if (data.size() >= 8 && std::memcmp(data.constData(), "MSWIM\0\0\0", 8) == 0)
        return {ModuleFormat::WIM, 0, QStringLiteral("Windows Imaging (WIM) image"), {}};

    if (data.size() >= 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
        if (p[0] == 0xA5 && p[1] == 0x96 &&
            ((p[2] == 0x0A && p[3] == 0x00) || (p[2] == 0x0A && p[3] == 0x0A) ||
             (p[2] == 0x00 && p[3] == 0x14) || (p[2] == 0x14 && p[3] == 0x0A) ||
             (p[2] == 0xFF && p[3] == 0xFF) || (p[2] == 0xFE && p[3] == 0xFF)))
            return {ModuleFormat::OS2_PACK, 0, QStringLiteral("IBM/Microsoft OS/2 PACK archive"), {}};
        const QByteArray magic = data.left(4);
        if (magic == QByteArrayLiteral("XBEH")) return {ModuleFormat::XBE, 0, QStringLiteral("Original Xbox Executable (XBE)"), {}};
        if (magic == QByteArrayLiteral("XEX1") || magic == QByteArrayLiteral("XEX2")) return {ModuleFormat::XEX, 0, QStringLiteral("Xbox 360 Executable (XEX)"), {}};
        if (magic == QByteArrayLiteral("XUIZ")) return {ModuleFormat::XUIZ, 0, QStringLiteral("Xbox 360 XUIZ archive"), {}};
        if (magic == QByteArrayLiteral("LIVE") || magic == QByteArrayLiteral("PIRS")) return {ModuleFormat::LIVE_PIRS, 0, QStringLiteral("Xbox 360 STFS LIVE/PIRS container"), {}};
        if (magic == QByteArrayLiteral("CON ")) return {ModuleFormat::CON, 0, QStringLiteral("Xbox 360 STFS CON container"), {}};
    }

    if (readLe16(data, 0) == 0x5A4D && data.size() >= 0x40) {
        const quint32 nh = readLe32(data, 0x3C);
        if (nh + 2 <= quint32(data.size())) {
            const quint16 sig = readLe16(data, nh);
            if (sig == 0x454E) return {ModuleFormat::NE, nh, QStringLiteral("New Executable (NE)"), {}};
            if (sig == 0x454C) return {ModuleFormat::LE, nh, QStringLiteral("Linear Executable (LE)"), {}};
            if (sig == 0x584C) return {ModuleFormat::LX, nh, QStringLiteral("Linear Executable (LX)"), {}};
            if (nh + 4 <= quint32(data.size()) && readLe32(data, nh) == 0x00004550)
                return {ModuleFormat::PE, nh, QStringLiteral("Portable Executable (PE)"), {}};
        }
        return {ModuleFormat::DosMZ, nh, QStringLiteral("DOS MZ executable"), {}};
    }

    return {ModuleFormat::Unknown, 0, {}, QStringLiteral("No recognised header")};
}

QString ModuleFormatDetector::formatName(ModuleFormat format)
{
    switch (format) {
    case ModuleFormat::DosMZ: return QStringLiteral("MZ");
    case ModuleFormat::PE: return QStringLiteral("PE");
    case ModuleFormat::NE: return QStringLiteral("NE");
    case ModuleFormat::LE: return QStringLiteral("LE");
    case ModuleFormat::LX: return QStringLiteral("LX");
    case ModuleFormat::XEX: return QStringLiteral("XEX");
    case ModuleFormat::XBE: return QStringLiteral("XBE");
    case ModuleFormat::XUIZ: return QStringLiteral("XUIZ");
    case ModuleFormat::LIVE_PIRS: return QStringLiteral("LIVE/PIRS");
    case ModuleFormat::CON: return QStringLiteral("CON");
    case ModuleFormat::OS2_PACK: return QStringLiteral("OS/2 PACK");
    case ModuleFormat::SZDD: return QStringLiteral("SZDD");
    case ModuleFormat::SIEMENS_IMG: return QStringLiteral("Siemens IMG");
    case ModuleFormat::SIEMENS_FWF: return QStringLiteral("Siemens FWF");
    case ModuleFormat::ISO9660: return QStringLiteral("ISO 9660");
    case ModuleFormat::WIM: return QStringLiteral("WIM");
    case ModuleFormat::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

} // namespace peare
