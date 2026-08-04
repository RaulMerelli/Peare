#include "ModuleFormat.h"

#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace peare {
namespace {

const qint64 kOpticalMode2SectorSize = 2352;
const qint64 kOpticalMode2PayloadOffset = 24;

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

quint16 readBe16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return (quint16(p[0]) << 8) | quint16(p[1]);
}


bool isOs2EaList(const QByteArray& data)
{
    if (data.size() < 8 || readLe32(data, 0) != quint32(data.size())) return false;
    qsizetype pos = 4;
    int entries = 0;
    while (pos < data.size()) {
        if (pos + 4 > data.size()) return false;
        const quint8 nameLength = quint8(data.at(pos + 1));
        const quint16 valueLength = readLe16(data, pos + 2);
        const qsizetype valueOffset = pos + 4 + nameLength + 1;
        if (nameLength == 0 || valueOffset > data.size() ||
            data.at(pos + 4 + nameLength) != '\0' ||
            qsizetype(valueLength) > data.size() - valueOffset)
            return false;
        pos = valueOffset + valueLength;
        ++entries;
    }
    return entries > 0 && pos == data.size();
}

ModuleFormatInfo directOs2Executable(const QByteArray& data)
{
    if (data.size() >= 64 && readLe16(data, 0) == 0x454E) {
        const quint16 segments = readLe16(data, 0x1C);
        const quint16 segmentTable = readLe16(data, 0x22);
        if (segments <= 4096 && segmentTable >= 0x40 &&
            quint64(segmentTable) + quint64(segments) * 8 <= quint64(data.size()))
            return {ModuleFormat::NE, 0, QStringLiteral("New Executable (NE), stubless OS/2 image"), {}};
    }

    if (data.size() >= 0xAC) {
        const quint16 signature = readLe16(data, 0);
        if ((signature == 0x454C || signature == 0x584C) &&
            quint8(data.at(2)) == 0 && quint8(data.at(3)) == 0) {
            const quint32 pageSize = readLe32(data, 0x28);
            const quint32 objectTable = readLe32(data, 0x40);
            const quint32 objectCount = readLe32(data, 0x44);
            if (pageSize >= 512 && pageSize <= 65536 &&
                (pageSize & (pageSize - 1)) == 0 && objectCount <= 65536 &&
                objectTable >= 0xAC && objectTable < quint32(data.size())) {
                if (signature == 0x454C)
                    return {ModuleFormat::LE, 0, QStringLiteral("Linear Executable (LE), stubless OS/2 image"), {}};
                return {ModuleFormat::LX, 0, QStringLiteral("Linear Executable (LX), stubless OS/2 image"), {}};
            }
        }
    }
    return {};
}

bool isMode2IsoDescriptor(const QByteArray& data)
{
    const qint64 pos = 16 * kOpticalMode2SectorSize + kOpticalMode2PayloadOffset + 1;
    return data.size() >= pos + 5 && std::memcmp(data.constData() + pos, "CD001", 5) == 0;
}

bool isMode2UdfDescriptor(const QByteArray& data)
{
    for (int i = 0; i < 64; ++i) {
        const qint64 pos = (16 + i) * kOpticalMode2SectorSize + kOpticalMode2PayloadOffset;
        if (pos + 6 > data.size()) break;
        const char* id = data.constData() + pos + 1;
        if (std::memcmp(id, "NSR02", 5) == 0 || std::memcmp(id, "NSR03", 5) == 0)
            return true;
        if (std::memcmp(id, "BEA01", 5) != 0 && std::memcmp(id, "BOOT2", 5) != 0 &&
            std::memcmp(id, "CD001", 5) != 0 && std::memcmp(id, "CDW02", 5) != 0 &&
            std::memcmp(id, "TEA01", 5) != 0)
            break;
    }
    return false;
}

bool looksPartitionedRawDisk(const QByteArray& data, qint64 totalSize)
{
    if (data.size() < 512 || data[510] != char(0x55) || data[511] != char(0xAA))
        return false;
    for (int i = 0; i < 4; ++i) {
        const qsizetype off = 0x1BE + i * 16;
        const quint8 type = quint8(*(data.constData() + off + 4));
        const quint32 startLba = readLe32(data, off + 8);
        const quint32 sectors = readLe32(data, off + 12);
        if (type == 0 || sectors == 0) continue;
        const qint64 start = qint64(startLba) * 512;
        const qint64 length = qint64(sectors) * 512;
        if (start >= 512 && length > 0 && start < totalSize && length <= totalSize - start)
            return true;
    }
    return false;
}

bool hasAuthoritativeOs2Layout(const QByteArray& data, qint64 totalSize)
{
    if (data.size() < 512 || totalSize < 512 ||
        data.at(510) != char(0x55) || data.at(511) != char(0xAA))
        return false;
    const qint64 diskSectors = totalSize / 512;
    for (int i = 0; i < 4; ++i) {
        const qsizetype off = 0x1BE + i * 16;
        const quint8 type = quint8(data.at(off + 4));
        if (type != 0x35 && type != 0x05 && type != 0x0F && type != 0x85)
            continue;
        const quint32 startLba = readLe32(data, off + 8);
        const quint32 sectors = readLe32(data, off + 12);
        if (startLba != 0 && sectors != 0 && qint64(startLba) < diskSectors)
            return true;
    }
    return false;
}

bool validJfsSuperblock(const QByteArray& sb)
{
    if (sb.size() < 24 || std::memcmp(sb.constData(), "JFS1", 4) != 0)
        return false;
    const quint32 version = readLe32(sb, 4);
    const quint32 blockSize = readLe32(sb, 16);
    const quint16 blockShift = readLe16(sb, 20);
    return (version == 1 || version == 2) && blockSize >= 512 && blockSize <= 4096 &&
           (blockSize & (blockSize - 1)) == 0 && blockShift >= 9 && blockShift <= 12 &&
           (quint32(1) << blockShift) == blockSize;
}

bool hasRecoverableJfsPartition(const QByteArray& data, qint64 totalSize)
{
    if (data.size() < 512 || totalSize < 0x8000 + 24) return false;
    for (int i = 0; i < 4; ++i) {
        const qsizetype entry = 0x1BE + i * 16;
        const quint8 type = quint8(data.at(entry + 4));
        const quint32 startLba = readLe32(data, entry + 8);
        const quint32 sectors = readLe32(data, entry + 12);
        if (type == 0 || startLba == 0 || sectors == 0) continue;
        const qint64 start = qint64(startLba) * 512;
        if (start <= 0 || start >= totalSize) continue;
        const qint64 offsets[] = {0x8000, 0xF000};
        for (qint64 off : offsets) {
            const qint64 absolute = start + off;
            if (absolute < start || absolute + 24 > totalSize || absolute + 24 > data.size())
                continue;
            if (validJfsSuperblock(data.mid(qsizetype(absolute), 24))) return true;
        }
    }
    return false;
}

bool hasRecoverableJfsPartition(QFile& file, const QByteArray& firstSector)
{
    if (firstSector.size() < 512 || file.size() < 0x8000 + 24) return false;
    for (int i = 0; i < 4; ++i) {
        const qsizetype entry = 0x1BE + i * 16;
        const quint8 type = quint8(firstSector.at(entry + 4));
        const quint32 startLba = readLe32(firstSector, entry + 8);
        const quint32 sectors = readLe32(firstSector, entry + 12);
        if (type == 0 || startLba == 0 || sectors == 0) continue;
        const qint64 start = qint64(startLba) * 512;
        if (start <= 0 || start >= file.size()) continue;
        const qint64 offsets[] = {0x8000, 0xF000};
        for (qint64 off : offsets) {
            const qint64 absolute = start + off;
            if (absolute < start || absolute + 24 > file.size()) continue;
            if (file.seek(absolute) && validJfsSuperblock(file.read(24))) {
                file.seek(0);
                return true;
            }
        }
    }
    file.seek(0);
    return false;
}


