#include "DiskImageWriter.h"
#include "VhdxImageWriter.h"
#include "VhdImageWriter.h"
#include "VmdkImageWriter.h"
#include "VmdkStreamOptimizedWriter.h"
#include "VmdkSplitImageWriter.h"
#include "NtfsVolumeUpdate.h"
#include "ExtVolumeUpdate.h"

#include "third_party/fatfs/ff.h"
#include "third_party/fatfs/diskio.h"

#include <limits>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QLockFile>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryFile>
#include <QUuid>
#include <QtEndian>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <array>
#include <mutex>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace peare {
namespace writer {

struct DiskImageIoContext {
    QFile* file = nullptr;
    qint64 offset = 0;
    qint64 size = 0;
    VhdxImageWriter* vhdx = nullptr;
    VhdImageWriter* vhd = nullptr;
    VmdkImageAccess* vmdk = nullptr;
    bool writable = true;
};

}  // namespace writer
}  // namespace peare

namespace {

using peare::writer::VhdxImageWriter;
using peare::writer::VhdImageWriter;
using peare::writer::VmdkImageWriter;
using peare::writer::VmdkStreamOptimizedWriter;
using peare::writer::VmdkImageAccess;
using peare::writer::VmdkSplitImageWriter;
using peare::writer::ExtVolumeUpdate;

const qint64 kSectorSize = 512;
const qint64 kCopyChunkSize = 1024 * 1024;
const UINT kFatChunkSize = 64 * 1024;

QMutex g_fatMutex;
QMutex g_ntfsFormatMutex;
QFile* g_device = nullptr;
qint64 g_deviceSize = 0;
qint64 g_deviceBaseOffset = 0;
qint64 g_logicalSectorSize = kSectorSize;
VhdxImageWriter* g_vhdxImage = nullptr;
VhdImageWriter* g_vhdImage = nullptr;
VmdkImageAccess* g_vmdkImage = nullptr;
bool g_writeEnabled = false;
FATFS g_filesystem;

int ntfsImageRead(void* opaque, int64_t offset, void* buffer, int64_t length) {
    peare::writer::DiskImageIoContext* context =
        static_cast<peare::writer::DiskImageIoContext*>(opaque);
    if (!context || !buffer || offset < 0 || length < 0 || offset > context->size ||
        length > context->size - offset) return 0;
    const qint64 imageOffset = context->offset + qint64(offset);
    if (context->vhdx)
        return context->vhdx->read(imageOffset, static_cast<char*>(buffer), qint64(length));
    if (context->vhd)
        return context->vhd->read(imageOffset, static_cast<char*>(buffer), qint64(length));
    if (context->vmdk)
        return context->vmdk->read(imageOffset, static_cast<char*>(buffer), qint64(length));
    return context->file && context->file->seek(imageOffset) &&
        context->file->read(static_cast<char*>(buffer), qint64(length)) == qint64(length);
}

int ntfsImageWrite(void* opaque, int64_t offset, const void* buffer, int64_t length) {
    peare::writer::DiskImageIoContext* context =
        static_cast<peare::writer::DiskImageIoContext*>(opaque);
    if (!context || !context->writable || !buffer || offset < 0 || length < 0 || offset > context->size ||
        length > context->size - offset) return 0;
    const qint64 imageOffset = context->offset + qint64(offset);
    const char* data = static_cast<const char*>(buffer);
    if (context->vhdx) return context->vhdx->write(imageOffset, data, qint64(length));
    if (context->vhd) return context->vhd->write(imageOffset, data, qint64(length));
    if (context->vmdk) return context->vmdk->write(imageOffset, data, qint64(length));
    return context->file && context->file->seek(imageOffset) &&
        context->file->write(data, qint64(length)) == qint64(length);
}

int ntfsImageFlush(void* opaque) {
    peare::writer::DiskImageIoContext* context =
        static_cast<peare::writer::DiskImageIoContext*>(opaque);
    return context && context->file &&
        (!context->writable || context->file->flush());
}

bool extVolumeLength(const peare_ext_io& io, qint64* volumeLength) {
    std::array<uchar, 1024> superblock = {};
    if (!volumeLength || !io.read ||
        !io.read(io.context, 1024, superblock.data(), superblock.size()) ||
        qFromLittleEndian<quint16>(superblock.data() + 56) != 0xef53)
        return false;
    const quint32 logBlockSize = qFromLittleEndian<quint32>(
        superblock.data() + 24);
    if (logBlockSize > 6) return false;
    const quint64 blockSize = quint64(1024) << logBlockSize;
    quint64 blocks = qFromLittleEndian<quint32>(superblock.data() + 4);
    const quint32 incompat = qFromLittleEndian<quint32>(superblock.data() + 96);
    if (incompat & 0x80)
        blocks |= quint64(qFromLittleEndian<quint32>(
            superblock.data() + 336)) << 32;
    if (!blocks || blocks > quint64(std::numeric_limits<qint64>::max()) /
            blockSize)
        return false;
    const quint64 bytes = blocks * blockSize;
    if (bytes > quint64(io.size)) return false;
    *volumeLength = qint64(bytes);
    return true;
}

template <typename Image>
struct NtfsProbeContext {
    Image* image;
    qint64 offset;
    qint64 size;
};

template <typename Image>
int ntfsProbeRead(void* opaque, int64_t offset, void* buffer, int64_t length) {
    NtfsProbeContext<Image>* context = static_cast<NtfsProbeContext<Image>*>(opaque);
    return context && buffer && offset >= 0 && length >= 0 &&
        offset <= context->size && length <= context->size - offset &&
        context->image->read(context->offset + qint64(offset),
                             static_cast<char*>(buffer), qint64(length));
}

template <typename Image>
struct ExtProbeContext {
    Image* image;
    qint64 offset;
    qint64 size;
};

template <typename Image>
int extProbeRead(void* opaque, int64_t offset, void* buffer, int64_t length) {
    ExtProbeContext<Image>* context = static_cast<ExtProbeContext<Image>*>(opaque);
    return context && buffer && offset >= 0 && length >= 0 &&
        offset <= context->size && length <= context->size - offset &&
        context->image->read(context->offset + qint64(offset),
                             static_cast<char*>(buffer), qint64(length));
}

int extProbeWrite(void*, int64_t, const void*, int64_t) { return 0; }
int extProbeFlush(void*) { return 1; }

template <typename Image>
bool extImageMountable(Image* image, qint64 offset, qint64 size,
                       bool* ext3 = nullptr, bool* ext4 = nullptr) {
    ExtProbeContext<Image> context = {image, offset, size};
    peare_ext_io io = {&context, size, extProbeRead<Image>,
                        extProbeWrite, extProbeFlush};
    return ExtVolumeUpdate::probe(io, ext3, ext4);
}

int ntfsProbeWrite(void*, int64_t, const void*, int64_t) { return 0; }
int ntfsProbeFlush(void*) { return 1; }

template <typename Image>
bool ntfsImageMountable(Image* image, qint64 offset, qint64 size) {
    NtfsProbeContext<Image> context = {image, offset, size};
    peare_ntfs_io io = {&context, size, ntfsProbeRead<Image>,
                        ntfsProbeWrite, ntfsProbeFlush};
    return peare::writer::NtfsVolumeUpdate::probe(io);
}

bool writeAll(QIODevice* output, const char* data, qint64 length) {
    qint64 written = 0;
    while (written < length) {
        const qint64 count = output->write(data + written, length - written);
        if (count <= 0) return false;
        written += count;
    }
    return true;
}

bool writeAt(QFile* file, qint64 offset, const QByteArray& bytes) {
    return file && file->seek(offset) && writeAll(file, bytes.constData(), bytes.size());
}

quint32 crc32cBlock(QByteArray* bytes) {
    quint32 crc = 0xffffffffU;
    for (qint64 i = 0; i < bytes->size(); ++i) {
        crc ^= static_cast<unsigned char>(bytes->at(int(i)));
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
    }
    return ~crc;
}

void writeVhdChecksum(QByteArray* bytes, int checksumOffset) {
    quint32 sum = 0;
    for (int i = 0; i < bytes->size(); ++i)
        if (i < checksumOffset || i >= checksumOffset + 4)
            sum += static_cast<unsigned char>(bytes->at(i));
    qToBigEndian<quint32>(~sum, reinterpret_cast<uchar*>(bytes->data() + checksumOffset));
}

QByteArray makeVhdFooter(qint64 diskSize, bool dynamic) {
    QByteArray footer(512, '\0');
    std::memcpy(footer.data(), "conectix", 8);
    qToBigEndian<quint32>(2, reinterpret_cast<uchar*>(footer.data() + 8));
    qToBigEndian<quint32>(0x00010000U, reinterpret_cast<uchar*>(footer.data() + 12));
    qToBigEndian<quint64>(dynamic ? 512 : ~quint64(0),
                           reinterpret_cast<uchar*>(footer.data() + 16));
    qToBigEndian<quint64>(quint64(diskSize), reinterpret_cast<uchar*>(footer.data() + 40));
    qToBigEndian<quint64>(quint64(diskSize), reinterpret_cast<uchar*>(footer.data() + 48));
    qToBigEndian<quint32>(dynamic ? 3U : 2U,
                           reinterpret_cast<uchar*>(footer.data() + 60));
    const QByteArray id = QUuid::createUuid().toRfc4122();
    std::memcpy(footer.data() + 68, id.constData(), 16);
    writeVhdChecksum(&footer, 64);
    return footer;
}

bool createVhd(QFile* file, qint64 diskSize, bool fixed, QString* error) {
    const QByteArray footer = makeVhdFooter(diskSize, !fixed);
    if (fixed) {
        if (!file->resize(diskSize + 512) || !writeAt(file, diskSize, footer)) {
            if (error) *error = file->errorString();
            return false;
        }
        return true;
    }
    const qint64 blockSize = 2 * 1024 * 1024;
    const qint64 blockCount = (diskSize + blockSize - 1) / blockSize;
    const qint64 tableOffset = 3 * 512;
    const qint64 tableBytes = ((blockCount * 4 + 511) / 512) * 512;
    const qint64 footerOffset = tableOffset + tableBytes;
    QByteArray header(1024, '\0');
    std::memcpy(header.data(), "cxsparse", 8);
    qToBigEndian<quint64>(~quint64(0), reinterpret_cast<uchar*>(header.data() + 8));
    qToBigEndian<quint64>(quint64(tableOffset), reinterpret_cast<uchar*>(header.data() + 16));
    qToBigEndian<quint32>(0x00010000U, reinterpret_cast<uchar*>(header.data() + 24));
    qToBigEndian<quint32>(quint32(blockCount), reinterpret_cast<uchar*>(header.data() + 28));
    qToBigEndian<quint32>(quint32(blockSize), reinterpret_cast<uchar*>(header.data() + 32));
    writeVhdChecksum(&header, 36);
    QByteArray table(int(tableBytes), '\0');
    for (qint64 i = 0; i < blockCount; ++i)
        qToBigEndian<quint32>(0xffffffffU,
                              reinterpret_cast<uchar*>(table.data() + i * 4));
    if (!file->resize(footerOffset + 512) || !writeAt(file, 0, footer) ||
        !writeAt(file, 512, header) || !writeAt(file, tableOffset, table) ||
        !writeAt(file, footerOffset, footer)) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}

bool createVhdx(QFile* file, qint64 diskSize, bool fixed,
                quint32 sectorSize, QString* error) {
    const qint64 mib = 1024 * 1024;
    const qint64 blockSize = mib;
    const qint64 dataBlocks = diskSize / blockSize;
    const qint64 logOffset = mib;
    const qint64 metadataOffset = 2 * mib;
    const qint64 batOffset = 3 * mib;
    const qint64 dataOffset = 4 * mib;
    QByteArray header1(4096, '\0'), header2(4096, '\0');
    const auto writeGuid = [](char* destination, const QUuid& guid) {
        qToLittleEndian<quint32>(guid.data1, reinterpret_cast<uchar*>(destination));
        qToLittleEndian<quint16>(guid.data2, reinterpret_cast<uchar*>(destination + 4));
        qToLittleEndian<quint16>(guid.data3, reinterpret_cast<uchar*>(destination + 6));
        std::memcpy(destination + 8, guid.data4, 8);
    };
    const QUuid fileWriteGuid = QUuid::createUuid();
    const QUuid dataWriteGuid = QUuid::createUuid();
    for (QByteArray* header : {&header1, &header2}) {
        std::memcpy(header->data(), "head", 4);
        qToLittleEndian<quint64>(header == &header1 ? 1 : 2,
                                 reinterpret_cast<uchar*>(header->data() + 8));
        writeGuid(header->data() + 16, fileWriteGuid);
        writeGuid(header->data() + 32, dataWriteGuid);
        qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(header->data() + 66));
        qToLittleEndian<quint32>(quint32(mib),
                                 reinterpret_cast<uchar*>(header->data() + 68));
        qToLittleEndian<quint64>(quint64(logOffset),
                                 reinterpret_cast<uchar*>(header->data() + 72));
        qToLittleEndian<quint32>(crc32cBlock(header),
                                 reinterpret_cast<uchar*>(header->data() + 4));
    }
    const QByteArray batGuid = QByteArray::fromHex("6677c22d23f600429d64115e9bfd4a08");
    const QByteArray metadataGuid = QByteArray::fromHex("06a27c8b90479a4bb8fe575f050f886e");
    auto makeRegionTable = [&]() {
        QByteArray table(64 * 1024, '\0');
        std::memcpy(table.data(), "regi", 4);
        qToLittleEndian<quint32>(2, reinterpret_cast<uchar*>(table.data() + 8));
        std::memcpy(table.data() + 16, metadataGuid.constData(), 16);
        qToLittleEndian<quint64>(quint64(metadataOffset), reinterpret_cast<uchar*>(table.data() + 32));
        qToLittleEndian<quint32>(quint32(mib), reinterpret_cast<uchar*>(table.data() + 40));
        qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(table.data() + 44));
        std::memcpy(table.data() + 48, batGuid.constData(), 16);
        qToLittleEndian<quint64>(quint64(batOffset), reinterpret_cast<uchar*>(table.data() + 64));
        qToLittleEndian<quint32>(quint32(mib), reinterpret_cast<uchar*>(table.data() + 72));
        qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(table.data() + 76));
        qToLittleEndian<quint32>(crc32cBlock(&table),
                                 reinterpret_cast<uchar*>(table.data() + 4));
        return table;
    };
    const QByteArray region = makeRegionTable();
    QByteArray metadata(mib, '\0');
    std::memcpy(metadata.data(), "metadata", 8);
    qToLittleEndian<quint16>(5, reinterpret_cast<uchar*>(metadata.data() + 10));
    const char* guids[] = {
        "3767a1ca36fa434db3b633f0aa44e76b", "2442a52f1bcd7648b2115dbed83bf4b8",
        "ab12cabe e6b2 2345 93ef c309e000c746", "1dbf41816fa90947ba47f233a8faab5f",
        "c748a3cd5d4471449cc9e9885251c556"
    };
    QByteArray cleanGuids[5];
    for (int i = 0; i < 5; ++i) {
        QByteArray hex(guids[i]);
        hex.replace(" ", "");
        cleanGuids[i] = QByteArray::fromHex(hex);
    }
    const QUuid diskId = QUuid::createUuid();
    const quint32 itemOffsets[] = {65536, 65544, 65552, 65568, 65572};
    const quint32 itemLengths[] = {8, 8, 16, 4, 4};
    const quint32 itemFlags[] = {4, 6, 6, 6, 6};
    for (int i = 0; i < 5; ++i) {
        char* entry = metadata.data() + 32 + i * 32;
        std::memcpy(entry, cleanGuids[i].constData(), 16);
        qToLittleEndian<quint32>(itemOffsets[i], reinterpret_cast<uchar*>(entry + 16));
        qToLittleEndian<quint32>(itemLengths[i], reinterpret_cast<uchar*>(entry + 20));
        qToLittleEndian<quint32>(itemFlags[i], reinterpret_cast<uchar*>(entry + 24));
    }
    qToLittleEndian<quint32>(quint32(blockSize), reinterpret_cast<uchar*>(metadata.data() + 65536));
    qToLittleEndian<quint32>(fixed ? 1U : 0U, reinterpret_cast<uchar*>(metadata.data() + 65540));
    qToLittleEndian<quint64>(quint64(diskSize), reinterpret_cast<uchar*>(metadata.data() + 65544));
    writeGuid(metadata.data() + 65552, diskId);
    qToLittleEndian<quint32>(sectorSize, reinterpret_cast<uchar*>(metadata.data() + 65568));
    qToLittleEndian<quint32>(sectorSize, reinterpret_cast<uchar*>(metadata.data() + 65572));
    QByteArray bat(int(mib), '\0');
    const qint64 initialSize = fixed ? dataOffset + diskSize : dataOffset;
    if (!fixed) {
        if (!file->resize(initialSize)) {
            if (error) *error = file->errorString();
            return false;
        }
    } else {
        for (qint64 i = 0; i < dataBlocks; ++i) {
            const quint64 entry = (quint64(dataOffset / mib + i) << 20) | 6U;
            qToLittleEndian<quint64>(entry, reinterpret_cast<uchar*>(bat.data() + i * 8));
        }
        if (!file->resize(initialSize)) {
            if (error) *error = file->errorString();
            return false;
        }
    }
    if (!writeAt(file, 0, QByteArrayLiteral("vhdxfile")) ||
        !writeAt(file, 64 * 1024, header1) || !writeAt(file, 128 * 1024, header2) ||
        !writeAt(file, 192 * 1024, region) || !writeAt(file, 256 * 1024, region) ||
        !writeAt(file, batOffset, bat) || !writeAt(file, metadataOffset, metadata)) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}

bool createMonolithicSparseVmdk(QFile* file, qint64 diskSize,
                                const QString& outputName, QString* error) {
    const qint64 sector = 512, grainSectors = 128, entriesPerTable = 512;
    const qint64 capacitySectors = diskSize / sector;
    const qint64 grains = (capacitySectors + grainSectors - 1) / grainSectors;
    const qint64 tableCount = (grains + entriesPerTable - 1) / entriesPerTable;
    const qint64 tableSectors = 4;
    const qint64 descriptorSector = 1, descriptorSectors = 20;
    const qint64 redundantDirectorySector = descriptorSector + descriptorSectors;
    const qint64 redundantTablesSector = redundantDirectorySector + 1;
    const qint64 directorySector = redundantTablesSector + tableCount * tableSectors;
    const qint64 mainTablesSector = directorySector + 1;
    const qint64 overheadSectors =
        ((mainTablesSector + tableCount * tableSectors + grainSectors - 1) /
         grainSectors) * grainSectors;
    const qint64 cylinders = qMax<qint64>(1, capacitySectors / (16 * 63));
    const QByteArray descriptor = QStringLiteral(
        "# Disk DescriptorFile\nversion=1\nencoding=\"UTF-8\"\n"
        "CID=fffffffe\nparentCID=ffffffff\ncreateType=\"monolithicSparse\"\n\n"
        "# Extent description\nRW %1 SPARSE \"%2\"\n\n# The Disk Data Base\n#DDB\n"
        "ddb.adapterType = \"ide\"\n"
        "ddb.geometry.cylinders = \"%3\"\n"
        "ddb.geometry.heads = \"16\"\n"
        "ddb.geometry.sectors = \"63\"\n"
        "ddb.longContentID = \"7aa82af186b5e4cf3e378aa6fffffffe\"\n"
        "ddb.uuid = \"60 00 C2 91 17 c1 1e 82-44 df f8 63 a3 53 a1 f9\"\n"
        "ddb.virtualHWVersion = \"13\"\n")
        .arg(QString::number(capacitySectors), outputName,
             QString::number(cylinders)).toUtf8();
    QByteArray header(512, '\0');
    std::memcpy(header.data(), "KDMV", 4);
    qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(header.data() + 4));
    qToLittleEndian<quint32>(3, reinterpret_cast<uchar*>(header.data() + 8));
    qToLittleEndian<quint64>(quint64(capacitySectors), reinterpret_cast<uchar*>(header.data() + 12));
    qToLittleEndian<quint64>(quint64(grainSectors), reinterpret_cast<uchar*>(header.data() + 20));
    qToLittleEndian<quint64>(quint64(descriptorSector), reinterpret_cast<uchar*>(header.data() + 28));
    qToLittleEndian<quint64>(quint64(descriptorSectors), reinterpret_cast<uchar*>(header.data() + 36));
    qToLittleEndian<quint32>(quint32(entriesPerTable), reinterpret_cast<uchar*>(header.data() + 44));
    qToLittleEndian<quint64>(quint64(redundantDirectorySector), reinterpret_cast<uchar*>(header.data() + 48));
    qToLittleEndian<quint64>(quint64(directorySector), reinterpret_cast<uchar*>(header.data() + 56));
    qToLittleEndian<quint64>(quint64(overheadSectors), reinterpret_cast<uchar*>(header.data() + 64));
    header[72] = 0;
    std::memcpy(header.data() + 73, "\n \r\n", 4);
    QByteArray directory(512, '\0');
    QByteArray table(int(tableSectors * sector), '\0');
    for (qint64 i = 0; i < tableCount; ++i) {
        qToLittleEndian<quint32>(quint32(redundantTablesSector + i * tableSectors),
            reinterpret_cast<uchar*>(directory.data() + i * 4));
    }
    QByteArray mainDirectory(512, '\0');
    for (qint64 i = 0; i < tableCount; ++i)
        qToLittleEndian<quint32>(quint32(mainTablesSector + i * tableSectors),
            reinterpret_cast<uchar*>(mainDirectory.data() + i * 4));
    QByteArray paddedDescriptor(int(descriptorSectors * sector), '\0');
    std::memcpy(paddedDescriptor.data(), descriptor.constData(),
                size_t(qMin<qint64>(descriptor.size(), paddedDescriptor.size())));
    if (!file->resize(overheadSectors * sector) || !writeAt(file, 0, header) ||
        !writeAt(file, descriptorSector * sector, paddedDescriptor) ||
        !writeAt(file, redundantDirectorySector * sector, directory) ||
        !writeAt(file, directorySector * sector, mainDirectory)) {
        if (error) *error = file->errorString();
        return false;
    }
    for (qint64 i = 0; i < tableCount; ++i) {
        if (!writeAt(file, (redundantTablesSector + i * tableSectors) * sector, table) ||
            !writeAt(file, (mainTablesSector + i * tableSectors) * sector, table)) {
            if (error) *error = file->errorString();
            return false;
        }
    }
    return true;
}