bool looksApplePartitionMap(const QByteArray& data)
{
    return data.size() >= 1024 && readBe16(data, 0) == 0x4552 &&
           readBe16(data, 512) == 0x504d;
}

bool looksLinuxRaidSuperblock(const QByteArray& data, qsizetype offset, quint32 expectedMinor)
{
    if (offset < 0 || offset + 512 > data.size()) return false;
    if (readLe32(data, offset) != 0xa92b4efcU) return false;
    if (expectedMinor == 9)
        return readLe32(data, offset + 4) == 0 && readLe32(data, offset + 8) == 9;
    return readLe32(data, offset + 4) == 1;
}

qint64 alignDown(qint64 value, qint64 alignment)
{
    if (value <= 0) return 0;
    return (value / alignment) * alignment;
}

bool isZipHeader(const QByteArray& data)
{
    return data.size() >= 4 &&
           (std::memcmp(data.constData(), "PK\003\004", 4) == 0 ||
            std::memcmp(data.constData(), "PK\005\006", 4) == 0 ||
            std::memcmp(data.constData(), "PK\007\010", 4) == 0);
}

int b000ffSignatureLength(const QByteArray& data)
{
    if (data.size() >= 8 && std::memcmp(data.constData(), "B000FF\r\n", 8) == 0)
        return 8;
    if (data.size() >= 7 && std::memcmp(data.constData(), "B000FF\n", 7) == 0)
        return 7;
    // A few OEM tools omit the line terminator. Accept that form only when the
    // following image range is structurally plausible, to avoid claiming text.
    if (data.size() >= 14 && std::memcmp(data.constData(), "B000FF", 6) == 0) {
        const quint32 start = readLe32(data, 6);
        const quint32 length = readLe32(data, 10);
        if (start != 0 && length != 0) return 6;
    }
    return 0;
}

bool isB000ffHeader(const QByteArray& data)
{
    const int sig = b000ffSignatureLength(data);
    if (!sig || data.size() < sig + 8) return false;
    const quint32 start = readLe32(data, sig);
    const quint32 length = readLe32(data, sig + 4);
    return start != 0 && length != 0;
}

bool isSiemensFwfHeader(const QByteArray& data)
{
    return data.size() >= 2 && quint8(data.at(0)) == 0x03 &&
           quint8(data.at(1)) == 0xA1;
}

std::int64_t tarOctal(const char* p, int n)
{
    std::int64_t value = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] == 0 || p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        value = value * 8 + (p[i] - '0');
    }
    return value;
}

bool isTarHeader(const QByteArray& data)
{
    if (data.size() < 512) return false;
    bool zero = true;
    for (int i = 0; i < 512; ++i) {
        if (data.at(i) != 0) { zero = false; break; }
    }
    if (zero) return false;
    unsigned int sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? ' ' : uchar(data.at(i));
    const std::int64_t stored = tarOctal(data.constData() + 148, 8);
    if (stored <= 0 || stored != std::int64_t(sum)) return false;
    return data.at(0) != 0;
}

bool plausibleWindowsCeRomHdr(const QByteArray& data, qsizetype off, bool ce1)
{
    const qsizetype entrySize = ce1 ? 0x130 : 32;
    if (off < 0 || off + 84 + entrySize > data.size()) return false;
    const quint32 physFirst = readLe32(data, off + 8);
    const quint32 physLast = readLe32(data, off + 12);
    const quint32 modules = readLe32(data, off + 16);
    const quint32 files = readLe32(data, off + 48);
    if (physFirst < 0x80000000U || physFirst >= 0xC0000000U ||
        physLast <= physFirst || modules == 0 || modules > 4096 || files > 50000)
        return false;
    if (quint64(off) + 84 + quint64(modules) * entrySize > quint64(data.size())) return false;
    const qsizetype toc = off + 84;
    if (ce1) {
        const qsizetype name = toc + 0x10;
        bool terminated = false;
        bool nonempty = false;
        for (int i = 0; i < 260 && name + i < data.size(); ++i) {
            const uchar c = uchar(data.at(name + i));
            if (!c) { terminated = true; break; }
            if (c < 0x20 || c > 0x7e) return false;
            nonempty = true;
        }
        if (!terminated || !nonempty) return false;
        const quint32 ntVa = readLe32(data, toc + 0x124);
        return ntVa >= physFirst && quint64(ntVa - physFirst) + 4 <= quint64(data.size()) &&
               readLe32(data, qsizetype(ntVa - physFirst)) == 0x00004550U;
    }
    const quint32 nameVa = readLe32(data, toc + 16);
    const quint32 e32Va = readLe32(data, toc + 20);
    if (nameVa < physFirst || quint64(nameVa - physFirst) >= quint64(data.size()) ||
        e32Va < physFirst || quint64(e32Va - physFirst) + 2 > quint64(data.size())) return false;
    const qsizetype name = qsizetype(nameVa - physFirst);
    const quint16 objects = readLe16(data, qsizetype(e32Va - physFirst));
    return name >= 0 && name < data.size() && uchar(data.at(name)) >= 0x20 &&
           uchar(data.at(name)) <= 0x7e && objects > 0 && objects <= 96;
}


bool fileContainsImgfs(QFile& file)
{
    if (file.size() <= 0 || file.size() > 512LL * 1024LL * 1024LL) return false;
    static const char uuidBytes[16] = {
        char(0xF8), char(0xAC), char(0x2C), char(0x9D), char(0xE3), char(0xD4), char(0x2B), char(0x4D),
        char(0xBD), char(0x30), char(0x91), char(0x6E), char(0xD8), char(0x4F), char(0x31), char(0xDC)
    };
    const QByteArray uuid(uuidBytes, 16);
    const qint64 stride = 8LL * 1024LL * 1024LL;
    for (qint64 base = 0; base < file.size(); base += stride) {
        if (!file.seek(base)) break;
        const QByteArray chunk = file.read(qMin<qint64>(stride + 0x28, file.size() - base));
        int pos = chunk.indexOf(uuid);
        while (pos >= 0) {
            const qint64 absolute = base + pos;
            if ((absolute & 0xfff) == 0 && pos + 0x28 <= chunk.size() &&
                readLe32(chunk, pos + 0x1c) == 0x34) {
                const quint32 block = readLe32(chunk, pos + 0x24);
                if (block >= 0x200 && block <= 0x10000) return true;
            }
            pos = chunk.indexOf(uuid, pos + 1);
        }
    }
    return false;
}