bool initializeExternalSparseVmdk(QFile* file, qint64 capacityBytes, QString* error) {
    const qint64 capacitySectors = capacityBytes / kSectorSize;
    if (!file || capacityBytes <= 0 || capacityBytes % kSectorSize ||
        capacitySectors > 4 * 1024 * 1024) {
        if (error) *error = QStringLiteral("The split sparse VMDK extent size is invalid.");
        return false;
    }
    QByteArray header(int(kSectorSize), '\0');
    std::memcpy(header.data(), "KDMV", 4);
    qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(header.data() + 4));
    qToLittleEndian<quint32>(3, reinterpret_cast<uchar*>(header.data() + 8));
    qToLittleEndian<quint64>(quint64(capacitySectors), reinterpret_cast<uchar*>(header.data() + 12));
    qToLittleEndian<quint64>(128, reinterpret_cast<uchar*>(header.data() + 20));
    qToLittleEndian<quint32>(512, reinterpret_cast<uchar*>(header.data() + 44));
    qToLittleEndian<quint64>(1, reinterpret_cast<uchar*>(header.data() + 48));
    qToLittleEndian<quint64>(2, reinterpret_cast<uchar*>(header.data() + 56));
    qToLittleEndian<quint64>(3, reinterpret_cast<uchar*>(header.data() + 64));
    std::memcpy(header.data() + 73, "\n \r\n", 4);
    if (!file->resize(3 * kSectorSize) || !writeAt(file, 0, header)) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}

QByteArray makeSplitVmdkDescriptor(qint64 capacityBytes, const QString& extentName) {
    const qint64 sectors = capacityBytes / kSectorSize;
    const qint64 cylinders = qMax<qint64>(1, sectors / (16 * 63));
    const QString cid = QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString diskUuid = QUuid::createUuid().toString(QUuid::Id128).toUpper();
    return QStringLiteral(
        "# Disk DescriptorFile\nversion=1\nencoding=\"UTF-8\"\n"
        "CID=%1\nparentCID=ffffffff\ncreateType=\"twoGbMaxExtentSparse\"\n\n"
        "# Extent description\nRW %2 SPARSE \"%3\"\n\n# The Disk Data Base\n#DDB\n"
        "ddb.adapterType = \"ide\"\n"
        "ddb.geometry.cylinders = \"%4\"\n"
        "ddb.geometry.heads = \"16\"\n"
        "ddb.geometry.sectors = \"63\"\n"
        "ddb.uuid = \"%5\"\n"
        "ddb.virtualHWVersion = \"13\"\n")
        .arg(cid, QString::number(sectors), extentName,
             QString::number(cylinders), diskUuid).toUtf8();
}

quint32 crc32Ieee(const char* data, qint64 length);

struct FloppyCreateGeometry {
    const char* id;
    qint64 capacity;
    quint16 rootEntries;
    quint16 sectorsPerTrack;
    quint16 heads;
};

bool floppyCreateGeometry(const QString& id, FloppyCreateGeometry* geometry) {
    static const FloppyCreateGeometry geometries[] = {
        {"160k", 163840, 64, 8, 1}, {"180k", 184320, 64, 9, 1},
        {"320k", 327680, 112, 8, 2}, {"360k", 368640, 112, 9, 2},
        {"400k", 409600, 112, 10, 2}, {"640k", 655360, 112, 8, 2},
        {"720k", 737280, 112, 9, 2}, {"800k", 819200, 112, 10, 2},
        {"1.2m", 1228800, 224, 15, 2}, {"1.44m", 1474560, 224, 18, 2},
        {"1.68m", 1720320, 224, 21, 2}, {"2.88m", 2949120, 240, 36, 2},
        {"xdf-1.84m", 1884160, 224, 23, 2}
    };
    for (const FloppyCreateGeometry& candidate : geometries) {
        if (id == QLatin1String(candidate.id)) {
            *geometry = candidate;
            return true;
        }
    }
    return false;
}

QByteArray xdfCompatibilityImage() {
    QByteArray image(11 * kSectorSize, '\0');
    char* boot = image.data();
    boot[0] = char(0xeb);
    boot[1] = char(0x4f);
    boot[2] = char(0x90);
    std::memcpy(boot + 3, "XDFv1.3i", 8);
    qToLittleEndian<quint16>(512, reinterpret_cast<uchar*>(boot + 11));
    boot[13] = 1;
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(boot + 14));
    boot[16] = 2;
    qToLittleEndian<quint16>(16, reinterpret_cast<uchar*>(boot + 17));
    qToLittleEndian<quint16>(8, reinterpret_cast<uchar*>(boot + 19));
    boot[21] = char(0xf9);
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(boot + 22));
    qToLittleEndian<quint16>(8, reinterpret_cast<uchar*>(boot + 24));
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(boot + 26));
    boot[510] = char(0x55);
    boot[511] = char(0xaa);

    const QByteArray fat = QByteArray::fromHex("f9ffffff0f");
    std::memcpy(image.data() + kSectorSize, fat.constData(), size_t(fat.size()));
    std::memcpy(image.data() + 2 * kSectorSize, fat.constData(), size_t(fat.size()));

    char* root = image.data() + 3 * kSectorSize;
    std::memcpy(root, "PEARE   TXT", 11);
    root[11] = char(0x20);
    qToLittleEndian<quint16>(2, reinterpret_cast<uchar*>(root + 26));
    const QByteArray readme = QByteArrayLiteral("Peare XDF compatibility image.\r\n");
    qToLittleEndian<quint32>(quint32(readme.size()), reinterpret_cast<uchar*>(root + 28));
    std::memcpy(image.data() + 4 * kSectorSize, readme.constData(), size_t(readme.size()));
    return image;
}

bool createRawPartitionedImage(QFile* file, const QString& layout,
                               const QString& filesystem, qint64 volumeSize,
                               qint64* diskSize,
                               qint64* volumeOffset, QString* error) {
    const qint64 sectorSize = 512;
    const qint64 firstPartitionLba = 2048;
    if (!file || !diskSize || !volumeOffset || volumeSize <= 0 ||
        volumeSize % sectorSize || (layout != QStringLiteral("mbr") &&
                                    layout != QStringLiteral("gpt"))) return false;
    const quint64 volumeSectors = quint64(volumeSize / sectorSize);
    const quint64 partitionEnd = quint64(firstPartitionLba) + volumeSectors - 1;
    const quint64 diskSectors = layout == QStringLiteral("gpt")
        ? partitionEnd + 34 : partitionEnd + 1;
    if (diskSectors > std::numeric_limits<quint32>::max() ||
        diskSectors > quint64(std::numeric_limits<qint64>::max() / sectorSize)) {
        if (error) *error = QStringLiteral("The requested raw disk is too large.");
        return false;
    }
    *diskSize = qint64(diskSectors * sectorSize);
    *volumeOffset = firstPartitionLba * sectorSize;
    if (!file->resize(*diskSize)) {
        if (error) *error = file->errorString();
        return false;
    }

    QByteArray mbr(512, '\0');
    char* entry = mbr.data() + 446;
    const bool extFilesystem = filesystem == QStringLiteral("ext2") ||
        filesystem == QStringLiteral("ext3") ||
        filesystem == QStringLiteral("ext4");
    entry[0] = extFilesystem ? char(0x00) : char(0x80);
    entry[1] = char(0xfe);
    entry[2] = char(0xff);
    entry[3] = char(0xff);
    entry[4] = layout == QStringLiteral("gpt") ? char(0xee) :
        filesystem == QStringLiteral("fat12") ? char(0x01) :
        filesystem == QStringLiteral("fat16") ? char(0x06) :
        extFilesystem ? char(0x83) :
        (filesystem == QStringLiteral("exfat") ||
         filesystem == QStringLiteral("ntfs")) ? char(0x07) : char(0x0c);
    entry[5] = char(0xfe);
    entry[6] = char(0xff);
    entry[7] = char(0xff);
    qToLittleEndian<quint32>(layout == QStringLiteral("gpt") ? 1U : firstPartitionLba,
                             reinterpret_cast<uchar*>(entry + 8));
    const quint32 partitionSectors = layout == QStringLiteral("gpt")
        ? quint32(diskSectors - 1) : quint32(volumeSectors);
    qToLittleEndian<quint32>(partitionSectors,
                             reinterpret_cast<uchar*>(entry + 12));
    mbr[510] = char(0x55);
    mbr[511] = char(0xaa);
    if (!writeAt(file, 0, mbr)) {
        if (error) *error = file->errorString();
        return false;
    }
    if (layout == QStringLiteral("mbr")) return true;

    const quint64 finalLba = diskSectors - 1;
    const quint64 backupEntriesLba = finalLba - 32;
    QByteArray entries(128 * 128, '\0');
    static const char basicDataGuid[16] = {
        char(0xeb), char(0xd0), char(0xa0), char(0xa2), char(0xb9), char(0xe5),
        char(0x33), char(0x44), char(0x87), char(0xc0), char(0x68), char(0xb6),
        char(0xb7), char(0x26), char(0x99), char(0xc7)
    };
    static const char linuxFilesystemGuid[16] = {
        char(0xaf), char(0x63), char(0xc6), char(0x0f), char(0x83), char(0x47),
        char(0x72), char(0x48), char(0x8e), char(0x79), char(0x3d), char(0x69),
        char(0xd8), char(0x47), char(0x7d), char(0x7e)
    };
    std::memcpy(entries.data(), extFilesystem
        ? linuxFilesystemGuid : basicDataGuid, sizeof(basicDataGuid));
    const QByteArray partitionGuid = QUuid::createUuid().toRfc4122();
    std::memcpy(entries.data() + 16, partitionGuid.constData(), 16);
    qToLittleEndian<quint64>(quint64(firstPartitionLba),
                             reinterpret_cast<uchar*>(entries.data() + 32));
    qToLittleEndian<quint64>(partitionEnd,
                             reinterpret_cast<uchar*>(entries.data() + 40));
    const quint32 entriesCrc = crc32Ieee(entries.constData(), entries.size());
    const QByteArray diskGuid = QUuid::createUuid().toRfc4122();

    auto makeHeader = [&](quint64 currentLba, quint64 backupLba, quint64 entriesLba) {
        QByteArray header(512, '\0');
        std::memcpy(header.data(), "EFI PART", 8);
        qToLittleEndian<quint32>(0x00010000U,
                                 reinterpret_cast<uchar*>(header.data() + 8));
        qToLittleEndian<quint32>(92, reinterpret_cast<uchar*>(header.data() + 12));
        qToLittleEndian<quint64>(currentLba,
                                 reinterpret_cast<uchar*>(header.data() + 24));
        qToLittleEndian<quint64>(backupLba,
                                 reinterpret_cast<uchar*>(header.data() + 32));
        qToLittleEndian<quint64>(34,
                                 reinterpret_cast<uchar*>(header.data() + 40));
        qToLittleEndian<quint64>(partitionEnd,
                                 reinterpret_cast<uchar*>(header.data() + 48));
        std::memcpy(header.data() + 56, diskGuid.constData(), 16);
        qToLittleEndian<quint64>(entriesLba,
                                 reinterpret_cast<uchar*>(header.data() + 72));
        qToLittleEndian<quint32>(128,
                                 reinterpret_cast<uchar*>(header.data() + 80));
        qToLittleEndian<quint32>(128,
                                 reinterpret_cast<uchar*>(header.data() + 84));
        qToLittleEndian<quint32>(entriesCrc,
                                 reinterpret_cast<uchar*>(header.data() + 88));
        qToLittleEndian<quint32>(crc32Ieee(header.constData(), 92),
                                 reinterpret_cast<uchar*>(header.data() + 16));
        return header;
    };
    const QByteArray primaryHeader = makeHeader(1, finalLba, 2);
    const QByteArray backupHeader = makeHeader(finalLba, 1, backupEntriesLba);
    if (!writeAt(file, 2 * sectorSize, entries) ||
        !writeAt(file, qint64(backupEntriesLba * sectorSize), entries) ||
        !writeAt(file, sectorSize, primaryHeader) ||
        !writeAt(file, qint64(finalLba * sectorSize), backupHeader)) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}

bool syncFileToDisk(QFileDevice* file, QString* error) {
    if (!file || !file->flush()) {
        if (error) *error = file ? file->errorString()
                                 : QStringLiteral("No staged image is open for synchronization.");
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(int(file->handle()));
    if (nativeHandle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle))) {
        if (error) *error = QStringLiteral("Flushing the staged image to storage failed (Windows error %1).")
            .arg(GetLastError());
        return false;
    }