bool isWindowsCeRomHeader(const QByteArray& data)
{
    if (isB000ffHeader(data)) return true;
    if (data.size() >= 6 && std::memcmp(data.constData(), "NOSAJ\0", 6) == 0)
        return true;
    if (data.size() >= 16 && std::memcmp(data.constData(), "ARNOLDBOOTBLOCK\0", 16) == 0)
        return true;
    if (data.size() >= 5 && std::memcmp(data.constData(), "iPAQ ", 5) == 0)
        return true;

    static const char imgfsUuid[16] = {
        char(0xF8), char(0xAC), char(0x2C), char(0x9D), char(0xE3), char(0xD4), char(0x2B), char(0x4D),
        char(0xBD), char(0x30), char(0x91), char(0x6E), char(0xD8), char(0x4F), char(0x31), char(0xDC)
    };
    int uuid = data.indexOf(QByteArray(imgfsUuid, 16));
    while (uuid >= 0) {
        if ((uuid & 0xfff) == 0 && uuid + 0x28 <= data.size() &&
            readLe32(data, uuid + 0x1c) == 0x34) {
            const quint32 block = readLe32(data, uuid + 0x24);
            if (block >= 0x200 && block <= 0x10000) return true;
        }
        uuid = data.indexOf(QByteArray(imgfsUuid, 16), uuid + 1);
    }

    // A normal DOS/PE executable may contain arbitrary ECEC bytes or values
    // resembling ROMHDR fields; it remains an executable, not a ROM archive.
    if (data.size() >= 2 && readLe16(data, 0) == 0x5a4d) return false;

    int pos = data.indexOf(QByteArrayLiteral("ECEC"));
    while (pos >= 0 && pos + 12 <= data.size()) {
        const quint32 ptoc = readLe32(data, pos + 4);
        if (ptoc >= 0x80000000U && ptoc < 0xC0000000U) return true;
        pos = data.indexOf(QByteArrayLiteral("ECEC"), pos + 4);
    }

    // Structural CE1/CE2 detection is a fallback for flat ROM dumps only.
    for (qsizetype off = 0; off + 84 <= data.size(); off += 4) {
        if (plausibleWindowsCeRomHdr(data, off, false) ||
            plausibleWindowsCeRomHdr(data, off, true)) return true;
    }
    return false;
}

ModuleFormatInfo detectOpticalBuffer(const QByteArray& data)
{
    if (data.size() >= 17 * kOpticalMode2SectorSize) {
        if (isMode2UdfDescriptor(data))
            return {ModuleFormat::UDF, 0, QStringLiteral("UDF (raw Mode 2 optical image)"), {}};
        if (isMode2IsoDescriptor(data))
            return {ModuleFormat::ISO9660, 0, QStringLiteral("ISO 9660 (raw Mode 2 optical image)"), {}};
    }

    // UDF volume recognition sequence, scanned from 0x8000 before ISO 9660.
    for (int i = 0; i < 64; ++i) {
        const qsizetype off = qsizetype(0x8000) + qsizetype(i) * 2048;
        if (off + 6 > data.size()) break;
        const char* id = data.constData() + off + 1;
        if (std::memcmp(id, "NSR02", 5) == 0 || std::memcmp(id, "NSR03", 5) == 0)
            return {ModuleFormat::UDF, 0, QStringLiteral("UDF (Universal Disk Format) image"), {}};
        if (std::memcmp(id, "BEA01", 5) != 0 && std::memcmp(id, "BOOT2", 5) != 0 &&
            std::memcmp(id, "CD001", 5) != 0 && std::memcmp(id, "CDW02", 5) != 0 &&
            std::memcmp(id, "TEA01", 5) != 0)
            break;
    }

    if (data.size() >= 0x8001 + 5 &&
        std::memcmp(data.constData() + 0x8001, "CD001", 5) == 0)
        return {ModuleFormat::ISO9660, 0, QStringLiteral("ISO 9660 image"), {}};

    return {};
}

} // namespace

ModuleFormatInfo ModuleFormatDetector::detectFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {ModuleFormat::Unknown, 0, {}, file.errorString()};
    }

    // Microsoft Full Flash Update: the security header starts at byte zero and
    // carries the 12-byte "SignedImage " signature at offset four.
    if (file.size() >= 16) {
        char signature[12];
        if (file.seek(4) && file.read(signature, 12) == 12 &&
            std::memcmp(signature, "SignedImage ", 12) == 0) {
            file.seek(0);
            return {ModuleFormat::FFU, 0, QStringLiteral("Microsoft Full Flash Update (FFU)"), {}};
        }
        file.seek(0);
    }

    // Siemens containers must be recognised from positioned reads. Falling
    // through to the generic detector would read an entire multi-gigabyte file.
    if (file.size() >= 31) {
        const QByteArray head = file.read(64);
        file.seek(0);
        if (head.size() >= 31 && std::memcmp(head.constData(), "FSF\0", 4) == 0 &&
            readBe16(head, 20) == 1 && readBe16(head, 22) >= 15 &&
            readBe16(head, 24) == 1 && quint8(head.at(30)) > 0)
            return {ModuleFormat::SIEMENS_FSF, 0,
                    QStringLiteral("Siemens FSF Windows CE flash-file archive"), {}};
        if (isSiemensFwfHeader(head))
            return {ModuleFormat::SIEMENS_FWF, 0,
                    QStringLiteral("Siemens FWF OMS firmware archive"), {}};
    }
    if (file.size() >= 4) {
        char tailMagic[4];
        if (file.seek(file.size() - 4) && file.read(tailMagic, 4) == 4) {
            const QByteArray tail(tailMagic, 4);
            if (readLe32(tail, 0) == 0x03031998U) {
                file.seek(0);
                return {ModuleFormat::SIEMENS_IMG, 0,
                        QStringLiteral("Siemens ProSave IMG firmware container"), {}};
            }
        }
        file.seek(0);
    }

    // Windows CE wrapper/XIP headers live near the beginning. IMGFS can be
    // placed later in an NB0 image, so known CE firmware extensions receive a
    // bounded whole-image UUID scan without penalising unrelated disk images.
    const QByteArray winceHead = file.read(qMin<qint64>(file.size(), 1024LL * 1024LL));
    file.seek(0);
    const QString winceSuffix = QFileInfo(filePath).suffix().toLower();
    const bool scanImgfs = winceSuffix == QStringLiteral("bin") ||
                           winceSuffix == QStringLiteral("nb0") ||
                           winceSuffix == QStringLiteral("nbf") ||
                           winceSuffix == QStringLiteral("fim") ||
                           winceSuffix == QStringLiteral("rom") ||
                           winceSuffix == QStringLiteral("imgfs");
    const ModuleFormatInfo optical = detectOpticalBuffer(winceHead);
    if (optical.format != ModuleFormat::Unknown) {
        file.seek(0);
        return optical;
    }
    if (isWindowsCeRomHeader(winceHead) ||
        (scanImgfs && file.size() > winceHead.size() && fileContainsImgfs(file))) {
        file.seek(0);
        return {ModuleFormat::WINCE_ROM, 0, QStringLiteral("Windows CE ROM / IMGFS"), {}};
    }
    file.seek(0);

    // VMDK: KDMV sparse magic, or a text disk descriptor, at offset 0. Probed
    // before the full read below so multi-GB virtual disks are never slurped into
    // a QByteArray (which is capped at INT_MAX and would fail the whole detection).
    if (file.size() >= 4) {
        char head[64] = {0};
        const qint64 n = file.seek(0) ? file.read(head, 64) : 0;
        file.seek(0);
        if (n >= 4 && std::memcmp(head, "KDMV", 4) == 0)
            return {ModuleFormat::VMDK, 0, QStringLiteral("VMware Virtual Disk (VMDK)"), {}};
        if (n >= 21 && std::memcmp(head, "# Disk DescriptorFile", 21) == 0)
            return {ModuleFormat::VMDK, 0, QStringLiteral("VMware Virtual Disk (VMDK)"), {}};
    }

    // VHD fixed disks identify themselves only with the trailing footer. Dynamic
    // and differencing disks usually also mirror the same footer at the front.
    if (file.size() >= 512) {
        char footer[8];
        char dmgFooter[4];
        if (file.seek(file.size() - 512) && file.read(dmgFooter, 4) == 4 &&
            std::memcmp(dmgFooter, "koly", 4) == 0) {
            file.seek(0);
            return {ModuleFormat::DMG, 0, QStringLiteral("Apple UDIF disk image (DMG)"), {}};
        }
        file.seek(0);
        if (file.seek(file.size() - 512) && file.read(footer, 8) == 8 &&
            std::memcmp(footer, "conectix", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::VHD, 0, QStringLiteral("Microsoft Virtual Hard Disk (VHD)"), {}};
        }
        file.seek(0);
        if (file.read(footer, 8) == 8 && std::memcmp(footer, "conectix", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::VHD, 0, QStringLiteral("Microsoft Virtual Hard Disk (VHD)"), {}};
        }
        file.seek(0);
    }

    // VHDX: the 64 KiB file header starts with little-endian "vhdxfile".
    if (file.size() >= 8) {
        char magic[8];
        if (file.seek(0) && file.read(magic, 8) == 8 &&
            std::memcmp(magic, "vhdxfile", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::VHDX, 0, QStringLiteral("Microsoft Virtual Hard Disk v2 (VHDX)"), {}};
        }
        file.seek(0);
    }

    if (file.size() >= 4) {
        const QByteArray head = file.read(512);
        if (isZipHeader(head)) {
            file.seek(0);
            return {ModuleFormat::ZIP, 0, QStringLiteral("ZIP archive"), {}};
        }
        if (isTarHeader(head)) {
            file.seek(0);
            return {ModuleFormat::TAR, 0, QStringLiteral("TAR archive"), {}};
        }
        file.seek(0);
    }

    // Microsoft System Deployment Image.
    if (file.size() >= 8) {
        char magic[8];
        if (file.seek(0) && file.read(magic, 8) == 8 &&
            std::memcmp(magic, "$SDI0001", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::SDI, 0, QStringLiteral("System Deployment Image (SDI)"), {}};
        }
        file.seek(0);
    }

    // XVA: a TAR archive whose first member is the appliance descriptor ova.xml.
    if (file.size() >= 512) {
        char tarHead[512] = {0};
        if (file.seek(0) && file.read(tarHead, 512) == 512 &&
            std::memcmp(tarHead, "ova.xml", 7) == 0) {
            file.seek(0);
            return {ModuleFormat::XVA, 0, QStringLiteral("Xen Virtual Appliance (XVA)"), {}};
        }
        file.seek(0);
    }

    // Linux swap stores its magic in the last 10 bytes of the first 4 KiB page.
    if (file.size() >= 4096) {
        char magic[10];
        if (file.seek(4096 - 10) && file.read(magic, 10) == 10 &&
            (std::memcmp(magic, "SWAP-SPACE", 10) == 0 ||
             std::memcmp(magic, "SWAPSPACE2", 10) == 0)) {
            file.seek(0);
            return {ModuleFormat::SWAP, 0, QStringLiteral("Linux swap"), {}};
        }
        file.seek(0);
    }

    // LVM2 physical volume label is normally in one of the first four sectors.
    if (file.size() >= 4 * 512) {
        for (int i = 0; i < 4; ++i) {
            char sector[512];
            if (!file.seek(qint64(i) * 512) || file.read(sector, 512) != 512)
                break;
            if (std::memcmp(sector, "LABELONE", 8) == 0 &&
                std::memcmp(sector + 0x18, "LVM2 001", 8) == 0) {
                file.seek(0);
                return {ModuleFormat::LVM, 0, QStringLiteral("Linux LVM2 physical volume"), {}};
            }
        }
        file.seek(0);
    }

    // Linux MD RAID member superblocks. DiscUtils checks v1.1 at the start,
    // v1.2 at 4 KiB, v1.0 near the end, and v0.90 near the end.
    if (file.size() >= 512) {
        const qint64 offsets[] = {
            0,
            4096,
            alignDown(file.size() - 8192, 65536),
            alignDown(file.size() - 65536, 512)
        };
        const quint32 minors[] = {1, 2, 0, 9};
        for (int i = 0; i < 4; ++i) {
            if (offsets[i] < 0 || offsets[i] + 512 > file.size())
                continue;
            if (file.seek(offsets[i])) {
                const QByteArray sb = file.read(512);
                if (looksLinuxRaidSuperblock(sb, 0, minors[i])) {
                    file.seek(0);
                    return {ModuleFormat::LINUX_RAID, 0, QStringLiteral("Linux MD RAID member"), {}};
                }
            }
        }
        file.seek(0);
    }

    if (file.size() >= 0xc00 + 512) {
        char priv[8];
        if (file.seek(0xc00) && file.read(priv, 8) == 8 &&
            std::memcmp(priv, "PRIVHEAD", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::DYNAMIC_DISK, 0, QStringLiteral("Windows Dynamic Disk (LDM)"), {}};
        }
        file.seek(0);
    }

    // Standalone JFS volumes can carry a 55AA boot signature and arbitrary
    // bytes in the MBR table area. Recognise the authoritative aggregate
    // superblock before treating those bytes as a partition table.
    QByteArray firstSector;
    if (file.size() >= 512 && file.seek(0))
        firstSector = file.read(512);
    const bool authoritativeOs2Layout = hasAuthoritativeOs2Layout(firstSector, file.size());
    const qint64 earlyJfsOffsets[] = {0x8000, 0xF000};
    for (qint64 superOffset : earlyJfsOffsets) {
        if (file.size() < superOffset + 24) continue;
        char raw[24];
        if (!file.seek(superOffset) || file.read(raw, sizeof(raw)) != qint64(sizeof(raw)))
            continue;
        const QByteArray sb(raw, int(sizeof(raw)));
        const quint32 version = readLe32(sb, 4);
        const quint32 blockSize = readLe32(sb, 16);
        const quint16 blockShift = readLe16(sb, 20);
        if (std::memcmp(raw, "JFS1", 4) == 0 &&
            (version == 1 || version == 2) && blockSize >= 512 && blockSize <= 4096 &&
            (blockSize & (blockSize - 1)) == 0 && blockShift >= 9 && blockShift <= 12 &&
            (quint32(1) << blockShift) == blockSize) {
            if (!authoritativeOs2Layout) {
                file.seek(0);
                return {ModuleFormat::JFS, 0, QStringLiteral("IBM Journaled File System (JFS) volume"), {}};
            }
            break;
        }
    }
    file.seek(0);
    if (hasRecoverableJfsPartition(file, firstSector))
        return {ModuleFormat::RAW_DISK, 0, QStringLiteral("Raw disk image with JFS partition"), {}};

    // Stubless OS/2 executables begin directly with NE/LE/LX. Also recognise
    // complete FEALIST sidecars before the permissive MBR probe.
    if (file.size() >= 4) {
        const QByteArray prefix = file.read(qMin<qint64>(file.size(), 256LL * 1024LL));
        file.seek(0);
        const ModuleFormatInfo direct = directOs2Executable(prefix);
        if (direct.format != ModuleFormat::Unknown) return direct;
        if (file.size() <= 64LL * 1024LL * 1024LL &&
            prefix.size() >= 4 && readLe32(prefix, 0) == quint32(file.size())) {
            const QByteArray candidate = file.readAll();
            file.seek(0);
            if (isOs2EaList(candidate))
                return {ModuleFormat::OS2_EA, 0, QStringLiteral("OS/2 extended attribute list (FEALIST)"), {}};
        }
    }

    if (file.size() >= 512) {
        char mbr[512];
        if (file.seek(0) && file.read(mbr, 512) == 512 &&
            (looksPartitionedRawDisk(QByteArray(mbr, 512), file.size()) ||
             hasAuthoritativeOs2Layout(QByteArray(mbr, 512), file.size()))) {
            file.seek(0);
            return {ModuleFormat::RAW_DISK, 0, QStringLiteral("Raw disk image"), {}};
        }
        file.seek(0);
    }

    if (file.size() >= 1024) {
        char head[1024];
        if (file.seek(0) && file.read(head, 1024) == 1024 &&
            looksApplePartitionMap(QByteArray(head, 1024))) {
            file.seek(0);
            return {ModuleFormat::RAW_DISK, 0, QStringLiteral("Raw Apple Partition Map disk image"), {}};
        }
        file.seek(0);
    }

    // VDI: 64-byte text pre-header followed by little-endian signature 0xBEDA107F.
    if (file.size() >= 72) {
        char pre[72];
        if (file.seek(0) && file.read(pre, 72) == 72) {
            const QByteArray data(pre, 72);
            if (readLe32(data, 64) == 0xBEDA107FU) {
                file.seek(0);
                return {ModuleFormat::VDI, 0, QStringLiteral("VirtualBox Disk Image (VDI)"), {}};
            }
        }
        file.seek(0);
    }

    // ext2/3/4 superblock magic 0xEF53 at offset 0x438 (1024 + 56), probed cheaply
    // so a multi-GB ext image is not fully read just to sniff it.
    if (file.size() >= 0x438 + 2) {
        char m[2];
        if (file.seek(0x438) && file.read(m, 2) == 2 &&
            static_cast<unsigned char>(m[0]) == 0x53 && static_cast<unsigned char>(m[1]) == 0xEF) {
            file.seek(0);
            return {ModuleFormat::EXT, 0, QStringLiteral("ext2/ext3/ext4 volume"), {}};
        }
        file.seek(0);
    }

    if (file.size() >= 0x10040 + 8) {
        char magic[8];
        if (file.seek(0x10040) && file.read(magic, 8) == 8 &&
            std::memcmp(magic, "_BHRfS_M", 8) == 0) {
            file.seek(0);
            return {ModuleFormat::BTRFS, 0, QStringLiteral("Btrfs volume"), {}};
        }
        file.seek(0);
    }

    // IBM JFS aggregate superblocks live at fixed offsets 0x8000 and 0xF000.
    // Probe both so a volume with a damaged primary copy remains discoverable.
    const qint64 jfsSuperOffsets[] = {0x8000, 0xF000};
    for (qint64 superOffset : jfsSuperOffsets) {
        if (file.size() < superOffset + 24) continue;
        char raw[24];
        if (!file.seek(superOffset) || file.read(raw, sizeof(raw)) != qint64(sizeof(raw)))
            continue;
        const QByteArray sb(raw, int(sizeof(raw)));
        const quint32 version = readLe32(sb, 4);
        const quint32 blockSize = readLe32(sb, 16);
        const quint16 blockShift = readLe16(sb, 20);
        if (std::memcmp(raw, "JFS1", 4) == 0 &&
            (version == 1 || version == 2) &&
            blockSize >= 512 && blockSize <= 4096 &&
            (blockSize & (blockSize - 1)) == 0 &&
            blockShift >= 9 && blockShift <= 12 &&
            (quint32(1) << blockShift) == blockSize) {
            file.seek(0);
            return {ModuleFormat::JFS, 0, QStringLiteral("IBM Journaled File System (JFS) volume"), {}};
        }
    }
    file.seek(0);

    // OS/2 HPFS is identified authoritatively by the two superblock magics at
    // sector 16. Some real installations have a rewritten/minimal boot sector
    // whose cosmetic "HPFS    " field is absent, so do not make that field a
    // prerequisite for opening an otherwise valid volume.
    if (file.size() >= 17 * 512) {
        char superRaw[24];
        if (file.seek(16 * 512) &&
            file.read(superRaw, sizeof(superRaw)) == qint64(sizeof(superRaw))) {
            const QByteArray sb(superRaw, int(sizeof(superRaw)));
            if (readLe32(sb, 0) == 0xF995E849U &&
                readLe32(sb, 4) == 0xFA53E9C5U &&
                readLe32(sb, 12) >= 18 && readLe32(sb, 16) >= 20) {
                file.seek(0);
                return {ModuleFormat::HPFS, 0, QStringLiteral("IBM/Microsoft HPFS volume"), {}};
            }
        }
        file.seek(0);
    }

    if (file.size() >= 4) {
        char magic[4];
        if (file.seek(0) && file.read(magic, 4) == 4 &&
            std::memcmp(magic, "XFSB", 4) == 0) {
            file.seek(0);
            return {ModuleFormat::XFS, 0, QStringLiteral("XFS volume"), {}};
        }
        file.seek(0);
    }

    if (file.size() >= 4) {
        char magic[4];
        if (file.seek(0) && file.read(magic, 4) == 4 &&
            std::memcmp(magic, "hsqs", 4) == 0) {
            file.seek(0);
            return {ModuleFormat::SQUASHFS, 0, QStringLiteral("SquashFS image"), {}};
        }
        file.seek(0);
    }

    if (file.size() >= 1026) {
        char sig[2];
        if (file.seek(1024) && file.read(sig, 2) == 2) {
            const quint16 hfsSig =
                (quint16(static_cast<unsigned char>(sig[0])) << 8) |
                quint16(static_cast<unsigned char>(sig[1]));
            if (hfsSig == 0x482b) {
                file.seek(0);
                return {ModuleFormat::HFSPLUS, 0, QStringLiteral("HFS+ volume"), {}};
            }
            if (hfsSig == 0x4858) {
                file.seek(0);
                return {ModuleFormat::HFSPLUS, 0, QStringLiteral("HFSX volume"), {}};
            }
        }
        file.seek(0);
    }

    if (file.size() >= 17 * kOpticalMode2SectorSize &&
        file.size() % kOpticalMode2SectorSize == 0) {
        const qint64 bytes = qMin<qint64>(file.size(), 80 * kOpticalMode2SectorSize);
        if (file.seek(0)) {
            const QByteArray mode2Head = file.read(bytes);
            if (isMode2UdfDescriptor(mode2Head)) {
                file.seek(0);
                return {ModuleFormat::UDF, 0, QStringLiteral("UDF (raw Mode 2 optical image)"), {}};
            }
            if (isMode2IsoDescriptor(mode2Head)) {
                file.seek(0);
                return {ModuleFormat::ISO9660, 0, QStringLiteral("ISO 9660 (raw Mode 2 optical image)"), {}};
            }
        }
        file.seek(0);
    }

    // UDF volume recognition sequence (== DiscUtils UdfReader.Detect): scan the
    // VRS at 0x8000 in 2048-byte steps for an NSR02/NSR03 marker. Checked before
    // ISO 9660 so UDF bridge discs (CD001 + NSR) open as UDF, which is the richer
    // intended file system on such media.
    {
        const qint64 vrsStart = 0x8000;
        for (int i = 0; i < 64; ++i) {
            const qint64 s = vrsStart + qint64(i) * 2048;
            if (s + 6 > file.size()) break;
            char vsd[6];
            if (!file.seek(s) || file.read(vsd, 6) != 6) break;
            const char* id = vsd + 1;
            if (std::memcmp(id, "NSR02", 5) == 0 || std::memcmp(id, "NSR03", 5) == 0) {
                file.seek(0);
                return {ModuleFormat::UDF, 0, QStringLiteral("UDF (Universal Disk Format) image"), {}};
            }
            if (std::memcmp(id, "BEA01", 5) != 0 && std::memcmp(id, "BOOT2", 5) != 0 &&
                std::memcmp(id, "CD001", 5) != 0 && std::memcmp(id, "CDW02", 5) != 0 &&
                std::memcmp(id, "TEA01", 5) != 0)
                break;  // unknown descriptor -> end of VRS
        }
        file.seek(0);
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

    if (data.size() >= 31 && std::memcmp(data.constData(), "FSF\0", 4) == 0 &&
        readBe16(data, 20) == 1 && readBe16(data, 22) >= 15 &&
        readBe16(data, 24) == 1 && quint8(data.at(30)) > 0) {
        return {ModuleFormat::SIEMENS_FSF, 0,
                QStringLiteral("Siemens FSF Windows CE flash-file archive"), {}};
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

    if (data.size() >= 4 && std::memcmp(data.constData(), "MSCF", 4) == 0) {
        return {ModuleFormat::CAB, 0, QStringLiteral("Microsoft Cabinet (CAB) archive"), {}};
    }

    if (data.size() >= 8 && std::memcmp(data.constData(), "MSWIM\0\0\0", 8) == 0) {
        return {ModuleFormat::WIM, 0, QStringLiteral("Windows Imaging (WIM) image"), {}};
    }

    // VMDK hosted sparse extent: "KDMV" magic at offset 0.
    if (data.size() >= 4 && std::memcmp(data.constData(), "KDMV", 4) == 0) {
        return {ModuleFormat::VMDK, 0, QStringLiteral("VMware Virtual Disk (VMDK)"), {}};
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
             (signature[2] == 0xFE && signature[3] == 0xFF) ||
             (signature[2] == 0xFD && signature[3] == 0xFF))) {
            return {ModuleFormat::OS2_PACK, 0, QStringLiteral("IBM/Microsoft OS/2 PACK/PACK2 archive"), {}};
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

    // NTFS boot sector: "NTFS    " OEM name at offset 3 plus the 0x55AA signature.
    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        std::memcmp(data.constData() + 3, "NTFS    ", 8) == 0) {
        return {ModuleFormat::NTFS, 0, QStringLiteral("Microsoft NTFS volume"), {}};
    }

    if (data.size() >= 512 &&
        std::memcmp(data.constData(), "regf", 4) == 0) {
        return {ModuleFormat::REGISTRY, 0, QStringLiteral("Windows Registry hive"), {}};
    }

    // exFAT boot sector: "EXFAT   " OEM name at offset 3 plus the 0x55AA signature.
    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        std::memcmp(data.constData() + 3, "EXFAT   ", 8) == 0) {
        return {ModuleFormat::EXFAT, 0, QStringLiteral("Microsoft exFAT volume"), {}};
    }

    // FAT boot sector: 0x55AA at 510 with a "FAT" label at 54 (FAT12/16) or 82
    // (FAT32) — distinguishes it from NTFS/exFAT/plain MBR.
    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        ((data.size() >= 57 && std::memcmp(data.constData() + 54, "FAT", 3) == 0) ||
         (data.size() >= 85 && std::memcmp(data.constData() + 82, "FAT", 3) == 0))) {
        return {ModuleFormat::FAT, 0, QStringLiteral("FAT volume"), {}};
    }

    if (isOs2EaList(data))
        return {ModuleFormat::OS2_EA, 0, QStringLiteral("OS/2 extended attribute list (FEALIST)"), {}};

    const ModuleFormatInfo direct = directOs2Executable(data);
    if (direct.format != ModuleFormat::Unknown) return direct;

    if (readLe16(data, 0) != 0x5A4D) {
        return {ModuleFormat::Unknown, 0, {}, QStringLiteral("Firma MZ, NE, LE, LX, XBEH, XEX1 o XEX2 assente")};
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
    if (data.size() >= 16 && std::memcmp(data.constData() + 4, "SignedImage ", 12) == 0)
        return {ModuleFormat::FFU, 0, QStringLiteral("Microsoft Full Flash Update (FFU)"), {}};

    if (data.size() >= 31 && std::memcmp(data.constData(), "FSF\0", 4) == 0 &&
        readBe16(data, 20) == 1 && readBe16(data, 22) >= 15 &&
        readBe16(data, 24) == 1 && quint8(data.at(30)) > 0)
        return {ModuleFormat::SIEMENS_FSF, 0,
                QStringLiteral("Siemens FSF Windows CE flash-file archive"), {}};

    if (isSiemensFwfHeader(data))
        return {ModuleFormat::SIEMENS_FWF, 0,
                QStringLiteral("Siemens FWF OMS firmware archive"), {}};

    const ModuleFormatInfo optical = detectOpticalBuffer(data);
    if (optical.format != ModuleFormat::Unknown)
        return optical;

    if (isWindowsCeRomHeader(data))
        return {ModuleFormat::WINCE_ROM, 0, QStringLiteral("Windows CE ROM / IMGFS"), {}};

    if (data.size() >= 8 && data.left(8) == QByteArray("SZDD\x88\xF0\x27\x33", 8))
        return {ModuleFormat::SZDD, 0, QStringLiteral("Microsoft Compress SZDD archive"), {}};

    if (isZipHeader(data))
        return {ModuleFormat::ZIP, 0, QStringLiteral("ZIP archive"), {}};

    if (isTarHeader(data))
        return {ModuleFormat::TAR, 0, QStringLiteral("TAR archive"), {}};

    if (data.size() >= 4 && std::memcmp(data.constData(), "MSCF", 4) == 0)
        return {ModuleFormat::CAB, 0, QStringLiteral("Microsoft Cabinet (CAB) archive"), {}};

    if (data.size() >= 8 && std::memcmp(data.constData(), "MSWIM\0\0\0", 8) == 0)
        return {ModuleFormat::WIM, 0, QStringLiteral("Windows Imaging (WIM) image"), {}};

    if (data.size() >= 4 && std::memcmp(data.constData(), "KDMV", 4) == 0)
        return {ModuleFormat::VMDK, 0, QStringLiteral("VMware Virtual Disk (VMDK)"), {}};

    if (data.size() >= 8 && std::memcmp(data.constData(), "conectix", 8) == 0)
        return {ModuleFormat::VHD, 0, QStringLiteral("Microsoft Virtual Hard Disk (VHD)"), {}};

    if (data.size() >= 72 && readLe32(data, 64) == 0xBEDA107FU)
        return {ModuleFormat::VDI, 0, QStringLiteral("VirtualBox Disk Image (VDI)"), {}};

    if (data.size() >= 8 && std::memcmp(data.constData(), "vhdxfile", 8) == 0)
        return {ModuleFormat::VHDX, 0, QStringLiteral("Microsoft Virtual Hard Disk v2 (VHDX)"), {}};

    if (data.size() >= 8 && std::memcmp(data.constData(), "$SDI0001", 8) == 0)
        return {ModuleFormat::SDI, 0, QStringLiteral("System Deployment Image (SDI)"), {}};

    if (data.size() >= 512 && std::memcmp(data.constData(), "ova.xml", 7) == 0)
        return {ModuleFormat::XVA, 0, QStringLiteral("Xen Virtual Appliance (XVA)"), {}};

    if (data.size() >= 4096 &&
        (std::memcmp(data.constData() + 4096 - 10, "SWAP-SPACE", 10) == 0 ||
         std::memcmp(data.constData() + 4096 - 10, "SWAPSPACE2", 10) == 0))
        return {ModuleFormat::SWAP, 0, QStringLiteral("Linux swap"), {}};

    if (data.size() >= 4 * 512) {
        for (int i = 0; i < 4; ++i) {
            const char* sector = data.constData() + i * 512;
            if (std::memcmp(sector, "LABELONE", 8) == 0 &&
                std::memcmp(sector + 0x18, "LVM2 001", 8) == 0)
                return {ModuleFormat::LVM, 0, QStringLiteral("Linux LVM2 physical volume"), {}};
        }
    }

    if (isOs2EaList(data))
        return {ModuleFormat::OS2_EA, 0, QStringLiteral("OS/2 extended attribute list (FEALIST)"), {}};

    const ModuleFormatInfo direct = directOs2Executable(data);
    if (direct.format != ModuleFormat::Unknown) return direct;

    const qsizetype earlyJfsSuperOffsets[] = {0x8000, 0xF000};
    for (qsizetype superOffset : earlyJfsSuperOffsets) {
        if (data.size() < superOffset + 24 ||
            std::memcmp(data.constData() + superOffset, "JFS1", 4) != 0)
            continue;
        const quint32 version = readLe32(data, superOffset + 4);
        const quint32 blockSize = readLe32(data, superOffset + 16);
        const quint16 blockShift = readLe16(data, superOffset + 20);
        if (!hasAuthoritativeOs2Layout(data, data.size()) &&
            (version == 1 || version == 2) && blockSize >= 512 && blockSize <= 4096 &&
            (blockSize & (blockSize - 1)) == 0 && blockShift >= 9 && blockShift <= 12 &&
            (quint32(1) << blockShift) == blockSize)
            return {ModuleFormat::JFS, 0, QStringLiteral("IBM Journaled File System (JFS) volume"), {}};
    }

    if (data.size() >= 512 &&
        (looksPartitionedRawDisk(data, data.size()) ||
         hasAuthoritativeOs2Layout(data, data.size()) ||
         hasRecoverableJfsPartition(data, data.size())))
        return {ModuleFormat::RAW_DISK, 0, QStringLiteral("Raw disk image"), {}};

    if (looksApplePartitionMap(data))
        return {ModuleFormat::RAW_DISK, 0, QStringLiteral("Raw Apple Partition Map disk image"), {}};

    if (looksLinuxRaidSuperblock(data, 0, 1) ||
        looksLinuxRaidSuperblock(data, 4096, 2))
        return {ModuleFormat::LINUX_RAID, 0, QStringLiteral("Linux MD RAID member"), {}};

    if (data.size() >= 0xc00 + 8 && std::memcmp(data.constData() + 0xc00, "PRIVHEAD", 8) == 0)
        return {ModuleFormat::DYNAMIC_DISK, 0, QStringLiteral("Windows Dynamic Disk (LDM)"), {}};

    if (data.size() >= 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
        if (p[0] == 0xA5 && p[1] == 0x96 &&
            ((p[2] == 0x0A && p[3] == 0x00) || (p[2] == 0x0A && p[3] == 0x0A) ||
             (p[2] == 0x00 && p[3] == 0x14) || (p[2] == 0x14 && p[3] == 0x0A) ||
             (p[2] == 0xFF && p[3] == 0xFF) || (p[2] == 0xFE && p[3] == 0xFF) ||
             (p[2] == 0xFD && p[3] == 0xFF)))
            return {ModuleFormat::OS2_PACK, 0, QStringLiteral("IBM/Microsoft OS/2 PACK/PACK2 archive"), {}};
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

    // ext2/3/4 superblock magic 0xEF53 at offset 0x438.
    if (data.size() >= 0x438 + 2 &&
        static_cast<unsigned char>(data[0x438]) == 0x53 &&
        static_cast<unsigned char>(data[0x439]) == 0xEF)
        return {ModuleFormat::EXT, 0, QStringLiteral("ext2/ext3/ext4 volume"), {}};

    if (data.size() >= 0x10040 + 8 &&
        std::memcmp(data.constData() + 0x10040, "_BHRfS_M", 8) == 0)
        return {ModuleFormat::BTRFS, 0, QStringLiteral("Btrfs volume"), {}};

    const qsizetype jfsSuperOffsets[] = {0x8000, 0xF000};
    for (qsizetype superOffset : jfsSuperOffsets) {
        if (data.size() < superOffset + 24 ||
            std::memcmp(data.constData() + superOffset, "JFS1", 4) != 0)
            continue;
        const quint32 version = readLe32(data, superOffset + 4);
        const quint32 blockSize = readLe32(data, superOffset + 16);
        const quint16 blockShift = readLe16(data, superOffset + 20);
        if ((version == 1 || version == 2) && blockSize >= 512 && blockSize <= 4096 &&
            (blockSize & (blockSize - 1)) == 0 && blockShift >= 9 && blockShift <= 12 &&
            (quint32(1) << blockShift) == blockSize)
            return {ModuleFormat::JFS, 0, QStringLiteral("IBM Journaled File System (JFS) volume"), {}};
    }

    if (data.size() >= 17 * 512 &&
        readLe32(data, 16 * 512) == 0xF995E849U &&
        readLe32(data, 16 * 512 + 4) == 0xFA53E9C5U &&
        readLe32(data, 16 * 512 + 12) >= 18 &&
        readLe32(data, 16 * 512 + 16) >= 20)
        return {ModuleFormat::HPFS, 0, QStringLiteral("IBM/Microsoft HPFS volume"), {}};

    if (data.size() >= 4 && std::memcmp(data.constData(), "XFSB", 4) == 0)
        return {ModuleFormat::XFS, 0, QStringLiteral("XFS volume"), {}};

    if (data.size() >= 4 && std::memcmp(data.constData(), "hsqs", 4) == 0)
        return {ModuleFormat::SQUASHFS, 0, QStringLiteral("SquashFS image"), {}};

    if (data.size() >= 512 && std::memcmp(data.constData() + data.size() - 512, "koly", 4) == 0)
        return {ModuleFormat::DMG, 0, QStringLiteral("Apple UDIF disk image (DMG)"), {}};

    if (data.size() >= 1026) {
        const quint16 hfsSig = (quint16(quint8(data[1024])) << 8) | quint16(quint8(data[1025]));
        if (hfsSig == 0x482b)
            return {ModuleFormat::HFSPLUS, 0, QStringLiteral("HFS+ volume"), {}};
        if (hfsSig == 0x4858)
            return {ModuleFormat::HFSPLUS, 0, QStringLiteral("HFSX volume"), {}};
    }

    // NTFS boot sector: "NTFS    " OEM name at offset 3 plus the 0x55AA signature.
    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        std::memcmp(data.constData() + 3, "NTFS    ", 8) == 0)
        return {ModuleFormat::NTFS, 0, QStringLiteral("Microsoft NTFS volume"), {}};

    // exFAT boot sector: "EXFAT   " OEM name at offset 3 plus the 0x55AA signature.
    if (data.size() >= 512 &&
        std::memcmp(data.constData(), "regf", 4) == 0)
        return {ModuleFormat::REGISTRY, 0, QStringLiteral("Windows Registry hive"), {}};

    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        std::memcmp(data.constData() + 3, "EXFAT   ", 8) == 0)
        return {ModuleFormat::EXFAT, 0, QStringLiteral("Microsoft exFAT volume"), {}};

    // FAT boot sector: 0x55AA signature at 510 and a "FAT" label at 54 (FAT12/16)
    // or 82 (FAT32). The label distinguishes it from NTFS/exFAT/MBR.
    if (data.size() >= 512 &&
        static_cast<unsigned char>(data[510]) == 0x55 &&
        static_cast<unsigned char>(data[511]) == 0xAA &&
        ((data.size() >= 57 && std::memcmp(data.constData() + 54, "FAT", 3) == 0) ||
         (data.size() >= 85 && std::memcmp(data.constData() + 82, "FAT", 3) == 0)))
        return {ModuleFormat::FAT, 0, QStringLiteral("FAT volume"), {}};

    return {ModuleFormat::Unknown, 0, {}, QStringLiteral("No recognised header")};
}