#else
    if (::fsync(int(file->handle())) != 0) {
        if (error) *error = QStringLiteral("Flushing the staged image to storage failed: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#endif
    return true;
}

bool logicalImageSize(QFile* image, qint64* logicalSize, bool* fixedVhd) {
    if (!image || !logicalSize || !fixedVhd || image->size() < kSectorSize) return false;
    *logicalSize = image->size();
    *fixedVhd = false;
    if (!image->seek(image->size() - kSectorSize)) return false;
    const QByteArray footer = image->read(kSectorSize);
    if (footer.size() != kSectorSize || std::memcmp(footer.constData(), "conectix", 8) != 0)
        return true;

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(footer.constData());
    const quint32 version = qFromBigEndian<quint32>(bytes + 12);
    const quint64 dataOffset = qFromBigEndian<quint64>(bytes + 16);
    const quint64 currentSize = qFromBigEndian<quint64>(bytes + 48);
    const quint32 diskType = qFromBigEndian<quint32>(bytes + 60);
    const quint32 storedChecksum = qFromBigEndian<quint32>(bytes + 64);
    quint32 sum = 0;
    for (int i = 0; i < footer.size(); ++i)
        if (i < 64 || i >= 68) sum += bytes[i];
    if (version != 0x00010000U || dataOffset != ~quint64(0) || diskType != 2 ||
        storedChecksum != ~sum || currentSize == 0 || currentSize > quint64(image->size()) ||
        currentSize != quint64(image->size() - kSectorSize) || currentSize % kSectorSize)
        return false;
    *logicalSize = qint64(currentSize);
    *fixedVhd = true;
    return true;
}

bool hasVhdxSignature(QFile* image) {
    char signature[8];
    return image && image->seek(0) && image->read(signature, sizeof(signature)) == sizeof(signature) &&
           std::memcmp(signature, "vhdxfile", sizeof(signature)) == 0;
}

bool hasVhdSignature(QFile* image) {
    char signature[8];
    return image && image->seek(0) && image->read(signature, sizeof(signature)) == sizeof(signature) &&
           std::memcmp(signature, "conectix", sizeof(signature)) == 0;
}

bool hasVmdkSignature(QFile* image) {
    char signature[4];
    return image && image->seek(0) && image->read(signature, sizeof(signature)) == sizeof(signature) &&
           std::memcmp(signature, "KDMV", sizeof(signature)) == 0;
}

bool isStreamOptimizedVmdk(QFile* image) {
    char header[80];
    const bool matches = image && image->seek(0) && image->read(header, sizeof(header)) == sizeof(header) &&
        qFromLittleEndian<quint32>(header) == 0x564d444bU &&
        qFromLittleEndian<quint32>(header + 4) == 3 &&
        qFromLittleEndian<quint16>(header + 77) == 1;
    if (image) image->seek(0);
    return matches;
}

bool hasVmdkDescriptor(QFile* image) {
    if (!image || !image->seek(0)) return false;
    const QByteArray header = image->read(1024 * 1024);
    return header.contains("# Disk DescriptorFile") &&
        (header.contains("createType=\"twoGbMaxExtentSparse\"") ||
         header.contains("createType=\"twoGbMaxExtentFlat\"") ||
         header.contains("createType=\"monolithicFlat\""));
}

quint32 crc32Ieee(const char* data, qint64 length) {
    quint32 crc = 0xffffffffU;
    for (qint64 i = 0; i < length; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

bool fatBootPlausible(const char* sector, qint64 volumeBytes, qint64 logicalSectorSize) {
    if (!sector || volumeBytes < logicalSectorSize ||
        static_cast<unsigned char>(sector[510]) != 0x55 ||
        static_cast<unsigned char>(sector[511]) != 0xaa) return false;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(sector);
    const quint16 bps = qFromLittleEndian<quint16>(bytes + 11);
    const unsigned char spc = bytes[13];
    const quint16 reserved = qFromLittleEndian<quint16>(bytes + 14);
    const unsigned char fats = bytes[16];
    const quint16 rootEntries = qFromLittleEndian<quint16>(bytes + 17);
    const quint16 total16 = qFromLittleEndian<quint16>(bytes + 19);
    const quint16 fat16 = qFromLittleEndian<quint16>(bytes + 22);
    const quint32 total32 = qFromLittleEndian<quint32>(bytes + 32);
    const quint32 fat32 = qFromLittleEndian<quint32>(bytes + 36);
    if (bps != logicalSectorSize) return false;
    const quint64 total = total16 ? total16 : total32;
    const quint64 fatSize = fat16 ? fat16 : fat32;
    const quint64 rootSectors = (quint64(rootEntries) * 32 + bps - 1) / bps;
    const quint64 overhead = reserved + quint64(fats) * fatSize + rootSectors;
    return spc && !(spc & (spc - 1)) && spc <= 128 && reserved &&
           (fats == 1 || fats == 2) && total && fatSize && overhead < total &&
           total <= quint64(volumeBytes / bps);
}

bool exfatBootRegionValid(const char* region, qint64 volumeBytes,
                          qint64 logicalSectorSize) {
    if (!region || (logicalSectorSize != 512 && logicalSectorSize != 4096) ||
        std::memcmp(region + 3, "EXFAT   ", 8) != 0 ||
        static_cast<unsigned char>(region[510]) != 0x55 ||
        static_cast<unsigned char>(region[511]) != 0xaa)
        return false;
    const unsigned char* boot = reinterpret_cast<const unsigned char*>(region);
    for (int i = 11; i < 64; ++i)
        if (boot[i]) return false;
    const quint64 volumeSectors = qFromLittleEndian<quint64>(boot + 72);
    const quint32 fatOffset = qFromLittleEndian<quint32>(boot + 80);
    const quint32 fatLength = qFromLittleEndian<quint32>(boot + 84);
    const quint32 dataOffset = qFromLittleEndian<quint32>(boot + 88);
    const quint32 clusterCount = qFromLittleEndian<quint32>(boot + 92);
    const quint32 rootCluster = qFromLittleEndian<quint32>(boot + 96);
    const quint16 volumeFlags = qFromLittleEndian<quint16>(boot + 106);
    const unsigned char sectorShift = boot[108];
    const unsigned char clusterShift = boot[109];
    const unsigned char fatCount = boot[110];
    const unsigned char percentInUse = boot[112];
    if (qFromLittleEndian<quint16>(boot + 104) != 0x0100 ||
        sectorShift != (logicalSectorSize == 512 ? 9 : 12) ||
        clusterShift > 15 || fatCount != 1 || volumeFlags != 0 ||
        (percentInUse > 100 && percentInUse != 0xff) ||
        volumeSectors < 24 || volumeSectors > quint64(volumeBytes / logicalSectorSize) ||
        fatOffset < 24 || !fatLength || quint64(fatOffset) + fatLength > dataOffset ||
        dataOffset >= volumeSectors || clusterCount < 16 ||
        clusterCount > 0x7ffffffdU || rootCluster < 2 ||
        rootCluster >= quint64(clusterCount) + 2)
        return false;
    const quint64 sectorsPerCluster = quint64(1) << clusterShift;
    if (quint64(dataOffset) + quint64(clusterCount) * sectorsPerCluster > volumeSectors)
        return false;
    quint32 checksum = 0;
    for (qint64 i = 0; i < logicalSectorSize * 11; ++i) {
        if (i == 106 || i == 107 || i == 112) continue;
        checksum = (checksum >> 1) | (checksum << 31);
        checksum += static_cast<unsigned char>(region[i]);
    }
    const char* checkSector = region + logicalSectorSize * 11;
    for (qint64 i = 0; i < logicalSectorSize; i += 4)
        if (qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(checkSector + i)) != checksum)
            return false;
    return true;
}

template <typename Image>
bool exfatBootRegionsConsistent(Image* image, qint64 volumeOffset,
                                qint64 volumeBytes, qint64 logicalSectorSize) {
    if (!image || volumeOffset < 0 || volumeBytes < logicalSectorSize * 24 ||
        volumeOffset > image->capacity() || volumeBytes > image->capacity() - volumeOffset)
        return false;
    QByteArray regions(int(logicalSectorSize * 24), Qt::Uninitialized);
    if (!image->read(volumeOffset, regions.data(), regions.size()) ||
        !exfatBootRegionValid(regions.constData(), volumeBytes, logicalSectorSize) ||
        !exfatBootRegionValid(regions.constData() + logicalSectorSize * 12,
                              volumeBytes, logicalSectorSize))
        return false;
    const char* primary = regions.constData();
    const char* backup = regions.constData() + logicalSectorSize * 12;
    for (qint64 i = 0; i < logicalSectorSize * 11; ++i) {
        if (i == 106 || i == 107 || i == 112) continue;
        if (primary[i] != backup[i]) return false;
    }
    return true;
}

template <typename Image>
bool xdfFloppy(Image* image, qint64 volumeOffset, qint64 volumeBytes,
               qint64 logicalSectorSize, qint64* protectedOffset,
               qint64* protectedSize) {
    if (!image || volumeOffset != 0 || volumeBytes != image->capacity() ||
        logicalSectorSize != kSectorSize || volumeBytes != 1884160) return false;
    char boot[512];
    if (!image->read(0, boot, sizeof(boot)) ||
        std::memcmp(boot + 3, "IBM 20.0", 8) != 0 ||
        !fatBootPlausible(boot, volumeBytes, logicalSectorSize)) return false;
    const unsigned char* bpb = reinterpret_cast<const unsigned char*>(boot);
    if (qFromLittleEndian<quint16>(bpb + 19) != 3680 ||
        qFromLittleEndian<quint16>(bpb + 22) != 11 ||
        qFromLittleEndian<quint16>(bpb + 24) != 23 ||
        qFromLittleEndian<quint16>(bpb + 26) != 2 || bpb[16] != 2 ||
        qFromLittleEndian<quint16>(bpb + 17) != 224) return false;
    const qint64 firstFat = qint64(qFromLittleEndian<quint16>(bpb + 14)) * logicalSectorSize;
    const qint64 fatBytes = qint64(qFromLittleEndian<quint16>(bpb + 22)) * logicalSectorSize;
    char compatibilityBoot[512];
    if (!image->read(firstFat + fatBytes, compatibilityBoot,
                     sizeof(compatibilityBoot)) ||
        std::memcmp(compatibilityBoot + 3, "XDFv1.3i", 8) != 0 ||
        static_cast<unsigned char>(compatibilityBoot[510]) != 0x55 ||
        static_cast<unsigned char>(compatibilityBoot[511]) != 0xaa) return false;
    if (protectedOffset) *protectedOffset = firstFat + fatBytes;
    if (protectedSize) *protectedSize = fatBytes;
    return true;
}

template <typename Image>
bool fatMirrorsConsistent(Image* image, qint64 volumeOffset,
                          qint64 volumeBytes, qint64 logicalSectorSize) {
    if (!image || volumeOffset < 0 || volumeBytes <= 0 ||
        volumeOffset > image->capacity() ||
        volumeBytes > image->capacity() - volumeOffset ||
        (logicalSectorSize != 512 && logicalSectorSize != 4096)) return false;
    char boot[4096];
    if (!image->read(volumeOffset, boot, logicalSectorSize) ||
        !fatBootPlausible(boot, volumeBytes, logicalSectorSize)) return false;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(boot);
    const quint16 bytesPerSector = qFromLittleEndian<quint16>(bytes + 11);
    const quint16 reservedSectors = qFromLittleEndian<quint16>(bytes + 14);
    const unsigned char fatCount = bytes[16];
    const quint16 rootEntries = qFromLittleEndian<quint16>(bytes + 17);
    const quint16 fat16Sectors = qFromLittleEndian<quint16>(bytes + 22);
    const quint32 fat32Sectors = qFromLittleEndian<quint32>(bytes + 36);
    const quint64 fatSectors = fat16Sectors ? fat16Sectors : fat32Sectors;
    if (!fatSectors || !reservedSectors || (fatCount != 1 && fatCount != 2) ||
        fatSectors > quint64(volumeBytes / bytesPerSector)) return false;

    const bool fat32 = rootEntries == 0 && fat16Sectors == 0;
    if (fat32) {
        const quint16 extFlags = qFromLittleEndian<quint16>(bytes + 40);
        if ((extFlags & 0xFF70U) != 0) return false;
        if (extFlags & 0x0080U) {
            if ((extFlags & 0x000FU) >= fatCount) return false;
            return true;
        }
    } else if (rootEntries == 0) {
        return false;
    }
    if (fatCount < 2) return true;

    const quint64 fatBytes = fatSectors * bytesPerSector;
    const quint64 firstFat = quint64(volumeOffset) + quint64(reservedSectors) * bytesPerSector;
    const quint64 secondFat = firstFat + fatBytes;
    if (fatBytes > quint64(image->capacity()) || firstFat > quint64(image->capacity()) ||
        secondFat > quint64(image->capacity()) ||
        fatBytes > quint64(image->capacity()) - firstFat ||
        fatBytes > quint64(image->capacity()) - secondFat) return false;

    QByteArray first(64 * 1024, Qt::Uninitialized);
    QByteArray second(64 * 1024, Qt::Uninitialized);
    quint64 compared = 0;
    while (compared < fatBytes) {
        const qint64 count = qint64(qMin<quint64>(quint64(first.size()), fatBytes - compared));
        if (!image->read(qint64(firstFat + compared), first.data(), count) ||
            !image->read(qint64(secondFat + compared), second.data(), count) ||
            std::memcmp(first.constData(), second.constData(), size_t(count)) != 0)
            return false;
        compared += quint64(count);
    }
    return true;
}

template <typename Image>
bool readDiskVolumeTargets(Image* image,
                        std::vector<peare::writer::DiskImageTarget>* targets,
                        int* efiPartitionCount,
                        qint64 logicalSectorSize = kSectorSize,
                        size_t verifyTargetIndex = (std::numeric_limits<size_t>::max)(),
                        bool allowXdf = false) {
    if (!image || !image->valid() || !targets || !efiPartitionCount ||
        (logicalSectorSize != 512 && logicalSectorSize != 4096) ||
        image->capacity() <= 0 || image->capacity() % logicalSectorSize) return false;
    targets->clear();
    *efiPartitionCount = 0;
    char sector[4096];
    if (exfatBootRegionsConsistent(image, 0, image->capacity(), logicalSectorSize)) {
        peare::writer::DiskImageTarget target;
        target.offset = 0;
        target.size = image->capacity();
        target.exfat = true;
        target.label = QStringLiteral("Whole disk exFAT volume (%1 MiB)")
            .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
        targets->push_back(target);
        return true;
    }
    if (image->read(0, sector, logicalSectorSize) &&
        fatBootPlausible(sector, image->capacity(), logicalSectorSize)) {
        qint64 protectedOffset = 0, protectedSize = 0;
        const bool xdf = allowXdf && xdfFloppy(image, 0, image->capacity(),
                                               logicalSectorSize,
                                               &protectedOffset, &protectedSize);
        if (!xdf && !fatMirrorsConsistent(image, 0, image->capacity(), logicalSectorSize))
            return false;
        peare::writer::DiskImageTarget target;
        target.offset = 0;
        target.size = image->capacity();
        target.xdf = xdf;
        target.protectedOffset = protectedOffset;
        target.protectedSize = protectedSize;
        target.label = QStringLiteral("Whole disk FAT volume (%1 MiB)")
            .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
        if (xdf) target.label += QStringLiteral(" (XDF)");
        targets->push_back(target);
        return true;
    }
    if (ntfsImageMountable(image, 0, image->capacity())) {
        peare::writer::DiskImageTarget target;
        target.offset = 0;
        target.size = image->capacity();
        target.ntfs = true;
        target.label = QStringLiteral("Whole disk NTFS volume (%1 MiB)")
            .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
        targets->push_back(target);
        return true;
    }
    bool wholeDiskExt3 = false;
    bool wholeDiskExt4 = false;
    if (extImageMountable(image, 0, image->capacity(), &wholeDiskExt3,
                          &wholeDiskExt4)) {
        peare::writer::DiskImageTarget target;
        target.offset = 0;
        target.size = image->capacity();
        target.ext = true;
        target.ext3 = wholeDiskExt3;
        target.ext4 = wholeDiskExt4;
        target.label = QStringLiteral("Whole disk %1 volume (%2 MiB)")
            .arg(wholeDiskExt4 ? QStringLiteral("Ext4") :
                 wholeDiskExt3 ? QStringLiteral("Ext3") : QStringLiteral("Ext2"))
            .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
        targets->push_back(target);
        return true;
    }

    struct PartitionCandidate {
        qint64 offset;
        qint64 size;
        quint32 number;
        bool efiSystem;
    };
    std::vector<PartitionCandidate> partitions;
    if (!image->read(0, sector, logicalSectorSize) || sector[510] != char(0x55) ||
        static_cast<unsigned char>(sector[511]) != 0xaa) return false;
    bool protectiveMbr = false;
    for (int i = 0; i < 4; ++i)
        if (static_cast<unsigned char>(sector[446 + i * 16 + 4]) == 0xee) protectiveMbr = true;

    if (protectiveMbr) {
        int protectiveCount = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char* entry = reinterpret_cast<const unsigned char*>(sector + 446 + i * 16);
            if (entry[4] == 0xee) {
                ++protectiveCount;
                const quint32 start = qFromLittleEndian<quint32>(entry + 8);
                const quint32 count = qFromLittleEndian<quint32>(entry + 12);
                if (start != 1 || !count || (entry[0] != 0 && entry[0] != 0x80)) return false;
            } else {
                for (int b = 0; b < 16; ++b) if (entry[b]) return false;
            }
        }
        if (protectiveCount != 1) return false;
        QByteArray header(int(logicalSectorSize), Qt::Uninitialized);
        if (!image->read(logicalSectorSize, header.data(), header.size()) ||
            std::memcmp(header.constData(), "EFI PART", 8) != 0) return false;
        const quint32 headerSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 12));
        const quint32 headerCrc = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 16));
        if (headerSize < 92 || headerSize > logicalSectorSize) return false;
        QByteArray checkedHeader = header.left(int(headerSize));
        std::memset(checkedHeader.data() + 16, 0, 4);
        if (crc32Ieee(checkedHeader.constData(), checkedHeader.size()) != headerCrc) return false;
        const quint64 currentLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 24));
        const quint64 backupLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 32));
        const quint64 firstUsableLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 40));
        const quint64 lastUsableLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 48));
        const quint64 entryLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 72));
        const quint32 entryCount = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 80));
        const quint32 entrySize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 84));
        const quint32 entriesCrc = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 88));
        const quint64 finalLba = quint64(image->capacity() / logicalSectorSize) - 1;
        if (qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 8)) != 0x00010000 ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 20)) != 0 ||
            currentLba != 1 || backupLba != finalLba || firstUsableLba < 2 ||
            firstUsableLba > lastUsableLba || lastUsableLba >= backupLba ||
            !entryCount || entryCount > 4096 || entrySize < 128 ||
            entrySize > 4096 || (entrySize & 7) ||
            entryLba > quint64(image->capacity() / logicalSectorSize))
            return false;
        const quint64 entryBytes = quint64(entryCount) * entrySize;
        const quint64 tableOffset = entryLba * logicalSectorSize;
        const quint64 tableSectors = (entryBytes + logicalSectorSize - 1) / logicalSectorSize;
        if (entryBytes > 16 * 1024 * 1024 || entryLba < 2 ||
            entryLba + tableSectors > firstUsableLba ||
            tableOffset > quint64(image->capacity()) ||
            entryBytes > quint64(image->capacity()) - tableOffset) return false;
        QByteArray entries(int(entryBytes), Qt::Uninitialized);
        if (!image->read(qint64(tableOffset), entries.data(), entries.size()) ||
            crc32Ieee(entries.constData(), entries.size()) != entriesCrc) return false;
        QByteArray backupHeader(int(logicalSectorSize), Qt::Uninitialized);
        if (!image->read(qint64(backupLba * logicalSectorSize),
                         backupHeader.data(), backupHeader.size()) ||
            std::memcmp(backupHeader.constData(), "EFI PART", 8) != 0) return false;
        const quint32 backupHeaderSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(backupHeader.constData() + 12));
        if (backupHeaderSize < 92 || backupHeaderSize > logicalSectorSize) return false;
        const quint32 backupHeaderCrc = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(backupHeader.constData() + 16));
        QByteArray checkedBackupHeader = backupHeader.left(int(backupHeaderSize));
        std::memset(checkedBackupHeader.data() + 16, 0, 4);
        const quint64 backupEntryLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(backupHeader.constData() + 72));
        const quint64 backupTableOffset = backupEntryLba * logicalSectorSize;
        QByteArray backupEntries(int(entryBytes), Qt::Uninitialized);
        if (crc32Ieee(checkedBackupHeader.constData(), checkedBackupHeader.size()) != backupHeaderCrc ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(backupHeader.constData() + 8)) != 0x00010000 ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(backupHeader.constData() + 20)) != 0 ||
            qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(backupHeader.constData() + 24)) != backupLba ||
            qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(backupHeader.constData() + 32)) != currentLba ||
            qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(backupHeader.constData() + 40)) != firstUsableLba ||
            qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(backupHeader.constData() + 48)) != lastUsableLba ||
            std::memcmp(backupHeader.constData() + 56, header.constData() + 56, 16) != 0 ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(backupHeader.constData() + 80)) != entryCount ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(backupHeader.constData() + 84)) != entrySize ||
            backupEntryLba <= lastUsableLba || backupEntryLba >= backupLba ||
            backupTableOffset > quint64(image->capacity()) ||
            entryBytes > quint64(image->capacity()) - backupTableOffset ||
            backupEntryLba + tableSectors > backupLba ||
            !image->read(qint64(backupTableOffset), backupEntries.data(), backupEntries.size()) ||
            crc32Ieee(backupEntries.constData(), backupEntries.size()) !=
                qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(backupHeader.constData() + 88)))
            return false;
        for (quint32 i = 0; i < entryCount; ++i) {
            const char* primaryEntry = entries.constData() + quint64(i) * entrySize;
            const char* backupEntry = backupEntries.constData() + quint64(i) * entrySize;
            if (std::memcmp(primaryEntry, backupEntry, 48) != 0 ||
                std::memcmp(primaryEntry + 56, backupEntry + 56, entrySize - 56) != 0)
                return false;
        }
        std::vector<std::pair<quint64, quint64>> partitionRanges;
        for (quint32 i = 0; i < entryCount; ++i) {
            const char* entry = entries.constData() + quint64(i) * entrySize;
            bool emptyType = true;
            for (int b = 0; b < 16; ++b) if (entry[b]) emptyType = false;
            if (emptyType) continue;
            const quint64 first = qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar*>(entry + 32));
            const quint64 last = qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar*>(entry + 40));
            if (first > last || first < firstUsableLba || last > lastUsableLba) return false;
            partitionRanges.push_back(std::make_pair(first, last));
            static const unsigned char kEfiSystemGuid[16] = {
                0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
            };
            const bool efiSystem = std::memcmp(entry, kEfiSystemGuid, sizeof(kEfiSystemGuid)) == 0;
            partitions.push_back({qint64(first * logicalSectorSize),
                                  qint64((last - first + 1) * logicalSectorSize),
                                  i + 1, efiSystem});
        }
        std::sort(partitionRanges.begin(), partitionRanges.end());
        for (size_t i = 1; i < partitionRanges.size(); ++i)
            if (partitionRanges[i].first <= partitionRanges[i - 1].second) return false;
    } else {
        std::vector<std::pair<quint64, quint64>> partitionRanges;
        for (int i = 0; i < 4; ++i) {
            const unsigned char* entry = reinterpret_cast<const unsigned char*>(sector + 446 + i * 16);
            const unsigned char bootIndicator = entry[0];
            const unsigned char partitionType = entry[4];
            const quint32 start = qFromLittleEndian<quint32>(entry + 8);
            const quint32 count = qFromLittleEndian<quint32>(entry + 12);
            if (!bootIndicator && !partitionType && !start && !count) continue;
            if ((bootIndicator != 0 && bootIndicator != 0x80) || !partitionType ||
                !start || !count ||
                quint64(start) + count > quint64(image->capacity() / logicalSectorSize) ||
                partitionType == 0x05 || partitionType == 0x0f || partitionType == 0x85)
                return false;
            partitionRanges.push_back(std::make_pair(start, quint64(start) + count - 1));
            partitions.push_back({qint64(start) * logicalSectorSize,
                                  qint64(count) * logicalSectorSize,
                                  static_cast<quint32>(i + 1), partitionType == 0xef});
        }
        std::sort(partitionRanges.begin(), partitionRanges.end());
        for (size_t i = 1; i < partitionRanges.size(); ++i)
            if (partitionRanges[i].first <= partitionRanges[i - 1].second) return false;
    }

    for (const auto& partition : partitions) {
        if (partition.efiSystem) ++*efiPartitionCount;
        bool partitionExt3 = false;
        bool partitionExt4 = false;
        if (extImageMountable(image, partition.offset, partition.size,
                              &partitionExt3, &partitionExt4)) {
            peare::writer::DiskImageTarget target;
            target.offset = partition.offset;
            target.size = partition.size;
            target.efiSystem = partition.efiSystem;
            target.ext = true;
            target.ext3 = partitionExt3;
            target.ext4 = partitionExt4;
            target.label = QStringLiteral("%1 partition %2 %3 (%4 MiB)")
                .arg(protectiveMbr ? QStringLiteral("GPT") : QStringLiteral("MBR"))
                .arg(partition.number)
                .arg(partitionExt4 ? QStringLiteral("Ext4") :
                     partitionExt3 ? QStringLiteral("Ext3") : QStringLiteral("Ext2"))
                .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
            targets->push_back(target);
            continue;
        }
        if (ntfsImageMountable(image, partition.offset, partition.size)) {
            peare::writer::DiskImageTarget target;
            target.offset = partition.offset;
            target.size = partition.size;
            target.efiSystem = partition.efiSystem;
            target.ntfs = true;
            target.label = QStringLiteral("%1 partition %2 NTFS (%3 MiB)")
                .arg(protectiveMbr ? QStringLiteral("GPT") : QStringLiteral("MBR"))
                .arg(partition.number)
                .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
            targets->push_back(target);
            continue;
        }
        if (exfatBootRegionsConsistent(image, partition.offset, partition.size,
                                       logicalSectorSize)) {
            peare::writer::DiskImageTarget target;
            target.offset = partition.offset;
            target.size = partition.size;
            target.efiSystem = partition.efiSystem;
            target.exfat = true;
            target.label = QStringLiteral("%1 partition %2 exFAT (%3 MiB)")
                .arg(protectiveMbr ? QStringLiteral("GPT") : QStringLiteral("MBR"))
                .arg(partition.number)
                .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
            targets->push_back(target);
            continue;
        }
        if (!image->read(partition.offset, sector, logicalSectorSize) ||
            !fatBootPlausible(sector, partition.size, logicalSectorSize)) continue;
        qint64 protectedOffset = 0, protectedSize = 0;
        const bool xdf = allowXdf && xdfFloppy(image, partition.offset, partition.size,
                                               logicalSectorSize,
                                               &protectedOffset, &protectedSize);
        if (!xdf && !fatMirrorsConsistent(image, partition.offset, partition.size,
                                          logicalSectorSize)) {
            if (targets->size() == verifyTargetIndex) return false;
            continue;
        }
        peare::writer::DiskImageTarget target;
        target.offset = partition.offset;
        target.size = partition.size;
        target.efiSystem = partition.efiSystem;
        target.xdf = xdf;
        target.protectedOffset = protectedOffset;
        target.protectedSize = protectedSize;
        target.label = QStringLiteral("%1 partition %2 FAT (%3 MiB)")
            .arg(protectiveMbr ? QStringLiteral("GPT") : QStringLiteral("MBR"))
            .arg(partition.number)
            .arg(QString::number(target.size / (1024.0 * 1024.0), 'f', 1));
        targets->push_back(target);
    }
    return true;
}

class FileDiskImage final {
public:
    FileDiskImage(QFile* file, qint64 logicalSize) : file_(file), logicalSize_(logicalSize) {}

    bool valid() const {
        return file_ && file_->isOpen() && logicalSize_ > 0 && logicalSize_ <= file_->size();
    }
    qint64 capacity() const { return logicalSize_; }
    bool read(qint64 offset, char* destination, qint64 length) {
        if (!valid() || !destination || offset < 0 || length < 0 ||
            offset > logicalSize_ || length > logicalSize_ - offset || !file_->seek(offset))
            return false;
        qint64 done = 0;
        while (done < length) {
            const qint64 count = file_->read(destination + done, length - done);
            if (count <= 0) return false;
            done += count;
        }
        return true;
    }

private:
    QFile* file_;
    qint64 logicalSize_;
};

bool containsFatTarget(const std::vector<peare::writer::DiskImageTarget>& targets,
                       qint64 offset, qint64 size, bool ntfs = false,
                       bool exfat = false, bool ext = false, bool ext3 = false,
                       bool ext4 = false) {
    for (const peare::writer::DiskImageTarget& target : targets)
        if (target.offset == offset && target.size == size && target.ntfs == ntfs &&
            target.exfat == exfat && target.ext == ext && target.ext3 == ext3 &&
            target.ext4 == ext4)
            return true;
    return false;
}

bool defaultVhdxFatTarget(const std::vector<peare::writer::DiskImageTarget>& targets,
                          int efiPartitionCount, size_t* index) {
    if (!index || targets.empty()) return false;
    if (efiPartitionCount) {
        if (efiPartitionCount != 1) return false;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i].efiSystem) {
                *index = i;
                return true;
            }
        }
        return false;
    }
    if (targets.size() != 1) return false;
    *index = 0;
    return true;
}

QByteArray hashFile(QFile* file, bool* ok) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file || !file->seek(0)) {
        *ok = false;
        return QByteArray();
    }
    std::vector<char> buffer(static_cast<size_t>(kCopyChunkSize));
    while (true) {
        const qint64 count = file->read(buffer.data(), qint64(buffer.size()));
        if (count < 0) {
            *ok = false;
            return QByteArray();
        }
        if (count == 0) break;
        hash.addData(buffer.data(), int(count));
    }
    *ok = file->error() == QFileDevice::NoError;
    return *ok ? hash.result() : QByteArray();
}

class MountedVolume final {
public:
    MountedVolume(QFile* device, qint64 deviceSize, qint64 baseOffset,
                  VhdxImageWriter* vhdxImage, VhdImageWriter* vhdImage,
                  VmdkImageAccess* vmdkImage, bool writable,
                  qint64 logicalSectorSize = kSectorSize,
                  bool mirrorFatCopies = true)
        : locker_(g_fatMutex), device_(device) {
        g_device = device_;
        g_deviceSize = deviceSize;
        g_deviceBaseOffset = baseOffset;
        g_vhdxImage = vhdxImage;
        g_vhdImage = vhdImage;
        g_vmdkImage = vmdkImage;
        g_logicalSectorSize = logicalSectorSize;
        g_writeEnabled = writable;
        result_ = f_mount(&g_filesystem, "0:", 1);
        mounted_ = result_ == FR_OK;
        if (mounted_ && !mirrorFatCopies) g_filesystem.n_fats = 1;
        if (!mounted_) g_device = nullptr;
    }

    ~MountedVolume() {
        if (mounted_) f_mount(nullptr, "0:", 0);
        g_device = nullptr;
        g_deviceSize = 0;
        g_deviceBaseOffset = 0;
        g_logicalSectorSize = kSectorSize;
        g_vhdxImage = nullptr;
        g_vhdImage = nullptr;
        g_vmdkImage = nullptr;
        g_writeEnabled = false;
    }

    bool mounted() const { return mounted_; }
    FRESULT result() const { return result_; }

private:
    std::unique_lock<QMutex> locker_;
    QFile* device_ = nullptr;
    FRESULT result_ = FR_NOT_READY;
    bool mounted_ = false;
};

QString fatError(const QString& operation, FRESULT result) {
    return QStringLiteral("%1 failed in FatFs (error %2).")
        .arg(operation, QString::number(static_cast<int>(result)));
}

bool normalizeFatPath(const QByteArray& utf8, QString* path, QString* error) {
    if (!path || utf8.isEmpty()) {
        if (error) *error = QStringLiteral("A non-empty FAT entry name is required.");
        return false;
    }
    QString name = QString::fromUtf8(utf8.constData(), utf8.size());
    if (name.isEmpty() || name.contains(QChar(0))) {
        if (error) *error = QStringLiteral("The FAT entry name is invalid UTF-8.");
        return false;
    }
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.startsWith(QLatin1Char('/')) ||
        (name.size() >= 2 && name.at(1) == QLatin1Char(':'))) {
        if (error) *error = QStringLiteral("Absolute FAT paths are not allowed.");
        return false;
    }
    const QStringList parts = name.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    static const QString forbidden = QStringLiteral("<>:\"|?*");
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") ||
            part == QStringLiteral("..") || part.endsWith(QLatin1Char(' ')) ||
            part.endsWith(QLatin1Char('.'))) {
            if (error) *error = QStringLiteral("The FAT entry path contains an invalid component.");
            return false;
        }
        for (const QChar c : part) {
            if (c.unicode() < 0x20 || forbidden.contains(c)) {
                if (error) *error = QStringLiteral("The FAT entry path contains an invalid character.");
                return false;
            }
        }
    }
    *path = parts.join(QLatin1Char('/'));
    return true;
}

bool ensureParentDirectories(const QString& path, QString* error) {
    int slash = path.indexOf(QLatin1Char('/'));
    while (slash >= 0) {
        const QByteArray directory = QByteArray("0:/") + path.left(slash).toUtf8();
        FILINFO info;
        const FRESULT stat = f_stat(directory.constData(), &info);
        if (stat == FR_OK) {
            if (!(info.fattrib & AM_DIR)) {
                if (error) *error = QStringLiteral("A FAT path component is not a directory.");
                return false;
            }
        } else if (stat == FR_NO_FILE || stat == FR_NO_PATH) {
            const FRESULT made = f_mkdir(directory.constData());
            if (made != FR_OK) {
                if (error) *error = fatError(QStringLiteral("Create directory"), made);
                return false;
            }
        } else {
            if (error) *error = fatError(QStringLiteral("Inspect directory"), stat);
            return false;
        }
        slash = path.indexOf(QLatin1Char('/'), slash + 1);
    }
    return true;
}

bool deleteFatTree(const QString& path, QString* error, int depth = 0) {
    if (depth > 128) {
        if (error) *error = QStringLiteral("The FAT directory tree is too deep.");
        return false;
    }
    const QByteArray fatPath = QByteArray("0:/") + path.toUtf8();
    FILINFO info;
    FRESULT result = f_stat(fatPath.constData(), &info);
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Inspect FAT entry"), result);
        return false;
    }
    if (info.fattrib & AM_DIR) {
        DIR directory;
        result = f_opendir(&directory, fatPath.constData());
        if (result != FR_OK) {
            if (error) *error = fatError(QStringLiteral("Open FAT directory"), result);
            return false;
        }
        while (true) {
            FILINFO child;
            result = f_readdir(&directory, &child);
            if (result != FR_OK || child.fname[0] == '\0') break;
            const QString childName = QString::fromUtf8(child.fname);
            if (childName == QStringLiteral(".") || childName == QStringLiteral("..")) continue;
            if (!deleteFatTree(path + QLatin1Char('/') + childName, error, depth + 1)) {
                f_closedir(&directory);
                return false;
            }
        }
        const FRESULT closed = f_closedir(&directory);
        if (result != FR_OK || closed != FR_OK) {
            if (error) *error = fatError(QStringLiteral("Read FAT directory"),
                                         result != FR_OK ? result : closed);
            return false;
        }
    }
    result = f_unlink(fatPath.constData());
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Delete FAT entry"), result);
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

void* ff_memalloc(UINT size) { return std::malloc(size); }
void ff_memfree(void* memory) { std::free(memory); }

DWORD get_fattime(void) {
    const QDateTime now = QDateTime::currentDateTime();
    const QDate date = now.date();
    const QTime time = now.time();
    const quint32 year = quint32(qBound(1980, date.year(), 2107) - 1980);
    return (year << 25) | (quint32(date.month()) << 21) |
           (quint32(date.day()) << 16) | (quint32(time.hour()) << 11) |
           (quint32(time.minute()) << 5) | quint32(time.second() / 2);
}