ModuleFormatInfo ModuleFormatDetector::detectNestedBuffer(const QByteArray& data)
{
    if (isOs2EaList(data))
        return {ModuleFormat::OS2_EA, 0, QStringLiteral("OS/2 extended attribute list (FEALIST)"), {}};
    const ModuleFormatInfo direct = directOs2Executable(data);
    if (direct.format != ModuleFormat::Unknown) return direct;
    // This function deliberately recognises only formats with deterministic
    // signatures in the supplied prefix. It is invoked for arbitrary PE/NE/LX
    // resources, so loose structural guesses would create false expand arrows.
    if (data.size() >= 16 && std::memcmp(data.constData() + 4, "SignedImage ", 12) == 0)
        return {ModuleFormat::FFU, 0, QStringLiteral("Microsoft Full Flash Update (FFU)"), {}};
    if (data.size() >= 31 && std::memcmp(data.constData(), "FSF\0", 4) == 0 &&
        readBe16(data, 20) == 1 && readBe16(data, 22) >= 15 &&
        readBe16(data, 24) == 1 && quint8(data.at(30)) > 0)
        return {ModuleFormat::SIEMENS_FSF, 0,
                QStringLiteral("Siemens FSF Windows CE flash-file archive"), {}};
    if (isSiemensFwfHeader(data))
        return {ModuleFormat::SIEMENS_FWF, 0,
                QStringLiteral("Siemens FWF OMS firmware archive"), {}};
    if (isB000ffHeader(data) ||
        (data.size() >= 6 && std::memcmp(data.constData(), "NOSAJ\0", 6) == 0) ||
        (data.size() >= 16 && std::memcmp(data.constData(), "ARNOLDBOOTBLOCK\0", 16) == 0) ||
        (data.size() >= 5 && std::memcmp(data.constData(), "iPAQ ", 5) == 0))
        return {ModuleFormat::WINCE_ROM, 0, QStringLiteral("Windows CE ROM / IMGFS"), {}};
    if (isZipHeader(data))
        return {ModuleFormat::ZIP, 0, QStringLiteral("ZIP archive"), {}};
    if (isTarHeader(data))
        return {ModuleFormat::TAR, 0, QStringLiteral("TAR archive"), {}};
    if (data.size() >= 4 && std::memcmp(data.constData(), "MSCF", 4) == 0)
        return {ModuleFormat::CAB, 0, QStringLiteral("Microsoft Cabinet (CAB) archive"), {}};
    if (data.size() >= 8 && data.left(8) == QByteArray("SZDD\x88\xF0\x27\x33", 8))
        return {ModuleFormat::SZDD, 0, QStringLiteral("Microsoft Compress SZDD archive"), {}};
    if (data.size() >= 8 && std::memcmp(data.constData(), "MSWIM\0\0\0", 8) == 0)
        return {ModuleFormat::WIM, 0, QStringLiteral("Windows Imaging (WIM) image"), {}};
    if (data.size() >= 4 && std::memcmp(data.constData(), "KDMV", 4) == 0)
        return {ModuleFormat::VMDK, 0, QStringLiteral("VMware Virtual Disk (VMDK)"), {}};
    if (data.size() >= 8 && std::memcmp(data.constData(), "vhdxfile", 8) == 0)
        return {ModuleFormat::VHDX, 0, QStringLiteral("Microsoft Virtual Hard Disk v2 (VHDX)"), {}};
    if (data.size() >= 8 && std::memcmp(data.constData(), "$SDI0001", 8) == 0)
        return {ModuleFormat::SDI, 0, QStringLiteral("System Deployment Image (SDI)"), {}};
    if (data.size() >= 4) {
        const QByteArray magic = data.left(4);
        if (magic == QByteArrayLiteral("XBEH"))
            return {ModuleFormat::XBE, 0, QStringLiteral("Original Xbox Executable (XBE)"), {}};
        if (magic == QByteArrayLiteral("XEX1") || magic == QByteArrayLiteral("XEX2"))
            return {ModuleFormat::XEX, 0, QStringLiteral("Xbox 360 Executable (XEX)"), {}};
        if (magic == QByteArrayLiteral("XUIZ"))
            return {ModuleFormat::XUIZ, 0, QStringLiteral("Xbox 360 XUIZ archive"), {}};
        if (magic == QByteArrayLiteral("LIVE") || magic == QByteArrayLiteral("PIRS"))
            return {ModuleFormat::LIVE_PIRS, 0, QStringLiteral("Xbox 360 STFS LIVE/PIRS container"), {}};
        if (magic == QByteArrayLiteral("CON "))
            return {ModuleFormat::CON, 0, QStringLiteral("Xbox 360 STFS CON container"), {}};
    }
    if (readLe16(data, 0) == 0x5A4D && data.size() >= 0x40) {
        const quint32 nh = readLe32(data, 0x3c);
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
    return {};
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
    case ModuleFormat::SIEMENS_FSF: return QStringLiteral("Siemens FSF");
    case ModuleFormat::ISO9660: return QStringLiteral("ISO 9660");
    case ModuleFormat::WIM: return QStringLiteral("WIM");
    case ModuleFormat::FAT: return QStringLiteral("FAT");
    case ModuleFormat::UDF: return QStringLiteral("UDF");
    case ModuleFormat::EXFAT: return QStringLiteral("exFAT");
    case ModuleFormat::VMDK: return QStringLiteral("VMDK");
    case ModuleFormat::VHD: return QStringLiteral("VHD");
    case ModuleFormat::VDI: return QStringLiteral("VDI");
    case ModuleFormat::VHDX: return QStringLiteral("VHDX");
    case ModuleFormat::SDI: return QStringLiteral("SDI");
    case ModuleFormat::XVA: return QStringLiteral("XVA");
    case ModuleFormat::SWAP: return QStringLiteral("Swap");
    case ModuleFormat::LVM: return QStringLiteral("LVM");
    case ModuleFormat::EXT: return QStringLiteral("ext");
    case ModuleFormat::NTFS: return QStringLiteral("NTFS");
    case ModuleFormat::XFS: return QStringLiteral("XFS");
    case ModuleFormat::JFS: return QStringLiteral("JFS");
    case ModuleFormat::HPFS: return QStringLiteral("HPFS");
    case ModuleFormat::SQUASHFS: return QStringLiteral("SquashFS");
    case ModuleFormat::HFSPLUS: return QStringLiteral("HFS+");
    case ModuleFormat::DMG: return QStringLiteral("DMG");
    case ModuleFormat::BTRFS: return QStringLiteral("Btrfs");
    case ModuleFormat::REGISTRY: return QStringLiteral("Registry");
    case ModuleFormat::BOOTCONFIG: return QStringLiteral("BCD");
    case ModuleFormat::CAB: return QStringLiteral("CAB");
    case ModuleFormat::RAW_DISK: return QStringLiteral("RAW");
    case ModuleFormat::ZIP: return QStringLiteral("ZIP");
    case ModuleFormat::TAR: return QStringLiteral("TAR");
    case ModuleFormat::LINUX_RAID: return QStringLiteral("Linux RAID");
    case ModuleFormat::DYNAMIC_DISK: return QStringLiteral("Dynamic Disk");
    case ModuleFormat::WINCE_ROM: return QStringLiteral("Windows CE ROM / IMGFS");
    case ModuleFormat::FFU: return QStringLiteral("FFU");
    case ModuleFormat::OS2_EA: return QStringLiteral("OS/2 EA");
    case ModuleFormat::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

} // namespace peare