DSTATUS disk_initialize(BYTE drive) {
    return drive == 0 && g_device ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE drive) {
    return drive == 0 && g_device ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE drive, BYTE* buffer, LBA_t sector, UINT count) {
    if (drive != 0 || !g_device || !buffer || !count) return RES_PARERR;
    if (quint64(count) > quint64((std::numeric_limits<qint64>::max)() / g_logicalSectorSize) ||
        quint64(sector) > quint64((std::numeric_limits<qint64>::max)() / g_logicalSectorSize))
        return RES_PARERR;
    const qint64 offset = qint64(sector) * g_logicalSectorSize;
    const qint64 length = qint64(count) * g_logicalSectorSize;
    if (offset < 0 || offset > g_deviceSize || length > g_deviceSize - offset)
        return RES_ERROR;
    if (g_vhdxImage)
        return g_vhdxImage->read(g_deviceBaseOffset + offset,
                                 reinterpret_cast<char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (g_vhdImage)
        return g_vhdImage->read(g_deviceBaseOffset + offset,
                                reinterpret_cast<char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (g_vmdkImage)
        return g_vmdkImage->read(g_deviceBaseOffset + offset,
                                 reinterpret_cast<char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (!g_device->seek(g_deviceBaseOffset + offset)) return RES_ERROR;
    return g_device->read(reinterpret_cast<char*>(buffer), length) == length ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE drive, const BYTE* buffer, LBA_t sector, UINT count) {
    if (drive != 0 || !g_device || !g_writeEnabled || !buffer || !count)
        return drive == 0 && g_device && !g_writeEnabled ? RES_WRPRT : RES_PARERR;
    if (quint64(count) > quint64((std::numeric_limits<qint64>::max)() / g_logicalSectorSize) ||
        quint64(sector) > quint64((std::numeric_limits<qint64>::max)() / g_logicalSectorSize))
        return RES_PARERR;
    const qint64 offset = qint64(sector) * g_logicalSectorSize;
    const qint64 length = qint64(count) * g_logicalSectorSize;
    if (offset < 0 || offset > g_deviceSize || length > g_deviceSize - offset)
        return RES_ERROR;
    if (g_vhdxImage)
        return g_vhdxImage->write(g_deviceBaseOffset + offset,
                                  reinterpret_cast<const char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (g_vhdImage)
        return g_vhdImage->write(g_deviceBaseOffset + offset,
                                 reinterpret_cast<const char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (g_vmdkImage)
        return g_vmdkImage->write(g_deviceBaseOffset + offset,
                                  reinterpret_cast<const char*>(buffer), length) ? RES_OK : RES_ERROR;
    if (!g_device->seek(g_deviceBaseOffset + offset)) return RES_ERROR;
    return g_device->write(reinterpret_cast<const char*>(buffer), length) == length ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void* buffer) {
    if (drive != 0 || !g_device) return RES_PARERR;
    if (command == CTRL_SYNC) return g_device->flush() ? RES_OK : RES_ERROR;
    if (command == GET_SECTOR_COUNT && buffer) {
        *static_cast<LBA_t*>(buffer) = LBA_t(g_deviceSize / g_logicalSectorSize);
        return RES_OK;
    }
    if (command == GET_SECTOR_SIZE && buffer) {
        *static_cast<WORD*>(buffer) = WORD(g_logicalSectorSize);
        return RES_OK;
    }
    if (command == GET_BLOCK_SIZE && buffer) {
        *static_cast<DWORD*>(buffer) = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

}  // extern "C"

namespace peare {
namespace writer {

static bool recoverImageJournalForTargetQuery(const QString& imagePath);

bool DiskImageUpdate::canUpdate(const QString& imagePath) {
    return !targets(imagePath).empty();
}

static std::vector<DiskImageTarget> scanDiskImageTargets(const QString& imagePath) {
    std::vector<DiskImageTarget> result;
    QFile image(imagePath);
    if (!image.open(QIODevice::ReadOnly)) return result;
    if (hasVmdkSignature(&image)) {
        std::unique_ptr<VmdkImageAccess> vmdk;
        if (isStreamOptimizedVmdk(&image))
            vmdk.reset(new VmdkStreamOptimizedWriter(&image, false));
        else
            vmdk.reset(new VmdkImageWriter(&image, false));
        std::vector<DiskImageTarget> candidates;
        int efiPartitionCount = 0;
        if (!vmdk->valid() ||
            !readDiskVolumeTargets(vmdk.get(), &candidates, &efiPartitionCount)) return result;
        for (const DiskImageTarget& candidate : candidates) {
            if (candidate.ntfs || candidate.ext) {
                result.push_back(candidate);
                continue;
            }
            MountedVolume volume(&image, candidate.size, candidate.offset,
                                 nullptr, nullptr, vmdk.get(), false,
                                 kSectorSize, !candidate.xdf);
            if (volume.mounted()) result.push_back(candidate);
        }
        return result;
    }
    if (hasVmdkDescriptor(&image)) {
        QString vmdkError;
        VmdkSplitImageWriter vmdk(imagePath, false, &vmdkError);
        std::vector<DiskImageTarget> candidates;
        int efiPartitionCount = 0;
        const bool readTargets = vmdk.valid() &&
            readDiskVolumeTargets(&vmdk, &candidates, &efiPartitionCount);
        if (!readTargets) return result;
        for (const DiskImageTarget& candidate : candidates) {
            if (candidate.ntfs || candidate.ext) {
                result.push_back(candidate);
                continue;
            }
            MountedVolume volume(&image, candidate.size, candidate.offset,
                                 nullptr, nullptr, &vmdk, false,
                                 kSectorSize, !candidate.xdf);
            if (volume.mounted()) result.push_back(candidate);
        }
        return result;
    }
    if (hasVhdxSignature(&image)) {
        VhdxImageWriter vhdx(&image, false);
        std::vector<DiskImageTarget> candidates;
        int efiPartitionCount = 0;
        if (!vhdx.valid() || !readDiskVolumeTargets(
                &vhdx, &candidates, &efiPartitionCount, vhdx.logicalSectorSize()))
            return result;
        Q_UNUSED(efiPartitionCount);
        for (const DiskImageTarget& candidate : candidates) {
            if (candidate.ntfs || candidate.ext) {
                result.push_back(candidate);
                continue;
            }
            MountedVolume volume(&image, candidate.size, candidate.offset,
                                 &vhdx, nullptr, nullptr, false,
                                 vhdx.logicalSectorSize(), !candidate.xdf);
            if (volume.mounted()) result.push_back(candidate);
        }
        return result;
    }

    if (hasVhdSignature(&image)) {
        QString vhdError;
        VhdImageWriter vhd(&image, false, &vhdError);
        if (!vhd.valid()) return result;
        std::vector<DiskImageTarget> candidates;
        int efiPartitionCount = 0;
        if (!readDiskVolumeTargets(&vhd, &candidates, &efiPartitionCount)) return result;
        for (const DiskImageTarget& candidate : candidates) {
            if (candidate.ntfs || candidate.ext) {
                result.push_back(candidate);
                continue;
            }
            MountedVolume volume(&image, candidate.size, candidate.offset,
                                 nullptr, &vhd, nullptr, false,
                                 kSectorSize, !candidate.xdf);
            if (volume.mounted()) result.push_back(candidate);
        }
        return result;
    }

    qint64 logicalSize = 0;
    bool fixedVhd = false;
    if (!logicalImageSize(&image, &logicalSize, &fixedVhd)) return result;
    FileDiskImage disk(&image, logicalSize);
    std::vector<DiskImageTarget> candidates;
    int efiPartitionCount = 0;
    if (!readDiskVolumeTargets(&disk, &candidates, &efiPartitionCount,
                            kSectorSize, (std::numeric_limits<size_t>::max)(), true))
        return result;
    Q_UNUSED(fixedVhd);
    for (const DiskImageTarget& candidate : candidates) {
        if (candidate.ntfs || candidate.ext) {
            result.push_back(candidate);
            continue;
        }
        MountedVolume volume(&image, candidate.size, candidate.offset,
                             nullptr, nullptr, nullptr, false,
                             kSectorSize, !candidate.xdf);
        if (volume.mounted()) result.push_back(candidate);
    }
    return result;
}

std::vector<DiskImageTarget> DiskImageUpdate::targets(const QString& imagePath) {
    if (!recoverImageJournalForTargetQuery(imagePath)) return {};
    return scanDiskImageTargets(imagePath);
}

class ExternalWriteGuard final {
public:
    explicit ExternalWriteGuard(const QString& path) : path_(path) {
#ifdef Q_OS_WIN
        handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        valid_ = handle_ != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(handle_, &originalInfo_);
#else
        const QByteArray encodedPath = QFile::encodeName(path);
        descriptor_ = ::open(encodedPath.constData(), O_RDWR);
        valid_ = descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0 &&
            ::fstat(descriptor_, &originalInfo_) == 0 && pathMatchesDescriptor();
        if (!valid_ && descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    ~ExternalWriteGuard() {
#ifdef Q_OS_WIN
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    bool valid() const { return valid_; }

#ifdef Q_OS_WIN
    bool writeAt(qint64 offset, const char* data, qint64 length) {
        LARGE_INTEGER position;
        position.QuadPart = offset;
        if (!valid_ || offset < 0 || length < 0 ||
            !SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
        while (length > 0) {
            DWORD written = 0;
            const DWORD request = DWORD(qMin<qint64>(length,
                qint64((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(handle_, data, request, &written, nullptr) || written == 0)
                return false;
            data += written;
            length -= written;
        }
        return true;
    }

    bool setSize(qint64 size) {
        LARGE_INTEGER position;
        position.QuadPart = size;
        return valid_ && size >= 0 && SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) &&
            SetEndOfFile(handle_);
    }

    bool flush() { return valid_ && FlushFileBuffers(handle_); }

    bool identityMatches(quint32 volume, quint32 high, quint32 low) const {
        return valid_ && originalInfo_.dwVolumeSerialNumber == volume &&
            originalInfo_.nFileIndexHigh == high && originalInfo_.nFileIndexLow == low;
    }

    bool sizeInRange(quint64 minimum, quint64 maximum) const {
        LARGE_INTEGER size;
        return valid_ && minimum <= maximum && GetFileSizeEx(handle_, &size) &&
            size.QuadPart >= 0 && quint64(size.QuadPart) >= minimum &&
            quint64(size.QuadPart) <= maximum;
    }

    quint32 volumeSerial() const { return originalInfo_.dwVolumeSerialNumber; }
    quint32 fileIndexHigh() const { return originalInfo_.nFileIndexHigh; }
    quint32 fileIndexLow() const { return originalInfo_.nFileIndexLow; }
#else
    bool writeAt(qint64 offset, const char* data, qint64 length) {
        if (!valid_ || !pathMatchesDescriptor() || offset < 0 || length < 0 ||
            offset > (std::numeric_limits<off_t>::max)() - length)
            return false;
        qint64 writtenTotal = 0;
        while (writtenTotal < length) {
            const size_t request = size_t(qMin<qint64>(
                length - writtenTotal, (std::numeric_limits<ssize_t>::max)()));
            const ssize_t written = ::pwrite(descriptor_, data + writtenTotal,
                request, off_t(offset + writtenTotal));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            writtenTotal += written;
        }
        return true;
    }

    bool setSize(qint64 size) {
        return valid_ && pathMatchesDescriptor() && size >= 0 &&
            quint64(size) <= quint64((std::numeric_limits<off_t>::max)()) &&
            ::ftruncate(descriptor_, off_t(size)) == 0;
    }

    bool flush() { return valid_ && pathMatchesDescriptor() && ::fsync(descriptor_) == 0; }

    bool identityMatches(quint32 volume, quint32 high, quint32 low) const {
        const quint64 index = (quint64(high) << 32) | quint64(low);
        return valid_ && pathMatchesDescriptor() &&
            quint32(quint64(originalInfo_.st_dev)) == volume &&
            quint64(originalInfo_.st_ino) == index;
    }

    bool sizeInRange(quint64 minimum, quint64 maximum) const {
        struct stat info;
        return valid_ && minimum <= maximum && pathMatchesDescriptor() &&
            ::fstat(descriptor_, &info) == 0 && info.st_size >= 0 &&
            quint64(info.st_size) >= minimum && quint64(info.st_size) <= maximum;
    }

    quint32 volumeSerial() const { return quint32(quint64(originalInfo_.st_dev)); }
    quint32 fileIndexHigh() const {
        return quint32(quint64(originalInfo_.st_ino) >> 32);
    }
    quint32 fileIndexLow() const { return quint32(quint64(originalInfo_.st_ino)); }
#endif

    bool matches(const QByteArray& expected) {
#ifdef Q_OS_WIN
        Q_UNUSED(expected);
        if (!valid_) return false;
        BY_HANDLE_FILE_INFORMATION currentInfo = {};
        HANDLE current = CreateFileW(reinterpret_cast<LPCWSTR>(path_.utf16()), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (current == INVALID_HANDLE_VALUE) return false;
        const bool matched = GetFileInformationByHandle(current, &currentInfo) &&
            currentInfo.dwVolumeSerialNumber == originalInfo_.dwVolumeSerialNumber &&
            currentInfo.nFileIndexHigh == originalInfo_.nFileIndexHigh &&
            currentInfo.nFileIndexLow == originalInfo_.nFileIndexLow &&
            currentInfo.nFileSizeHigh == originalInfo_.nFileSizeHigh &&
            currentInfo.nFileSizeLow == originalInfo_.nFileSizeLow &&
            CompareFileTime(&currentInfo.ftLastWriteTime, &originalInfo_.ftLastWriteTime) == 0;
        CloseHandle(current);
        return matched;
#else
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::vector<char> buffer(static_cast<size_t>(kCopyChunkSize));
        struct stat info;
        if (!valid_ || !pathMatchesDescriptor() ||
            ::fstat(descriptor_, &info) != 0 || info.st_size < 0)
            return false;
        qint64 offset = 0;
        while (offset < info.st_size) {
            const ssize_t count = ::pread(descriptor_, buffer.data(),
                size_t(qMin<qint64>(qint64(buffer.size()), info.st_size - offset)),
                off_t(offset));
            if (count <= 0) return false;
            hash.addData(buffer.data(), int(count));
            offset += count;
        }
        return hash.result() == expected;
#endif
    }

private:
#ifdef Q_OS_WIN
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION originalInfo_ = {};
#else
    bool pathMatchesDescriptor() const {
        struct stat descriptorInfo;
        struct stat pathInfo;
        const QByteArray encodedPath = QFile::encodeName(path_);
        return descriptor_ >= 0 &&
            ::fstat(descriptor_, &descriptorInfo) == 0 &&
            ::stat(encodedPath.constData(), &pathInfo) == 0 &&
            descriptorInfo.st_dev == pathInfo.st_dev &&
            descriptorInfo.st_ino == pathInfo.st_ino;
    }

    int descriptor_ = -1;
    struct stat originalInfo_ = {};
#endif
    QString path_;
    bool valid_ = false;
};

class OverlayImageFile final : public QTemporaryFile {
public:
    OverlayImageFile(const QString& fileTemplate, const QString& sourcePath)
        : QTemporaryFile(fileTemplate), sourcePath_(sourcePath) {}

    bool openOverlay(qint64 sourceSize, QString* error) {
        source_.setFileName(sourcePath_);
        if (!QTemporaryFile::open(QIODevice::ReadWrite | QIODevice::Unbuffered) ||
            !source_.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            if (error) *error = source_.errorString().isEmpty()
                ? QTemporaryFile::errorString() : source_.errorString();
            return false;
        }
#ifdef Q_OS_WIN
        DWORD returned = 0;
        if (!DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(handle())),
                             FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
            if (error) *error = QStringLiteral("Could not create a sparse update overlay (Windows error %1).")
                .arg(GetLastError());
            return false;
        }
#endif
        overlayReader_.setFileName(fileName());
        if (!resize(sourceSize) ||
            !overlayReader_.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            if (error) *error = errorString().isEmpty()
                ? QStringLiteral("Could not initialize the sparse update overlay.") : errorString();
            return false;
        }
        sourceSize_ = sourceSize;
        logicalSize_ = sourceSize;
        return true;
    }

    const std::vector<std::pair<qint64, qint64>>& dirtyRanges() const { return dirtyRanges_; }
    qint64 originalSize() const { return sourceSize_; }

    bool readOriginal(qint64 offset, char* data, qint64 length) {
        if (offset < 0 || length < 0 || !source_.seek(offset)) return false;
        qint64 remaining = qMin<qint64>(length, qMax<qint64>(0, sourceSize_ - offset));
        qint64 copied = 0;
        while (copied < remaining) {
            const qint64 count = source_.read(data + copied, remaining - copied);
            if (count <= 0) return false;
            copied += count;
        }
        if (copied < length) std::memset(data + copied, 0, size_t(length - copied));
        return true;
    }

    bool readOverlay(qint64 offset, char* data, qint64 length) {
        if (offset < 0 || length < 0 || !overlayReader_.seek(offset)) return false;
        qint64 copied = 0;
        while (copied < length) {
            const qint64 count = overlayReader_.read(data + copied, length - copied);
            if (count <= 0) return false;
            copied += count;
        }
        return true;
    }

    qint64 size() const override { return qMax(logicalSize_, QTemporaryFile::size()); }

protected:
    qint64 readData(char* data, qint64 maximum) override {
        const qint64 offset = QTemporaryFile::pos();
        if (offset < 0 || maximum < 0 || offset >= size()) return 0;
        const qint64 count = qMin(maximum, size() - offset);
        if (!readOriginal(offset, data, count)) return -1;
        const qint64 end = offset + count;
        for (const auto& range : dirtyRanges_) {
            const qint64 first = qMax(offset, range.first);
            const qint64 last = qMin(end, range.second);
            if (first < last && !readOverlay(first, data + (first - offset), last - first))
                return -1;
        }
        if (!QTemporaryFile::seek(end)) return -1;
        return count;
    }

    qint64 writeData(const char* data, qint64 length) override {
        const qint64 offset = QTemporaryFile::pos();
        if (offset < 0 || length < 0) return -1;
        const qint64 written = QTemporaryFile::writeData(data, length);
        if (written <= 0) return written;
        logicalSize_ = qMax(logicalSize_, offset + written);
        addDirtyRange(offset, offset + written);
        return written;
    }

private:
    void addDirtyRange(qint64 first, qint64 last) {
        auto it = dirtyRanges_.begin();
        while (it != dirtyRanges_.end() && it->second < first) ++it;
        while (it != dirtyRanges_.end() && it->first <= last) {
            first = qMin(first, it->first);
            last = qMax(last, it->second);
            it = dirtyRanges_.erase(it);
        }
        dirtyRanges_.insert(it, std::make_pair(first, last));
    }

    QString sourcePath_;
    QFile source_;
    QFile overlayReader_;
    qint64 sourceSize_ = 0;
    qint64 logicalSize_ = 0;
    std::vector<std::pair<qint64, qint64>> dirtyRanges_;
};

static bool restoreImageJournal(const QString& imagePath, ExternalWriteGuard* guard,
                               QString* error) {
    const QString journalPath = imagePath + QStringLiteral(".peare-journal");
    if (!QFileInfo::exists(journalPath)) return true;
    QFile journal(journalPath);
    if (!guard || !guard->valid() || !journal.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("An image update journal exists but cannot be opened safely.");
        return false;
    }
    QDataStream input(&journal);
    input.setByteOrder(QDataStream::LittleEndian);
    char magic[8] = {};
    quint32 state = 0, volume = 0, fileHigh = 0, fileLow = 0;
    quint64 originalSize = 0, finalSize = 0, rangeCount = 0;
    if (input.readRawData(magic, sizeof(magic)) != sizeof(magic)) {
        if (error) *error = QStringLiteral("The image update journal header is incomplete.");
        return false;
    }
    input >> state >> volume >> fileHigh >> fileLow >> originalSize >> finalSize >> rangeCount;
    const bool checksummed = std::memcmp(magic, "PEAREJ02", 8) == 0;
    const quint64 maximumSize = quint64((std::numeric_limits<qint64>::max)());
    if (input.status() != QDataStream::Ok ||
        (!checksummed && std::memcmp(magic, "PEAREJ01", 8) != 0) ||
        state > 1 || originalSize == 0 || originalSize > maximumSize ||
        finalSize < originalSize || finalSize > maximumSize ||
        rangeCount > quint64(journal.size() / 16) ||
        !guard->identityMatches(volume, fileHigh, fileLow) ||
        (state == 0 && !guard->sizeInRange(originalSize, finalSize)) ||
        (state == 1 && !guard->sizeInRange(finalSize, finalSize))) {
        if (error) *error = QStringLiteral("The image update journal is invalid or belongs to another image.");
        return false;
    }
    QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
    const qint64 rangesOffset = journal.pos();
    if (rangesOffset < 0 || rangesOffset > journal.size()) {
        if (error) *error = QStringLiteral("The image update journal header is invalid.");
        return false;
    }
    quint64 previousEnd = 0;
    for (quint64 index = 0; index < rangeCount; ++index) {
        quint64 offset = 0, length = 0;
        input >> offset >> length;
        const qint64 dataOffset = journal.pos();
        const quint64 digestSize = checksummed ? 32 : 0;
        if (input.status() != QDataStream::Ok || !length || offset < previousEnd ||
            offset > originalSize || length > originalSize - offset ||
            dataOffset < 0 || dataOffset > journal.size() ||
            quint64(journal.size() - dataOffset) < length ||
            quint64(journal.size() - dataOffset) - length < digestSize) {
            if (error) *error = QStringLiteral("The image update journal contains an invalid range.");
            return false;
        }
        if (checksummed) {
            char metadata[16];
            qToLittleEndian<quint64>(offset, reinterpret_cast<uchar*>(metadata));
            qToLittleEndian<quint64>(length, reinterpret_cast<uchar*>(metadata + 8));
            QCryptographicHash rangeHash(QCryptographicHash::Sha256);
            rangeHash.addData(metadata, sizeof(metadata));
            quint64 verified = 0;
            while (verified < length) {
                const qint64 count = qint64(qMin<quint64>(quint64(buffer.size()),
                                                          length - verified));
                if (input.readRawData(buffer.data(), int(count)) != count) {
                    if (error) *error = QStringLiteral("The image update journal is truncated.");
                    return false;
                }
                rangeHash.addData(buffer.constData(), int(count));
                verified += quint64(count);
            }
            char storedDigest[32];
            const QByteArray actualDigest = rangeHash.result();
            if (input.readRawData(storedDigest, sizeof(storedDigest)) != sizeof(storedDigest) ||
                std::memcmp(storedDigest, actualDigest.constData(), sizeof(storedDigest)) != 0) {
                if (error) *error = QStringLiteral("The image update journal backup checksum is invalid.");
                return false;
            }
        } else if (!journal.seek(dataOffset + qint64(length))) {
            if (error) *error = QStringLiteral("The image update journal contains an invalid range.");
            return false;
        }
        previousEnd = offset + length;
    }
    if (journal.pos() != journal.size() || !journal.seek(rangesOffset)) {
        if (error) *error = QStringLiteral("The image update journal has invalid trailing data.");
        return false;
    }
    if (state == 1) {
        journal.close();
        if (!QFile::remove(journalPath)) {
            if (error) *error = QStringLiteral("The committed image journal could not be removed.");
            return false;
        }
        return true;
    }
    for (quint64 index = 0; index < rangeCount; ++index) {
        quint64 offset = 0, length = 0;
        input >> offset >> length;
        if (input.status() != QDataStream::Ok || offset > originalSize ||
            length > originalSize - offset) {
            if (error) *error = QStringLiteral("The image update journal contains an invalid range.");
            return false;
        }
        quint64 restored = 0;
        while (restored < length) {
            const qint64 count = qint64(qMin<quint64>(quint64(buffer.size()), length - restored));
            if (input.readRawData(buffer.data(), int(count)) != count ||
                !guard->writeAt(qint64(offset + restored), buffer.constData(), count)) {
                if (error) *error = QStringLiteral("Image rollback failed; the journal was retained.");
                return false;
            }
            restored += quint64(count);
        }
        if (checksummed) {
            char storedDigest[32];
            if (input.readRawData(storedDigest, sizeof(storedDigest)) != sizeof(storedDigest)) {
                if (error) *error = QStringLiteral("The image update journal is truncated.");
                return false;
            }
        }
    }
    journal.close();
    if (!guard->setSize(qint64(originalSize)) || !guard->flush()) {
        if (error) *error = QStringLiteral("Image rollback could not be flushed; the journal was retained.");
        return false;
    }
    if (!QFile::remove(journalPath)) {
        if (error) *error = QStringLiteral("Image rollback completed, but its journal could not be removed.");
        return false;
    }
    Q_UNUSED(finalSize);
    return true;
}

static bool recoverImageJournalForTargetQuery(const QString& imagePath) {
    const QString absolutePath = QFileInfo(imagePath).absoluteFilePath();
    if (!QFileInfo::exists(absolutePath + QStringLiteral(".peare-journal"))) return true;
    QLockFile updateLock(absolutePath + QStringLiteral(".peare-fat.lock"));
    updateLock.setStaleLockTime(0);
    if (!updateLock.tryLock(0)) return false;
    ExternalWriteGuard guard(absolutePath);
    if (!guard.valid()) return false;
    return restoreImageJournal(absolutePath, &guard, nullptr);
}

static bool commitImageOverlay(OverlayImageFile* overlay, const QString& imagePath,
                               ExternalWriteGuard* guard, QString* error) {
    if (!overlay || !guard || !guard->valid() || !guard->matches(QByteArray())) {
        if (error) *error = QStringLiteral("The original image changed before commit.");
        return false;
    }
    const QString journalPath = imagePath + QStringLiteral(".peare-journal");
    if (QFileInfo::exists(journalPath)) {
        if (error) *error = QStringLiteral("An earlier image update journal must be recovered first.");
        return false;
    }
    QSaveFile journal(journalPath);
    if (!journal.open(QIODevice::WriteOnly)) {
        if (error) *error = journal.errorString();
        return false;
    }
    QDataStream output(&journal);
    output.setByteOrder(QDataStream::LittleEndian);
    quint64 journalRangeCount = 0;
    for (const auto& range : overlay->dirtyRanges())
        if (range.first < overlay->originalSize()) ++journalRangeCount;
    output.writeRawData("PEAREJ02", 8);
    output << quint32(0) << guard->volumeSerial() << guard->fileIndexHigh()
           << guard->fileIndexLow() << quint64(overlay->originalSize())
           << quint64(overlay->size()) << journalRangeCount;
    QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
    for (const auto& range : overlay->dirtyRanges()) {
        if (range.first >= overlay->originalSize()) continue;
        const qint64 length = qMax<qint64>(0,
            qMin(range.second, overlay->originalSize()) - range.first);
        output << quint64(range.first) << quint64(length);
        char metadata[16];
        qToLittleEndian<quint64>(quint64(range.first), reinterpret_cast<uchar*>(metadata));
        qToLittleEndian<quint64>(quint64(length), reinterpret_cast<uchar*>(metadata + 8));
        QCryptographicHash rangeHash(QCryptographicHash::Sha256);
        rangeHash.addData(metadata, sizeof(metadata));
        qint64 copied = 0;
        while (copied < length) {
            const qint64 count = qMin<qint64>(buffer.size(), length - copied);
            if (!overlay->readOriginal(range.first + copied, buffer.data(), count)) {
                if (error) *error = QStringLiteral("Could not prepare the image rollback journal.");
                return false;
            }
            rangeHash.addData(buffer.constData(), int(count));
            if (output.writeRawData(buffer.constData(), int(count)) != count) {
                if (error) *error = QStringLiteral("Could not prepare the image rollback journal.");
                return false;
            }
            copied += count;
        }
        const QByteArray digest = rangeHash.result();
        if (output.writeRawData(digest.constData(), digest.size()) != digest.size()) {
            if (error) *error = QStringLiteral("Could not prepare the image rollback journal.");
            return false;
        }
    }
    if (output.status() != QDataStream::Ok || !syncFileToDisk(&journal, error) ||
        !journal.commit()) {
        if (error && error->isEmpty()) *error = journal.errorString();
        return false;
    }
    for (const auto& range : overlay->dirtyRanges()) {
        qint64 copied = 0;
        while (range.first + copied < range.second) {
            const qint64 count = qMin<qint64>(buffer.size(), range.second - range.first - copied);
            if (!overlay->readOverlay(range.first + copied, buffer.data(), count) ||
                !guard->writeAt(range.first + copied, buffer.constData(), count)) {
                if (error) *error = QStringLiteral("Image commit failed; rolling back its journal.");
                QString rollbackError;
                restoreImageJournal(imagePath, guard, &rollbackError);
                if (error && !rollbackError.isEmpty()) *error += QLatin1Char(' ') + rollbackError;
                return false;
            }
            copied += count;
        }
    }
    if (!guard->setSize(overlay->size()) || !guard->flush()) {
        if (error) *error = QStringLiteral("Image commit could not be flushed; rolling back its journal.");
        QString rollbackError;
        restoreImageJournal(imagePath, guard, &rollbackError);
        if (error && !rollbackError.isEmpty()) *error += QLatin1Char(' ') + rollbackError;
        return false;
    }
    QFile committedJournal(journalPath);
    if (!committedJournal.open(QIODevice::ReadWrite) || !committedJournal.seek(8)) {
        if (error) *error = QStringLiteral("The image committed, but its journal state could not be saved.");
        return false;
    }
    const char committed[4] = {1, 0, 0, 0};
    if (committedJournal.write(committed, sizeof(committed)) != sizeof(committed) ||
        !syncFileToDisk(&committedJournal, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("Could not mark the image journal committed.");
        return false;
    }
    committedJournal.close();
    if (!QFile::remove(journalPath)) {
        if (error) *error = QStringLiteral("The image committed, but its journal could not be removed.");
        return false;
    }
    return true;
}

DiskImageUpdate::DiskImageUpdate() = default;
DiskImageUpdate::~DiskImageUpdate() { abort(); }

bool DiskImageUpdate::begin(const QString& imagePath, QString* error) {
    const std::vector<DiskImageTarget> available = targets(imagePath);
    if (available.size() != 1) {
        if (error) *error = available.empty()
            ? QStringLiteral("No supported writable filesystem volume was found.")
            : QStringLiteral("The image has multiple writable filesystem volumes; select a target first.");
        return false;
    }
    return begin(imagePath, 0, error);
}

bool DiskImageUpdate::begin(const QString& imagePath, size_t targetIndex,
                            QString* error, qint64 journalOffset,
                            qint64 journalSize) {
    if (staging_) {
        if (error) *error = QStringLiteral("A disk image update is already active.");
        return false;
    }
    if ((journalOffset == 0) != (journalSize == 0) ||
        journalOffset < 0 || journalSize < 0) {
        if (error) *error = QStringLiteral("The external journal range is invalid.");
        return false;
    }
    extJournalOffset_ = journalOffset;
    extJournalSize_ = journalSize;
    std::unique_ptr<QLockFile> updateLock(new QLockFile(
        QFileInfo(imagePath).absoluteFilePath() + QStringLiteral(".peare-fat.lock")));
    updateLock->setStaleLockTime(0);
    if (!updateLock->tryLock(0)) {
        if (error) *error = QStringLiteral(
            "Another Peare FAT update holds the image transaction lock.");
        return false;
    }
    {
        ExternalWriteGuard recoveryGuard(QFileInfo(imagePath).absoluteFilePath());
        if (!recoveryGuard.valid() || !restoreImageJournal(imagePath, &recoveryGuard, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("An interrupted image update could not be recovered safely.");
            return false;
        }
    }
    const std::vector<DiskImageTarget> available = scanDiskImageTargets(imagePath);
    if (targetIndex >= available.size()) {
        if (error) *error = QStringLiteral("The selected FAT volume is no longer available.");
        return false;
    }
    const DiskImageTarget target = available[targetIndex];
    QFile input(imagePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = input.errorString();
        return false;
    }
    const bool vmdkSource = hasVmdkSignature(&input);
    const bool streamOptimizedSource = vmdkSource && isStreamOptimizedVmdk(&input);
    const bool splitVmdkSource = !vmdkSource && hasVmdkDescriptor(&input);
    const bool vhdxSource = !vmdkSource && hasVhdxSignature(&input);
    const bool dynamicVhdSource = !vmdkSource && !splitVmdkSource && !vhdxSource &&
        hasVhdSignature(&input);
    std::unique_ptr<ExternalWriteGuard> externalWriteGuard;
    if (!splitVmdkSource) {
        input.close();
        externalWriteGuard.reset(new ExternalWriteGuard(QFileInfo(imagePath).absoluteFilePath()));
        if (!externalWriteGuard->valid()) {
            if (error) *error = QStringLiteral("The image could not be write-guarded.");
            return false;
        }
        if (!input.open(QIODevice::ReadOnly) || !externalWriteGuard->matches(QByteArray())) {
            if (error) *error = QStringLiteral("The image changed while its write guard was acquired.");
            return false;
        }
    }
    if (target.xdf && (vmdkSource || splitVmdkSource || vhdxSource || dynamicVhdSource)) {
        if (error) *error = QStringLiteral("XDF updates are supported only for raw or fixed-VHD images.");
        return false;
    }
    qint64 logicalSize = 0;
    bool fixedVhd = false;
    qint64 volumeSectorSize = kSectorSize;
    qint64 volumeOffset = target.offset, volumeSize = target.size;
    if (vmdkSource) {
        QString vmdkError;
        std::unique_ptr<VmdkImageAccess> sourceVmdk;
        if (streamOptimizedSource)
            sourceVmdk.reset(new VmdkStreamOptimizedWriter(&input, false, QString(), &vmdkError));
        else
            sourceVmdk.reset(new VmdkImageWriter(&input, false, &vmdkError));
        std::vector<DiskImageTarget> sourceTargets;
        int efiPartitionCount = 0;
        if (!sourceVmdk->valid() ||
            !readDiskVolumeTargets(sourceVmdk.get(), &sourceTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            targetIndex >= sourceTargets.size() ||
            sourceTargets[targetIndex].offset != volumeOffset ||
            sourceTargets[targetIndex].size != volumeSize ||
            sourceTargets[targetIndex].ntfs != target.ntfs ||
            sourceTargets[targetIndex].ext != target.ext ||
            sourceTargets[targetIndex].ext3 != target.ext3 ||
            sourceTargets[targetIndex].ext4 != target.ext4 ||
            sourceTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = vmdkError.isEmpty()
                ? QStringLiteral("The selected monolithic sparse VMDK FAT volume changed or is invalid.")
                : vmdkError;
            return false;
        }
        logicalSize = sourceVmdk->capacity();
    } else if (splitVmdkSource) {
        QString vmdkError;
        VmdkSplitImageWriter sourceVmdk(imagePath, false, &vmdkError);
        std::vector<DiskImageTarget> sourceTargets;
        int efiPartitionCount = 0;
        if (!sourceVmdk.valid() ||
            !readDiskVolumeTargets(&sourceVmdk, &sourceTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            targetIndex >= sourceTargets.size() ||
            sourceTargets[targetIndex].offset != volumeOffset ||
            sourceTargets[targetIndex].size != volumeSize ||
            sourceTargets[targetIndex].ntfs != target.ntfs ||
            sourceTargets[targetIndex].ext != target.ext ||
            sourceTargets[targetIndex].ext3 != target.ext3 ||
            sourceTargets[targetIndex].ext4 != target.ext4 ||
            sourceTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = vmdkError.isEmpty()
                ? QStringLiteral("The selected split VMDK FAT volume changed or is invalid.")
                : vmdkError;
            return false;
        }
        logicalSize = sourceVmdk.capacity();
    } else if (vhdxSource) {
        QString vhdxError;
        VhdxImageWriter sourceVhdx(&input, false, &vhdxError);
        std::vector<DiskImageTarget> sourceTargets;
        int efiPartitionCount = 0;
        if (!sourceVhdx.valid() ||
            !readDiskVolumeTargets(&sourceVhdx, &sourceTargets, &efiPartitionCount,
                                sourceVhdx.logicalSectorSize(), targetIndex) ||
            targetIndex >= sourceTargets.size() ||
            sourceTargets[targetIndex].offset != volumeOffset ||
            sourceTargets[targetIndex].size != volumeSize ||
            sourceTargets[targetIndex].ntfs != target.ntfs ||
            sourceTargets[targetIndex].ext != target.ext ||
            sourceTargets[targetIndex].ext3 != target.ext3 ||
            sourceTargets[targetIndex].ext4 != target.ext4 ||
            sourceTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = vhdxError.isEmpty() ? QStringLiteral(
                "The selected VHDX FAT volume changed or is ambiguous.") : vhdxError;
            return false;
        }
        logicalSize = sourceVhdx.capacity();
        volumeSectorSize = sourceVhdx.logicalSectorSize();
    } else if (dynamicVhdSource) {
        QString vhdError;
        VhdImageWriter sourceVhd(&input, false, &vhdError);
        std::vector<DiskImageTarget> sourceTargets;
        int efiPartitionCount = 0;
        if (!sourceVhd.valid() ||
            !readDiskVolumeTargets(&sourceVhd, &sourceTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            targetIndex >= sourceTargets.size() ||
            sourceTargets[targetIndex].offset != volumeOffset ||
            sourceTargets[targetIndex].size != volumeSize ||
            sourceTargets[targetIndex].ntfs != target.ntfs ||
            sourceTargets[targetIndex].ext != target.ext ||
            sourceTargets[targetIndex].ext3 != target.ext3 ||
            sourceTargets[targetIndex].ext4 != target.ext4 ||
            sourceTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = vhdError.isEmpty()
                ? QStringLiteral("The selected dynamic VHD FAT volume changed or is invalid.")
                : vhdError;
            return false;
        }
        logicalSize = sourceVhd.capacity();
    } else if (!logicalImageSize(&input, &logicalSize, &fixedVhd)) {
        if (error) *error = QStringLiteral(
            "The raw image or fixed VHD footer is invalid.");
        return false;
    } else {
        FileDiskImage sourceDisk(&input, logicalSize);
        std::vector<DiskImageTarget> sourceTargets;
        int efiPartitionCount = 0;
        if (!readDiskVolumeTargets(&sourceDisk, &sourceTargets, &efiPartitionCount,
                                kSectorSize, targetIndex, true) ||
            !containsFatTarget(sourceTargets, volumeOffset, volumeSize,
                               target.ntfs, target.exfat, target.ext, target.ext3,
                               target.ext4) ||
            targetIndex >= sourceTargets.size() ||
            sourceTargets[targetIndex].xdf != target.xdf) {
            if (error) *error = QStringLiteral(
                "The selected raw or fixed VHD FAT volume changed or has an unsupported partition layout.");
            return false;
        }
        volumeOffset = target.offset;
        volumeSize = target.size;
    }
    QByteArray xdfProtectedBytes;
    if (target.xdf) {
        if (target.protectedOffset < 0 || target.protectedSize <= 0 ||
            target.protectedSize > std::numeric_limits<int>::max() ||
            !input.seek(target.protectedOffset)) {
            if (error) *error = QStringLiteral("The XDF compatibility area is invalid.");
            return false;
        }
        xdfProtectedBytes = input.read(target.protectedSize);
        if (xdfProtectedBytes.size() != target.protectedSize) {
            if (error) *error = QStringLiteral("The XDF compatibility area could not be read.");
            return false;
        }
    }
    const qint64 inputSize = input.size();
    if (inputSize <= 0 || !input.seek(0)) {
        if (error) *error = QStringLiteral("The FAT image is empty or could not be rewound.");
        return false;
    }
    std::unique_ptr<QTemporaryFile> staging;
#ifdef Q_OS_WIN
    if (!splitVmdkSource)
        staging.reset(new OverlayImageFile(QFileInfo(imagePath).absoluteDir().filePath(
            QStringLiteral(".peare-update-XXXXXX")), imagePath));
    else
#endif
        staging.reset(new QTemporaryFile);
    staging->setAutoRemove(true);
    bool useOverlay = false;
#ifdef Q_OS_WIN
    OverlayImageFile* overlay = dynamic_cast<OverlayImageFile*>(staging.get());
    useOverlay = overlay != nullptr;
    const bool opened = overlay ? overlay->openOverlay(inputSize, error) : staging->open();
#else
    const bool opened = staging->open();
#endif
    if (!opened) {
        if (error && error->isEmpty()) *error = staging->errorString();
        return false;
    }
    QCryptographicHash originalHash(QCryptographicHash::Sha256);
    if (!useOverlay) {
        std::vector<char> buffer(static_cast<size_t>(kCopyChunkSize));
        qint64 remaining = inputSize;
        while (remaining > 0) {
            const qint64 count = input.read(buffer.data(),
                qMin<qint64>(remaining, qint64(buffer.size())));
            if (count <= 0) {
                if (error) *error = QStringLiteral("Reading the source image failed (%1 bytes remain): %2")
                    .arg(QString::number(remaining), input.errorString());
                return false;
            }
            originalHash.addData(buffer.data(), int(count));
            if (!writeAll(staging.get(), buffer.data(), count)) {
                if (error) *error = QStringLiteral("Writing the staged image failed: %1")
                    .arg(staging->errorString());
                return false;
            }
            remaining -= count;
        }
        if (input.size() != inputSize) {
            if (error) *error = QStringLiteral("The source image changed while it was being staged.");
            return false;
        }
    }
    if (externalWriteGuard && !externalWriteGuard->matches(originalHash.result())) {
        if (error) *error = QStringLiteral("The source image changed while it was being staged.");
        return false;
    }
    if (!staging->flush()) {
        if (error) *error = QStringLiteral("Flushing the staged image failed: %1")
            .arg(staging->errorString());
        return false;
    }
    if (!staging->seek(0)) {
        if (error) *error = QStringLiteral("Could not rewind the staged image: %1")
            .arg(staging->errorString());
        return false;
    }
    qint64 stagedLogicalSize = 0;
    bool stagedFixedVhd = false;
    std::unique_ptr<VhdxImageWriter> stagedVhdx;
    std::unique_ptr<VhdImageWriter> stagedVhd;
    std::unique_ptr<VmdkImageAccess> stagedVmdk;
    if (vmdkSource) {
        if (streamOptimizedSource)
            stagedVmdk.reset(new VmdkStreamOptimizedWriter(staging.get(), true,
                QFileInfo(imagePath).fileName()));
        else
            stagedVmdk.reset(new VmdkImageWriter(staging.get(), true));
        std::vector<DiskImageTarget> stagedTargets;
        int efiPartitionCount = 0;
        if (!stagedVmdk->valid() || stagedVmdk->capacity() != logicalSize ||
            !readDiskVolumeTargets(stagedVmdk.get(), &stagedTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            targetIndex >= stagedTargets.size() ||
            stagedTargets[targetIndex].offset != volumeOffset ||
            stagedTargets[targetIndex].size != volumeSize ||
            stagedTargets[targetIndex].ntfs != target.ntfs ||
            stagedTargets[targetIndex].ext != target.ext ||
            stagedTargets[targetIndex].ext3 != target.ext3 ||
            stagedTargets[targetIndex].ext4 != target.ext4 ||
            stagedTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = QStringLiteral("The staged monolithic sparse VMDK layout or FAT target changed.");
            return false;
        }
        stagedLogicalSize = stagedVmdk->capacity();
    } else if (splitVmdkSource) {
        QString vmdkError;
        stagedVmdk.reset(new VmdkSplitImageWriter(imagePath, true, &vmdkError));
        std::vector<DiskImageTarget> stagedTargets;
        int efiPartitionCount = 0;
        if (!stagedVmdk->valid() || stagedVmdk->capacity() != logicalSize ||
            !readDiskVolumeTargets(stagedVmdk.get(), &stagedTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            targetIndex >= stagedTargets.size() ||
            stagedTargets[targetIndex].offset != volumeOffset ||
            stagedTargets[targetIndex].size != volumeSize ||
            stagedTargets[targetIndex].ntfs != target.ntfs ||
            stagedTargets[targetIndex].ext != target.ext ||
            stagedTargets[targetIndex].ext3 != target.ext3 ||
            stagedTargets[targetIndex].ext4 != target.ext4 ||
            stagedTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = vmdkError.isEmpty()
                ? QStringLiteral("The staged split VMDK layout or FAT target changed.")
                : vmdkError;
            return false;
        }
        stagedLogicalSize = stagedVmdk->capacity();
    } else if (vhdxSource) {
        stagedVhdx.reset(new VhdxImageWriter(staging.get(), true));
        std::vector<DiskImageTarget> stagedTargets;
        int efiPartitionCount = 0;
        if (!stagedVhdx->valid() || stagedVhdx->capacity() != logicalSize ||
            stagedVhdx->logicalSectorSize() != volumeSectorSize ||
            !readDiskVolumeTargets(stagedVhdx.get(), &stagedTargets, &efiPartitionCount,
                                stagedVhdx->logicalSectorSize(), targetIndex) ||
            targetIndex >= stagedTargets.size() ||
            stagedTargets[targetIndex].offset != volumeOffset ||
            stagedTargets[targetIndex].size != volumeSize ||
            stagedTargets[targetIndex].ntfs != target.ntfs ||
            stagedTargets[targetIndex].ext != target.ext ||
            stagedTargets[targetIndex].ext3 != target.ext3 ||
            stagedTargets[targetIndex].ext4 != target.ext4 ||
            stagedTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = QStringLiteral("The staged VHDX layout changed or the selected volume is ambiguous.");
            return false;
        }
        stagedLogicalSize = stagedVhdx->capacity();
    } else if (dynamicVhdSource) {
        stagedVhd.reset(new VhdImageWriter(staging.get(), true));
        std::vector<DiskImageTarget> stagedTargets;
        int efiPartitionCount = 0;
        if (!stagedVhd->valid() || stagedVhd->capacity() != logicalSize ||
            !readDiskVolumeTargets(stagedVhd.get(), &stagedTargets, &efiPartitionCount,
                                kSectorSize, targetIndex) ||
            stagedTargets.size() != available.size() || targetIndex >= stagedTargets.size() ||
            stagedTargets[targetIndex].offset != volumeOffset ||
            stagedTargets[targetIndex].size != volumeSize ||
            stagedTargets[targetIndex].ntfs != target.ntfs ||
            stagedTargets[targetIndex].ext != target.ext ||
            stagedTargets[targetIndex].ext3 != target.ext3 ||
            stagedTargets[targetIndex].ext4 != target.ext4 ||
            stagedTargets[targetIndex].exfat != target.exfat) {
            if (error) *error = QStringLiteral("The staged dynamic VHD layout or FAT target changed.");
            return false;
        }
        stagedLogicalSize = stagedVhd->capacity();
    } else {
        FileDiskImage stagedDisk(staging.get(), logicalSize);
        std::vector<DiskImageTarget> stagedTargets;
        int efiPartitionCount = 0;
        if (!logicalImageSize(staging.get(), &stagedLogicalSize, &stagedFixedVhd) ||
            stagedLogicalSize != logicalSize || stagedFixedVhd != fixedVhd ||
            !readDiskVolumeTargets(&stagedDisk, &stagedTargets, &efiPartitionCount,
                                kSectorSize, targetIndex, true) ||
            !containsFatTarget(stagedTargets, volumeOffset, volumeSize,
                               target.ntfs, target.exfat, target.ext, target.ext3,
                               target.ext4) ||
            targetIndex >= stagedTargets.size() ||
            stagedTargets[targetIndex].xdf != target.xdf) {
            if (error) *error = QStringLiteral("The staged image layout is invalid or ambiguous.");
            return false;
        }
    }
    std::unique_ptr<DiskImageIoContext> ntfsContext;
    std::unique_ptr<NtfsVolumeUpdate> ntfsUpdate;
    std::unique_ptr<DiskImageIoContext> extContext;
    std::unique_ptr<ExtVolumeUpdate> extUpdate;
    if (target.ntfs) {
        ntfsContext.reset(new (std::nothrow) DiskImageIoContext);
        if (!ntfsContext) {
            if (error) *error = QStringLiteral("Could not allocate the NTFS image context.");
            return false;
        }
        ntfsContext->file = staging.get();
        ntfsContext->offset = volumeOffset;
        ntfsContext->size = volumeSize;
        ntfsContext->vhdx = stagedVhdx.get();
        ntfsContext->vhd = stagedVhd.get();
        ntfsContext->vmdk = stagedVmdk.get();
        peare_ntfs_io io = {ntfsContext.get(), volumeSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        ntfsUpdate.reset(new (std::nothrow) NtfsVolumeUpdate);
        if (!ntfsUpdate || !ntfsUpdate->begin(io, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("The selected NTFS volume could not be mounted safely for update.");
            return false;
        }
    } else if (target.ext) {
        extContext.reset(new (std::nothrow) DiskImageIoContext);
        extUpdate.reset(new (std::nothrow) ExtVolumeUpdate);
        if (!extContext || !extUpdate) {
            if (error) *error = QStringLiteral("Could not allocate the Ext volume writer.");
            return false;
        }
        extContext->file = staging.get();
        extContext->offset = volumeOffset;
        extContext->size = volumeSize;
        extContext->vhdx = stagedVhdx.get();
        extContext->vhd = stagedVhd.get();
        extContext->vmdk = stagedVmdk.get();
        peare_ext_io volumeIo = {extContext.get(), volumeSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        qint64 extSize = 0;
        if (!extVolumeLength(volumeIo, &extSize) ||
            (extJournalSize_ &&
             (extJournalOffset_ > logicalSize ||
              extJournalSize_ > logicalSize - extJournalOffset_ ||
              (extJournalOffset_ < volumeOffset + extSize &&
               volumeOffset < extJournalOffset_ + extJournalSize_)))) {
            if (error) *error = QStringLiteral(
                "The external journal range is invalid or overlaps the Ext filesystem.");
            return false;
        }
        extContext->size = extSize;
        peare_ext_io io = {extContext.get(), extSize, ntfsImageRead,
                           ntfsImageWrite, ntfsImageFlush};
        peare_ext_io journalIo = {};
        std::unique_ptr<DiskImageIoContext> journalContext;
        if (extJournalSize_) {
            journalContext.reset(new (std::nothrow) DiskImageIoContext);
            if (!journalContext) {
                if (error) *error = QStringLiteral(
                    "Could not allocate the external journal image context.");
                return false;
            }
            journalContext->file = staging.get();
            journalContext->offset = extJournalOffset_;
            journalContext->size = extJournalSize_;
            journalContext->vhdx = stagedVhdx.get();
            journalContext->vhd = stagedVhd.get();
            journalContext->vmdk = stagedVmdk.get();
            journalIo = {journalContext.get(), extJournalSize_, ntfsImageRead,
                         ntfsImageWrite, ntfsImageFlush};
        }
        if (!extUpdate->begin(io, error,
                extJournalSize_ ? &journalIo : nullptr)) return false;
        extJournalContext_ = std::move(journalContext);
    } else {
        if (extJournalSize_) {
            if (error) *error = QStringLiteral(
                "An external journal range is valid only for an Ext volume.");
            return false;
        }
        MountedVolume volume(staging.get(), volumeSize, volumeOffset,
                              stagedVhdx.get(), stagedVhd.get(), stagedVmdk.get(), false,
                              volumeSectorSize, !target.xdf);
        if (!volume.mounted()) {
            if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
            return false;
        }
    }
    if (externalWriteGuard && !externalWriteGuard->matches(originalHash.result())) {
        if (ntfsUpdate) ntfsUpdate->abort();
        if (extUpdate) extUpdate->abort();
        if (error) *error = QStringLiteral(
            "The image changed while it was being staged or could not be write-guarded.");
        return false;
    }
    originalSize_ = inputSize;
    imageLogicalSize_ = logicalSize;
    fatVolumeOffset_ = volumeOffset;
    fatVolumeSize_ = volumeSize;
    fatSectorSize_ = volumeSectorSize;
    xdfVolume_ = target.xdf;
    xdfProtectedOffset_ = target.protectedOffset;
    xdfProtectedBytes_ = xdfProtectedBytes;
    originalSha256_ = originalHash.result();
    imagePath_ = imagePath;
    vhdx_ = std::move(stagedVhdx);
    vhd_ = std::move(stagedVhd);
    vmdk_ = std::move(stagedVmdk);
    ntfsContext_ = std::move(ntfsContext);
    ntfsUpdate_ = std::move(ntfsUpdate);
    extContext_ = std::move(extContext);
    extUpdate_ = std::move(extUpdate);
    ntfsVolume_ = target.ntfs;
    extVolume_ = target.ext;
    extJournalOffset_ = 0;
    extJournalSize_ = 0;
    splitVmdk_ = splitVmdkSource;
    staging_ = std::move(staging);
    updateLock_ = std::move(updateLock);
    externalWriteGuard_ = std::move(externalWriteGuard);
    return true;
}

bool DiskImageUpdate::beginNew(const QString& imagePath, const QString& variant, QString* error) {
    return beginNew(imagePath, QStringLiteral("fat"), variant, variant, error);
}

bool DiskImageUpdate::beginNew(const QString& imagePath, const QString& formatId,
                              const QString& variant, const QString& filesystem,
                              QString* error) {
    if (staging_) {
        if (error) *error = QStringLiteral("A FAT update is already active.");
        return false;
    }
    const bool rawFat = formatId == QStringLiteral("fat");
    const bool floppy = formatId == QStringLiteral("floppy");
    const bool xdfFloppyCreate = floppy && variant == QStringLiteral("xdf-1.84m");
    const bool rawDisk = formatId == QStringLiteral("raw-disk");
    const bool splitVmdkCreate = formatId == QStringLiteral("vmdk") &&
        variant == QStringLiteral("two-gb-sparse");
    const bool vhdx4KnCreate = formatId == QStringLiteral("vhdx") &&
        (variant == QStringLiteral("fixed-4kn") ||
         variant == QStringLiteral("dynamic-4kn"));
    if (rawFat && filesystem != variant) {
        if (error) *error = QStringLiteral("The standalone FAT variant does not accept a separate filesystem.");
        return false;
    }
    FloppyCreateGeometry floppyGeometry = {};
    if (floppy && (filesystem != QStringLiteral("fat12") ||
                   !floppyCreateGeometry(variant, &floppyGeometry))) {
        if (error) *error = QStringLiteral("The requested floppy geometry is not supported.");
        return false;
    }
    const QByteArray xdfCompatibility = xdfFloppyCreate ? xdfCompatibilityImage() : QByteArray();
    if (rawDisk && variant != QStringLiteral("mbr") && variant != QStringLiteral("gpt")) {
        if (error) *error = QStringLiteral("The requested raw disk layout is not supported.");
        return false;
    }
    const bool supportedFilesystem = filesystem == QStringLiteral("fat12") ||
        filesystem == QStringLiteral("fat16") || filesystem == QStringLiteral("fat32") ||
        filesystem == QStringLiteral("exfat") || filesystem == QStringLiteral("ntfs") ||
        filesystem == QStringLiteral("ext2") || filesystem == QStringLiteral("ext3") ||
        filesystem == QStringLiteral("ext4");
    if (!rawFat && !floppy && !supportedFilesystem) {
        if (error) *error = QStringLiteral("The requested virtual disk filesystem is not supported.");
        return false;
    }
    qint64 imageSize = 0;
    qint64 containerSize = 0;
    qint64 volumeOffset = 0;
    BYTE format = FM_ANY;
    if (filesystem == QStringLiteral("fat12")) {
        imageSize = rawFat ? 1440 * 1024 :
            rawDisk ? 16 * 1024 * 1024 :
            floppy ? floppyGeometry.capacity : 32 * 1024 * 1024;
        format = FM_FAT;
    } else if (filesystem == QStringLiteral("fat16")) {
        imageSize = rawFat ? 32 * 1024 * 1024 : 64 * 1024 * 1024;
        format = FM_FAT;
    } else if (filesystem == QStringLiteral("fat32")) {
        imageSize = vhdx4KnCreate ? 512 * 1024 * 1024 : 128 * 1024 * 1024;
        format = FM_FAT32;
    } else if (filesystem == QStringLiteral("exfat")) {
        imageSize = 128 * 1024 * 1024;
        format = FM_EXFAT;
    } else if (filesystem == QStringLiteral("ntfs")) {
        imageSize = 128 * 1024 * 1024;
    } else if (filesystem == QStringLiteral("ext2") ||
               filesystem == QStringLiteral("ext3") ||
               filesystem == QStringLiteral("ext4")) {
        imageSize = 128 * 1024 * 1024;
    }
    if (rawFat && !supportedFilesystem) {
        if (error) *error = QStringLiteral("The requested FAT image variant is not supported.");
        return false;
    }
    containerSize = imageSize;
    std::unique_ptr<QLockFile> updateLock(new QLockFile(
        QFileInfo(imagePath).absoluteFilePath() + QStringLiteral(".peare-fat.lock")));
    updateLock->setStaleLockTime(0);
    if (!updateLock->tryLock(0)) {
        if (error) *error = QStringLiteral("Another Peare FAT update holds the output transaction lock.");
        return false;
    }
    const bool targetExisted = QFileInfo::exists(imagePath);
    std::unique_ptr<ExternalWriteGuard> externalWriteGuard;
    QByteArray targetHash;
    qint64 targetSize = 0;
    if (targetExisted) {
        externalWriteGuard.reset(new ExternalWriteGuard(QFileInfo(imagePath).absoluteFilePath()));
        QFile original(imagePath);
        if (!externalWriteGuard->valid() || !original.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("The existing output image cannot be locked for replacement.");
            return false;
        }
        targetSize = original.size();
        bool hashOk = false;
        targetHash = hashFile(&original, &hashOk);
        if (!hashOk) {
            if (error) *error = original.errorString();
            return false;
        }
    }
    std::unique_ptr<QTemporaryFile> staging(new QTemporaryFile);
    if (!staging->open()) {
        if (error) *error = staging->errorString();
        return false;
    }
    std::unique_ptr<VhdxImageWriter> newVhdx;
    std::unique_ptr<VhdImageWriter> newVhd;
    std::unique_ptr<VmdkImageAccess> newVmdk;
    std::unique_ptr<DiskImageIoContext> extContext;
    std::unique_ptr<ExtVolumeUpdate> extUpdate;
    bool initialized = false;
    if (rawFat || floppy || splitVmdkCreate) {
        initialized = staging->resize(imageSize);
    } else if (rawDisk) {
        initialized = createRawPartitionedImage(staging.get(), variant, filesystem,
                                                imageSize, &containerSize,
                                                &volumeOffset, error);
    } else if (formatId == QStringLiteral("vhd")) {
        if (variant == QStringLiteral("fixed")) {
            initialized = createVhd(staging.get(), containerSize, true, error);
        } else if (variant == QStringLiteral("dynamic")) {
            initialized = createVhd(staging.get(), containerSize, false, error);
            if (initialized) {
                newVhd.reset(new VhdImageWriter(staging.get(), true, error));
                initialized = newVhd->valid();
            }
        }
    } else if (formatId == QStringLiteral("vhdx")) {
        const bool fixed = variant == QStringLiteral("fixed") ||
            variant == QStringLiteral("fixed-4kn");
        const bool fourKn = variant == QStringLiteral("fixed-4kn") ||
            variant == QStringLiteral("dynamic-4kn");
        if (variant == QStringLiteral("fixed") || variant == QStringLiteral("dynamic") ||
            fourKn) {
            const quint32 sectorSize = fourKn ? 4096U : 512U;
            initialized = createVhdx(staging.get(), containerSize, fixed,
                                     sectorSize, error);
            if (initialized) {
                newVhdx.reset(new VhdxImageWriter(staging.get(), true, error));
                initialized = newVhdx->valid();
            }
        }
    } else if (formatId == QStringLiteral("vmdk") &&
               variant == QStringLiteral("monolithic-sparse")) {
        initialized = createMonolithicSparseVmdk(
            staging.get(), containerSize, QFileInfo(imagePath).fileName(), error);
        if (initialized) {
            newVmdk.reset(new VmdkImageWriter(staging.get(), true, error));
            initialized = newVmdk->valid();
        }
    }
    if (!initialized) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The requested virtual disk variant is not supported.");
        return false;
    }
    if (!staging->flush()) {
        if (error) *error = staging->errorString();
        return false;
    }
    FRESULT formatted = FR_DISK_ERR;
    bool ntfsFormatted = false;
    bool extFormatted = false;
    if (filesystem == QStringLiteral("ntfs")) {
#ifdef PEARE_NTFS_WRITER
        DiskImageIoContext context = {};
        context.file = staging.get();
        context.offset = volumeOffset;
        context.size = imageSize;
        context.vhdx = newVhdx.get();
        context.vhd = newVhd.get();
        context.vmdk = newVmdk.get();
        peare_ntfs_io io = {&context, imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        QMutexLocker locker(&g_ntfsFormatMutex);
        const quint32 logicalSectorSize = newVhdx
            ? quint32(newVhdx->logicalSectorSize()) : quint32(kSectorSize);
        ntfsFormatted = peare_ntfs_format(&io,
            quint64(volumeOffset / logicalSectorSize), logicalSectorSize) == 0;
        if (!ntfsFormatted) {
            if (error) *error = QStringLiteral("NTFS-3G could not format the staged volume.");
            return false;
        }
#else
        if (error) *error = QStringLiteral("NTFS volume creation is unavailable in this build.");
        return false;
#endif
    } else if (filesystem == QStringLiteral("ext2") ||
               filesystem == QStringLiteral("ext3") ||
               filesystem == QStringLiteral("ext4")) {
        extContext.reset(new (std::nothrow) DiskImageIoContext);
        if (!extContext) {
            if (error) *error = QStringLiteral("Could not allocate the Ext image context.");
            return false;
        }
        extContext->file = staging.get();
        extContext->offset = volumeOffset;
        extContext->size = imageSize;
        extContext->vhdx = newVhdx.get();
        extContext->vhd = newVhd.get();
        extContext->vmdk = newVmdk.get();
        peare_ext_io io = {extContext.get(), imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        const bool ext3 = filesystem == QStringLiteral("ext3");
        const bool ext4 = filesystem == QStringLiteral("ext4");
        extFormatted = ExtVolumeUpdate::format(io, ext3, ext4, error);
        if (!extFormatted) return false;
    } else {
        QMutexLocker locker(&g_fatMutex);
        g_device = staging.get();
        g_deviceSize = imageSize;
        g_deviceBaseOffset = volumeOffset;
        g_logicalSectorSize = newVhdx
            ? newVhdx->logicalSectorSize() : kSectorSize;
        g_vhdxImage = newVhdx.get();
        g_vhdImage = newVhd.get();
        g_vmdkImage = newVmdk.get();
        g_writeEnabled = true;
        MKFS_PARM options = {};
        options.fmt = format | FM_SFD;
        options.n_fat = filesystem == QStringLiteral("exfat") ? 1 : 2;
        options.n_root = floppy ? floppyGeometry.rootEntries :
            filesystem == QStringLiteral("fat12") ? 512 : 0;
        if (!rawFat && !floppy && filesystem == QStringLiteral("fat12"))
            options.au_size = 16 * 1024;
        std::vector<BYTE> work(FF_MAX_SS);
        formatted = f_mkfs("0:", &options, work.data(), UINT(work.size()));
        g_device = nullptr;
        g_deviceSize = 0;
        g_vhdxImage = nullptr;
        g_vhdImage = nullptr;
        g_vmdkImage = nullptr;
        g_writeEnabled = false;
        g_logicalSectorSize = kSectorSize;
    }
    if (!ntfsFormatted && !extFormatted && formatted != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Format FAT image"), formatted);
        return false;
    }
    if (floppy || rawDisk) {
        QByteArray boot(int(kSectorSize), Qt::Uninitialized);
        if (!staging->seek(volumeOffset) ||
            staging->read(boot.data(), boot.size()) != boot.size()) {
            if (error) *error = staging->errorString();
            return false;
        }
        if (floppy) {
            qToLittleEndian<quint16>(floppyGeometry.sectorsPerTrack,
                                     reinterpret_cast<uchar*>(boot.data() + 24));
            qToLittleEndian<quint16>(floppyGeometry.heads,
                                     reinterpret_cast<uchar*>(boot.data() + 26));
            if (variant == QStringLiteral("xdf-1.84m"))
                std::memcpy(boot.data() + 3, "IBM 20.0", 8);
        } else if (filesystem != QStringLiteral("exfat") &&
                   filesystem != QStringLiteral("ntfs") &&
                   filesystem != QStringLiteral("ext2") &&
                   filesystem != QStringLiteral("ext3") &&
                   filesystem != QStringLiteral("ext4")) {
            qToLittleEndian<quint32>(quint32(volumeOffset / kSectorSize),
                                     reinterpret_cast<uchar*>(boot.data() + 28));
        }
        if (!writeAt(staging.get(), volumeOffset, boot)) {
            if (error) *error = staging->errorString();
            return false;
        }
        if (xdfFloppyCreate &&
            !writeAt(staging.get(), 12 * kSectorSize, xdfCompatibility)) {
            if (error) *error = staging->errorString();
            return false;
        }
        if (rawDisk && filesystem == QStringLiteral("fat32")) {
            QByteArray backup = boot;
            if (!staging->seek(volumeOffset + 6 * kSectorSize) ||
                staging->read(backup.data(), backup.size()) != backup.size()) {
                if (error) *error = staging->errorString();
                return false;
            }
            qToLittleEndian<quint32>(quint32(volumeOffset / kSectorSize),
                                     reinterpret_cast<uchar*>(backup.data() + 28));
            if (!writeAt(staging.get(), volumeOffset + 6 * kSectorSize, backup)) {
                if (error) *error = staging->errorString();
                return false;
            }
        }
    }
    if ((newVhdx && !newVhdx->finish(error)) ||
        (newVhd && !newVhd->finish(error)) ||
        (newVmdk && !newVmdk->finish(error))) return false;
    {
    if (filesystem == QStringLiteral("ntfs")) {
        DiskImageIoContext context = {};
        context.file = staging.get();
        context.offset = volumeOffset;
        context.size = imageSize;
        context.vhdx = newVhdx.get();
        context.vhd = newVhd.get();
        context.vmdk = newVmdk.get();
        peare_ntfs_io io = {&context, imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        if (!peare::writer::NtfsVolumeUpdate::probe(io)) {
            if (error) *error = QStringLiteral("The newly formatted NTFS volume failed validation.");
            return false;
        }
    } else if (filesystem == QStringLiteral("ext2") ||
               filesystem == QStringLiteral("ext3") ||
               filesystem == QStringLiteral("ext4")) {
        peare_ext_io io = {extContext.get(), imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        bool formattedExt3 = false;
        bool formattedExt4 = false;
        const bool expectedExt3 = filesystem == QStringLiteral("ext3");
        const bool expectedExt4 = filesystem == QStringLiteral("ext4");
        if (!ExtVolumeUpdate::probe(io, &formattedExt3, &formattedExt4) ||
            formattedExt3 != (expectedExt3 || expectedExt4) ||
            formattedExt4 != expectedExt4) {
            if (error) *error = QStringLiteral("The newly formatted Ext volume failed validation.");
            return false;
        }
    } else {
        MountedVolume volume(staging.get(), imageSize, volumeOffset,
                             newVhdx.get(), newVhd.get(), newVmdk.get(), false,
                             newVhdx ? newVhdx->logicalSectorSize() : kSectorSize);
        if (!volume.mounted()) {
            if (error) *error = fatError(QStringLiteral("Validate new FAT image"), volume.result());
            return false;
        }
    }
    }
    if (rawDisk) {
        FileDiskImage rawImage(staging.get(), containerSize);
        std::vector<DiskImageTarget> targets;
        int efiPartitionCount = 0;
        if (!readDiskVolumeTargets(&rawImage, &targets, &efiPartitionCount, kSectorSize, 0) ||
            targets.size() != 1) {
            if (error) *error = QStringLiteral("The new raw partition table or file system is invalid.");
            return false;
        }
    }
    std::unique_ptr<DiskImageIoContext> ntfsContext;
    std::unique_ptr<NtfsVolumeUpdate> ntfsUpdate;
    if (filesystem == QStringLiteral("ntfs")) {
        ntfsContext.reset(new (std::nothrow) DiskImageIoContext);
        ntfsUpdate.reset(new (std::nothrow) NtfsVolumeUpdate);
        if (!ntfsContext || !ntfsUpdate) {
            if (error) *error = QStringLiteral("Could not allocate the NTFS volume writer.");
            return false;
        }
        ntfsContext->file = staging.get();
        ntfsContext->offset = volumeOffset;
        ntfsContext->size = imageSize;
        ntfsContext->vhdx = newVhdx.get();
        ntfsContext->vhd = newVhd.get();
        ntfsContext->vmdk = newVmdk.get();
        peare_ntfs_io io = {ntfsContext.get(), imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        if (!ntfsUpdate->begin(io, error)) return false;
    } else if (filesystem == QStringLiteral("ext2") ||
               filesystem == QStringLiteral("ext3") ||
               filesystem == QStringLiteral("ext4")) {
        extUpdate.reset(new (std::nothrow) ExtVolumeUpdate);
        if (!extUpdate) {
            if (error) *error = QStringLiteral("Could not allocate the Ext volume writer.");
            return false;
        }
        peare_ext_io io = {extContext.get(), imageSize, ntfsImageRead,
                            ntfsImageWrite, ntfsImageFlush};
        if (!extUpdate->begin(io, error)) return false;
    }
    imagePath_ = imagePath;
    staging_ = std::move(staging);
    updateLock_ = std::move(updateLock);
    externalWriteGuard_ = std::move(externalWriteGuard);
    xdfVolume_ = xdfFloppyCreate;
    xdfProtectedOffset_ = xdfFloppyCreate ? 12 * kSectorSize : 0;
    xdfProtectedBytes_ = xdfCompatibility;
    originalSize_ = targetSize;
    originalSha256_ = targetHash;
    imageLogicalSize_ = containerSize;
    fatVolumeOffset_ = volumeOffset;
    fatVolumeSize_ = imageSize;
    fatSectorSize_ = newVhdx ? newVhdx->logicalSectorSize() : kSectorSize;
    createMode_ = true;
    createTargetExisted_ = targetExisted;
    vhdx_ = std::move(newVhdx);
    vhd_ = std::move(newVhd);
    vmdk_ = std::move(newVmdk);
    ntfsContext_ = std::move(ntfsContext);
    ntfsUpdate_ = std::move(ntfsUpdate);
    extContext_ = std::move(extContext);
    extUpdate_ = std::move(extUpdate);
    ntfsVolume_ = filesystem == QStringLiteral("ntfs");
    extVolume_ = filesystem == QStringLiteral("ext2") ||
                 filesystem == QStringLiteral("ext3") ||
                 filesystem == QStringLiteral("ext4");
    vmdkSplitCreate_ = splitVmdkCreate;
    return true;
}

bool DiskImageUpdate::addFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                             QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No FAT update is active.");
        return false;
    }
    if (createMode_ && !createTargetExisted_ && QFileInfo::exists(imagePath_)) {
        if (error) *error = QStringLiteral("The output image appeared during creation.");
        return false;
    }
    if (ntfsVolume_) {
        if (ntfsUpdate_) return ntfsUpdate_->addFile(sourcePath, entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active NTFS writer is available.");
        return false;
    }
    if (extVolume_) {
        if (extUpdate_) return extUpdate_->addFile(sourcePath, entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active Ext writer is available.");
        return false;
    }
    QString path;
    if (!normalizeFatPath(entryNameUtf8, &path, error)) return false;
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    const qint64 sourceSize = source.size();
    if (sourceSize < 0) {
        if (error) *error = source.errorString();
        return false;
    }
    if (!staging_->flush() || !staging_->seek(0)) {
        if (error) *error = staging_->errorString();
        return false;
    }
    MountedVolume volume(staging_.get(), fatVolumeSize_, fatVolumeOffset_,
                         vhdx_.get(), vhd_.get(), vmdk_.get(), true,
                         fatSectorSize_, !xdfVolume_);
    if (!volume.mounted()) {
        if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
        return false;
    }
    if (!ensureParentDirectories(path, error)) return false;
    const QByteArray fatPath = QByteArray("0:/") + path.toUtf8();
    FIL file;
    FRESULT result = f_open(&file, fatPath.constData(), FA_WRITE | FA_CREATE_NEW);
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Create FAT file"), result);
        return false;
    }
    std::vector<BYTE> buffer(static_cast<size_t>(kFatChunkSize));
    qint64 copied = 0;
    while (copied < sourceSize) {
        const qint64 request = qMin<qint64>(sourceSize - copied, qint64(buffer.size()));
        const qint64 count = source.read(reinterpret_cast<char*>(buffer.data()), request);
        if (count <= 0) {
            f_close(&file);
            if (error) *error = source.errorString().isEmpty()
                ? QStringLiteral("The source file changed while it was being copied.")
                : source.errorString();
            return false;
        }
        UINT written = 0;
        result = f_write(&file, buffer.data(), UINT(count), &written);
        if (result != FR_OK || written != UINT(count)) {
            f_close(&file);
            if (error) *error = result == FR_OK
                ? QStringLiteral("FatFs wrote fewer bytes than requested.")
                : fatError(QStringLiteral("Write FAT file"), result);
            return false;
        }
        copied += count;
    }
    if (source.size() != sourceSize || !source.atEnd()) {
        f_close(&file);
        if (error) *error = QStringLiteral("The source file changed while it was being copied.");
        return false;
    }
    result = f_close(&file);
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Close FAT file"), result);
        return false;
    }
    return true;
}

bool DiskImageUpdate::createDirectory(const QByteArray& entryNameUtf8, QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No FAT update is active.");
        return false;
    }
    if (ntfsVolume_) {
        if (ntfsUpdate_) return ntfsUpdate_->createDirectory(entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active NTFS writer is available.");
        return false;
    }
    if (extVolume_) {
        if (extUpdate_) return extUpdate_->createDirectory(entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active Ext writer is available.");
        return false;
    }
    QString path;
    if (!normalizeFatPath(entryNameUtf8, &path, error)) return false;
    if (!staging_->flush() || !staging_->seek(0)) {
        if (error) *error = staging_->errorString();
        return false;
    }
    MountedVolume volume(staging_.get(), fatVolumeSize_, fatVolumeOffset_,
                         vhdx_.get(), vhd_.get(), vmdk_.get(), true,
                         fatSectorSize_, !xdfVolume_);
    if (!volume.mounted()) {
        if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
        return false;
    }
    if (!ensureParentDirectories(path, error)) return false;
    const QByteArray fatPath = QByteArray("0:/") + path.toUtf8();
    const FRESULT result = f_mkdir(fatPath.constData());
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Create FAT directory"), result);
        return false;
    }
    return true;
}

bool DiskImageUpdate::replaceFile(const QString& sourcePath,
                                 const QByteArray& entryNameUtf8, QString* error) {
    if (ntfsVolume_) {
        if (ntfsUpdate_) return ntfsUpdate_->replaceFile(sourcePath, entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active NTFS writer is available.");
        return false;
    }
    if (extVolume_) {
        if (extUpdate_) return extUpdate_->replaceFile(sourcePath, entryNameUtf8, error);
        if (error) *error = QStringLiteral("No active Ext writer is available.");
        return false;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    QString path;
    if (!normalizeFatPath(entryNameUtf8, &path, error)) return false;
    {
        if (!staging_ || !staging_->flush() || !staging_->seek(0)) {
            if (error) *error = staging_ ? staging_->errorString()
                                         : QStringLiteral("No FAT update is active.");
            return false;
        }
        MountedVolume volume(staging_.get(), fatVolumeSize_, fatVolumeOffset_,
                             vhdx_.get(), vhd_.get(), vmdk_.get(), true,
                             fatSectorSize_, !xdfVolume_);
        if (!volume.mounted()) {
            if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
            return false;
        }
        const QByteArray fatPath = QByteArray("0:/") + path.toUtf8();
        FILINFO info;
        const FRESULT stat = f_stat(fatPath.constData(), &info);
        if (stat != FR_OK || (info.fattrib & AM_DIR)) {
            if (error) *error = stat == FR_OK
                ? QStringLiteral("A FAT directory cannot be replaced with a file.")
                : fatError(QStringLiteral("Find FAT file to replace"), stat);
            return false;
        }
        const FRESULT removed = f_unlink(fatPath.constData());
        if (removed != FR_OK) {
            if (error) *error = fatError(QStringLiteral("Remove FAT file"), removed);
            return false;
        }
    }
    return addFile(sourcePath, entryNameUtf8, error);
}

bool DiskImageUpdate::enableFsVerity(const QByteArray& entryNameUtf8,
                                    QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No disk image update is active.");
        return false;
    }
    if (!extVolume_ || !extUpdate_) {
        if (error) *error = QStringLiteral(
            "fs-verity is supported only by the active Ext filesystem writer.");
        return false;
    }
    return extUpdate_->enableFsVerity(entryNameUtf8, error);
}

bool DiskImageUpdate::renameEntry(const QByteArray& entryNameUtf8,
                                 const QByteArray& newEntryNameUtf8, QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No FAT update is active.");
        return false;
    }
    if (ntfsVolume_) {
        if (ntfsUpdate_)
            return ntfsUpdate_->renameEntry(entryNameUtf8, newEntryNameUtf8, error);
        if (error) *error = QStringLiteral("No active NTFS writer is available.");
        return false;
    }
    if (extVolume_) {
        if (extUpdate_)
            return extUpdate_->renameEntry(entryNameUtf8, newEntryNameUtf8, error);
        if (error) *error = QStringLiteral("No active Ext writer is available.");
        return false;
    }
    QString oldPath, newPath;
    if (!normalizeFatPath(entryNameUtf8, &oldPath, error) ||
        !normalizeFatPath(newEntryNameUtf8, &newPath, error)) return false;
    if (newPath == oldPath) return true;
    if (newPath.startsWith(oldPath + QLatin1Char('/'))) {
        if (error) *error = QStringLiteral("A FAT directory cannot be moved inside itself.");
        return false;
    }
    if (!staging_->flush() || !staging_->seek(0)) {
        if (error) *error = staging_->errorString();
        return false;
    }
    MountedVolume volume(staging_.get(), fatVolumeSize_, fatVolumeOffset_,
                         vhdx_.get(), vhd_.get(), vmdk_.get(), true,
                         fatSectorSize_, !xdfVolume_);
    if (!volume.mounted()) {
        if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
        return false;
    }
    if (!ensureParentDirectories(newPath, error)) return false;
    const QByteArray oldFatPath = QByteArray("0:/") + oldPath.toUtf8();
    const QByteArray newFatPath = QByteArray("0:/") + newPath.toUtf8();
    const FRESULT result = f_rename(oldFatPath.constData(), newFatPath.constData());
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Rename FAT entry"), result);
        return false;
    }
    return true;
}

bool DiskImageUpdate::deleteEntry(const QByteArray& entryNameUtf8, bool recursive,
                                 QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No FAT update is active.");
        return false;
    }
    if (ntfsVolume_) {
        if (ntfsUpdate_) return ntfsUpdate_->deleteEntry(entryNameUtf8, recursive, error);
        if (error) *error = QStringLiteral("No active NTFS writer is available.");
        return false;
    }
    if (extVolume_) {
        if (extUpdate_) return extUpdate_->deleteEntry(entryNameUtf8, recursive, error);
        if (error) *error = QStringLiteral("No active Ext writer is available.");
        return false;
    }
    QString path;
    if (!normalizeFatPath(entryNameUtf8, &path, error)) return false;
    if (!staging_->flush() || !staging_->seek(0)) {
        if (error) *error = staging_->errorString();
        return false;
    }
    MountedVolume volume(staging_.get(), fatVolumeSize_, fatVolumeOffset_,
                         vhdx_.get(), vhd_.get(), vmdk_.get(), true,
                         fatSectorSize_, !xdfVolume_);
    if (!volume.mounted()) {
        if (error) *error = fatError(QStringLiteral("Mount FAT image"), volume.result());
        return false;
    }
    if (recursive) return deleteFatTree(path, error);
    const QByteArray fatPath = QByteArray("0:/") + path.toUtf8();
    const FRESULT result = f_unlink(fatPath.constData());
    if (result != FR_OK) {
        if (error) *error = fatError(QStringLiteral("Delete FAT entry"), result);
        return false;
    }
    return true;
}

bool DiskImageUpdate::finish(QString* error) {
    if (!staging_) {
        if (error) *error = QStringLiteral("No FAT update is active.");
        return false;
    }
    if (createMode_) {
        if (createTargetExisted_) {
            QFile original(imagePath_);
            if (!externalWriteGuard_ || !externalWriteGuard_->matches(originalSha256_) ||
                !original.open(QIODevice::ReadOnly) || original.size() != originalSize_) {
                if (error) *error = QStringLiteral("The existing output image changed during creation.");
                return false;
            }
#ifndef Q_OS_WIN
            bool hashOk = false;
            const QByteArray currentHash = hashFile(&original, &hashOk);
            if (!hashOk || currentHash != originalSha256_) {
                if (error) *error = QStringLiteral("The existing output image changed during creation.");
                return false;
            }
#endif
        } else if (QFileInfo::exists(imagePath_)) {
            if (error) *error = QStringLiteral("The output image appeared during creation.");
            return false;
        }
    }
    if (!createMode_ && externalWriteGuard_) {
        if (!externalWriteGuard_->matches(originalSha256_)) {
            if (error) *error = QStringLiteral("The original FAT image changed during the update.");
            return false;
        }
    }
    if (!createMode_) {
        QFile original(imagePath_);
        if (!original.open(QIODevice::ReadOnly) || original.size() != originalSize_) {
            if (error) *error = QStringLiteral("The original FAT image changed during the update.");
            return false;
        }
#ifndef Q_OS_WIN
        bool hashOk = false;
        const QByteArray currentHash = hashFile(&original, &hashOk);
        if (!hashOk || currentHash != originalSha256_) {
            if (error) *error = QStringLiteral("The original FAT image changed during the update.");
            return false;
        }
#endif
        original.close();
    }
    const bool ntfsDirty = ntfsUpdate_ && ntfsUpdate_->dirty();
    const bool extDirty = extUpdate_ && extUpdate_->dirty();
    if (ntfsUpdate_) {
        const bool closed = ntfsUpdate_->finish(error);
        ntfsUpdate_.reset();
        ntfsContext_.reset();
        if (!closed) return false;
    }
    if (extUpdate_) {
        const bool closed = extUpdate_->finish(error);
        extUpdate_.reset();
        extContext_.reset();
        extJournalContext_.reset();
        if (!closed) return false;
    }
    if ((ntfsVolume_ && !ntfsDirty) || (extVolume_ && !extDirty)) {
        abort();
        return true;
    }
    if (xdfVolume_) {
        if (!staging_->flush() || !staging_->seek(xdfProtectedOffset_)) {
            if (error) *error = QStringLiteral("The XDF compatibility area could not be rechecked.");
            return false;
        }
        const QByteArray protectedAfter = staging_->read(xdfProtectedBytes_.size());
        if (protectedAfter != xdfProtectedBytes_) {
            if (error) *error = QStringLiteral("The XDF compatibility image changed in the staged update.");
            return false;
        }
    }
    if (vhdx_ && !vhdx_->finish(error)) return false;
    if (vhd_ && !vhd_->finish(error)) return false;
    if (vmdk_ && !vmdk_->finish(error)) return false;
    if (splitVmdk_) {
        vhdx_.reset();
        vhd_.reset();
        vmdk_.reset();
        staging_.reset();
        imagePath_.clear();
        originalSha256_.clear();
        originalSize_ = 0;
        imageLogicalSize_ = 0;
        fatVolumeOffset_ = 0;
        fatVolumeSize_ = 0;
        fatSectorSize_ = kSectorSize;
        xdfVolume_ = false;
        ntfsVolume_ = false;
        extVolume_ = false;
        ntfsUpdate_.reset();
        extUpdate_.reset();
        extContext_.reset();
        extJournalContext_.reset();
        xdfProtectedOffset_ = 0;
        xdfProtectedBytes_.clear();
        splitVmdk_ = false;
        createMode_ = false;
        externalWriteGuard_.reset();
        updateLock_.reset();
        return true;
    }
    if (!staging_->flush() || !staging_->seek(0)) {
        if (error) *error = staging_->errorString();
        return false;
    }
    if (vmdkSplitCreate_) {
        const QFileInfo outputInfo(imagePath_);
        const QString extentName = QStringLiteral("peare-%1-s001.vmdk")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString extentPath = outputInfo.absoluteDir().filePath(extentName);
        QTemporaryFile extentTemp(outputInfo.absoluteDir().filePath(
            QStringLiteral(".peare-vmdk-XXXXXX")));
        if (!extentTemp.open() ||
            !initializeExternalSparseVmdk(&extentTemp, imageLogicalSize_, error)) {
            if (error && error->isEmpty()) *error = extentTemp.errorString();
            return false;
        }
        VmdkImageWriter extentWriter(&extentTemp, true, true, error);
        if (!extentWriter.valid()) return false;
        QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
        qint64 offset = 0;
        while (offset < imageLogicalSize_) {
            const qint64 count = qMin<qint64>(buffer.size(), imageLogicalSize_ - offset);
            if (staging_->read(buffer.data(), count) != count ||
                !extentWriter.write(offset, buffer.constData(), count)) {
                if (error) *error = staging_->errorString().isEmpty()
                    ? QStringLiteral("Could not encode data into the split sparse VMDK extent.")
                    : staging_->errorString();
                return false;
            }
            offset += count;
        }
        if (!extentWriter.finish(error) || !extentTemp.flush()) {
            if (error && error->isEmpty()) *error = extentTemp.errorString();
            return false;
        }
        extentTemp.seek(0);
        VmdkImageWriter extentReader(&extentTemp, false, true, error);
        if (!extentReader.valid() || !staging_->seek(0)) return false;
        offset = 0;
        while (offset < imageLogicalSize_) {
            const qint64 count = qMin<qint64>(buffer.size(), imageLogicalSize_ - offset);
            QByteArray expected(int(count), Qt::Uninitialized);
            QByteArray actual(int(count), Qt::Uninitialized);
            if (staging_->read(expected.data(), count) != count ||
                !extentReader.read(offset, actual.data(), count) || expected != actual) {
                if (error) *error = QStringLiteral("Split sparse VMDK extent verification failed.");
                return false;
            }
            offset += count;
        }
        if (!syncFileToDisk(&extentTemp, error)) return false;
        extentTemp.setAutoRemove(false);
        extentTemp.close();
        if (!extentTemp.rename(extentPath)) {
            extentTemp.setAutoRemove(true);
            if (error) *error = QStringLiteral("Could not publish the split sparse VMDK extent: %1")
                .arg(extentTemp.errorString());
            return false;
        }
        const QByteArray descriptor = makeSplitVmdkDescriptor(imageLogicalSize_, extentName);
        QSaveFile output(imagePath_);
        if (!output.open(QIODevice::WriteOnly) ||
            !writeAll(&output, descriptor.constData(), descriptor.size()) ||
            !syncFileToDisk(&output, error) || !output.commit()) {
            const QString message = output.errorString();
            QFile::remove(extentPath);
            if (error && error->isEmpty()) *error = message;
            return false;
        }
        externalWriteGuard_.reset();
        staging_.reset();
        imagePath_.clear();
        originalSha256_.clear();
        originalSize_ = 0;
        imageLogicalSize_ = 0;
        fatVolumeOffset_ = 0;
        fatVolumeSize_ = 0;
        fatSectorSize_ = kSectorSize;
        createMode_ = false;
        createTargetExisted_ = false;
        vmdkSplitCreate_ = false;
        updateLock_.reset();
        return true;
    }
    bool published = false;
#ifdef Q_OS_WIN
    if (!createMode_) {
        OverlayImageFile* overlay = dynamic_cast<OverlayImageFile*>(staging_.get());
        if (overlay) {
            if (!syncFileToDisk(overlay, error) ||
                !commitImageOverlay(overlay, imagePath_, externalWriteGuard_.get(), error))
                return false;
            published = true;
        }
    }
#endif
    if (!published) {
        QSaveFile output(imagePath_);
        if (!output.open(QIODevice::WriteOnly)) {
            if (error) *error = output.errorString();
            return false;
        }
        std::vector<char> buffer(static_cast<size_t>(kCopyChunkSize));
        while (true) {
            const qint64 count = staging_->read(buffer.data(), qint64(buffer.size()));
            if (count < 0) {
                if (error) *error = staging_->errorString();
                return false;
            }
            if (count == 0) break;
            if (!writeAll(&output, buffer.data(), count)) {
                if (error) *error = output.errorString();
                return false;
            }
        }
        if (!syncFileToDisk(&output, error)) return false;
        externalWriteGuard_.reset();
        if (!output.commit()) {
            if (error) *error = output.errorString();
            return false;
        }
    }
    externalWriteGuard_.reset();
    vhdx_.reset();
    vhd_.reset();
    vmdk_.reset();
    splitVmdk_ = false;
    staging_.reset();
    imagePath_.clear();
    originalSha256_.clear();
    originalSize_ = 0;
    imageLogicalSize_ = 0;
    fatVolumeOffset_ = 0;
    fatVolumeSize_ = 0;
    fatSectorSize_ = kSectorSize;
    xdfVolume_ = false;
    ntfsVolume_ = false;
    extVolume_ = false;
    ntfsUpdate_.reset();
    extUpdate_.reset();
    extContext_.reset();
    xdfProtectedOffset_ = 0;
    xdfProtectedBytes_.clear();
    createMode_ = false;
    createTargetExisted_ = false;
    vmdkSplitCreate_ = false;
    updateLock_.reset();
    return true;
}

void DiskImageUpdate::abort() {
    if (ntfsUpdate_) ntfsUpdate_->abort();
    if (extUpdate_) extUpdate_->abort();
    ntfsUpdate_.reset();
    extUpdate_.reset();
    ntfsContext_.reset();
    extContext_.reset();
    extJournalContext_.reset();
    vhdx_.reset();
    vhd_.reset();
    vmdk_.reset();
    splitVmdk_ = false;
    staging_.reset();
    externalWriteGuard_.reset();
    updateLock_.reset();
    imagePath_.clear();
    originalSha256_.clear();
    originalSize_ = 0;
    imageLogicalSize_ = 0;
    fatVolumeOffset_ = 0;
    fatVolumeSize_ = 0;
    fatSectorSize_ = kSectorSize;
    xdfVolume_ = false;
    ntfsVolume_ = false;
    extVolume_ = false;
    ntfsUpdate_.reset();
    extUpdate_.reset();
    xdfProtectedOffset_ = 0;
    xdfProtectedBytes_.clear();
    createMode_ = false;
    createTargetExisted_ = false;
    vmdkSplitCreate_ = false;
    extJournalOffset_ = 0;
    extJournalSize_ = 0;
}

}  // namespace writer
}  // namespace peare
