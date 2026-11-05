#include "VhdxImageWriter.h"

#include <QDebug>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace peare {
namespace writer {
namespace {

const qint64 kMiB = 1024 * 1024;
const qint64 kRegionTableOffset1 = 192 * 1024;
const qint64 kRegionTableOffset2 = 256 * 1024;
const qint64 kRegionTableSize = 64 * 1024;
const quint64 kBatStatusMask = 7;

const unsigned char kBatGuid[16] =
    {0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42, 0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
const unsigned char kMetadataGuid[16] =
    {0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b, 0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
const unsigned char kFileParametersGuid[16] =
    {0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d, 0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
const unsigned char kDiskSizeGuid[16] =
    {0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48, 0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
const unsigned char kLogicalSectorGuid[16] =
    {0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47, 0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
const unsigned char kVirtualDiskIdGuid[16] =
    {0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45, 0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46};
const unsigned char kPhysicalSectorGuid[16] =
    {0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44, 0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};
const unsigned char kParentLocatorGuid[16] =
    {0x2d, 0x5f, 0xd3, 0xa8, 0x0b, 0xb3, 0x4d, 0x45, 0xab, 0xf7, 0xd3, 0xd8, 0x48, 0x34, 0xab, 0x0c};

quint32 crc32c(const char* data, qint64 length) {
    quint32 crc = 0xffffffffU;
    for (qint64 i = 0; i < length; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0U);
    }
    return ~crc;
}

bool hasGuid(const char* data, const unsigned char guid[16]) {
    return std::memcmp(data, guid, 16) == 0;
}

bool validCrcBlock(QByteArray* bytes) {
    if (!bytes || bytes->size() < 8) return false;
    const quint32 expected = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(bytes->constData() + 4));
    std::memset(bytes->data() + 4, 0, 4);
    return crc32c(bytes->constData(), bytes->size()) == expected;
}

}  // namespace

VhdxImageWriter::VhdxImageWriter(QFile* file, bool writable, QString* error)
    : file_(file), writable_(writable) {
    valid_ = parse(error);
}

bool VhdxImageWriter::readFile(qint64 offset, char* data, qint64 length) const {
    if (!file_ || offset < 0 || length < 0 || offset > file_->size() ||
        length > file_->size() - offset || !file_->seek(offset)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 got = file_->read(data + done, length - done);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}

bool VhdxImageWriter::writeFile(qint64 offset, const char* data, qint64 length) {
    if (!file_ || !writable_ || offset < 0 || length < 0 || !file_->seek(offset))
        return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 wrote = file_->write(data + done, length - done);
        if (wrote <= 0) return false;
        done += wrote;
    }
    return true;
}

bool VhdxImageWriter::parse(QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!file_ || file_->size() < kRegionTableOffset2 + kRegionTableSize)
        return fail(QStringLiteral("Truncated VHDX image."));
    char signature[8];
    if (!readFile(0, signature, sizeof(signature)) || std::memcmp(signature, "vhdxfile", 8))
        return fail(QStringLiteral("Invalid VHDX signature."));

    auto headerValid = [&](qint64 offset, quint64* sequence, bool* hasLog,
                           quint64* logOffset, quint32* logLength) {
        QByteArray header(4096, Qt::Uninitialized);
        if (!readFile(offset, header.data(), header.size()) ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData())) != 0x64616568U)
            return false;
        const quint32 expected = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 4));
        std::memset(header.data() + 4, 0, 4);
        if (crc32c(header.constData(), header.size()) != expected ||
            qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.constData() + 66)) != 1)
            return false;
        *sequence = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 8));
        quint8 logGuid = 0;
        for (int i = 48; i < 64; ++i) logGuid |= quint8(header.at(i));
        *hasLog = logGuid != 0;
        *logLength = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData() + 68));
        *logOffset = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(header.constData() + 72));
        return true;
    };
    quint64 sequence1 = 0, sequence2 = 0;
    quint64 logOffset1 = 0, logOffset2 = 0;
    quint32 logLength1 = 0, logLength2 = 0;
    bool hasLog1 = false, hasLog2 = false;
    const bool header1 = headerValid(64 * 1024, &sequence1, &hasLog1,
                                     &logOffset1, &logLength1);
    const bool header2 = headerValid(128 * 1024, &sequence2, &hasLog2,
                                     &logOffset2, &logLength2);
    if (!header1 && !header2) return fail(QStringLiteral("No valid VHDX header."));
    if (header1 && header2 && sequence1 == sequence2)
        return fail(QStringLiteral("VHDX headers have an ambiguous sequence number."));
    const bool useHeader2 = header2 && (!header1 || sequence2 > sequence1);
    activeHeaderOffset_ = useHeader2 ? 128 * 1024 : 64 * 1024;
    activeHeaderSequence_ = useHeader2 ? sequence2 : sequence1;
    const quint64 activeLogOffset = useHeader2 ? logOffset2 : logOffset1;
    const quint32 activeLogLength = useHeader2 ? logLength2 : logLength1;
    if (activeLogLength == 0) {
        if (activeLogOffset != 0)
            return fail(QStringLiteral("VHDX log offset is set but its length is zero."));
    } else if (activeLogLength % kMiB != 0 || activeLogOffset < quint64(kMiB) ||
               activeLogOffset % quint64(kMiB) != 0 ||
               activeLogOffset > quint64(file_->size()) ||
               activeLogLength > quint64(file_->size()) - activeLogOffset) {
        return fail(QStringLiteral("VHDX log extent is invalid."));
    }
    logOffset_ = qint64(activeLogOffset);
    logLength_ = activeLogLength;
    if (useHeader2 ? hasLog2 : hasLog1)
        return fail(QStringLiteral("VHDX log replay is required and is not supported."));

    qint64 metadataOffset = 0, metadataLength = 0;
    bool haveBat = false, haveMetadata = false;
    std::vector<std::pair<qint64, qint64>> regionExtents;
    auto readRegions = [&](qint64 offset) {
        QByteArray table(int(kRegionTableSize), Qt::Uninitialized);
        if (!readFile(offset, table.data(), table.size()) ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(table.constData())) != 0x69676572U ||
            !validCrcBlock(&table)) return false;
        const quint32 count = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(table.constData() + 8));
        const quint32 reserved = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(table.constData() + 12));
        if (count > 2047 || reserved != 0) return false;
        qint64 candidateBatOffset = 0, candidateBatLength = 0;
        qint64 candidateMetadataOffset = 0, candidateMetadataLength = 0;
        bool foundBat = false, foundMetadata = false;
        std::set<std::string> seenGuids;
        std::vector<std::pair<quint64, quint64>> candidateExtents;
        for (quint32 i = 0; i < count; ++i) {
            const char* entry = table.constData() + 16 + qint64(i) * 32;
            if (!seenGuids.insert(std::string(entry, 16)).second) return false;
            const quint64 regionOffset = qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar*>(entry + 16));
            const quint32 regionLength = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(entry + 24));
            const quint32 required = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(entry + 28));
            const bool isBat = hasGuid(entry, kBatGuid);
            const bool isMetadata = hasGuid(entry, kMetadataGuid);
            if (required > 1 || regionOffset < quint64(kMiB) ||
                regionOffset % quint64(kMiB) != 0 || regionLength == 0 ||
                regionOffset > quint64(file_->size()) ||
                regionLength > quint64(file_->size()) - regionOffset) return false;
            if (!isBat && !isMetadata && required)
                return false;
            if (isBat) {
                candidateBatOffset = qint64(regionOffset);
                candidateBatLength = regionLength;
                foundBat = true;
            } else if (isMetadata) {
                candidateMetadataOffset = qint64(regionOffset);
                candidateMetadataLength = regionLength;
                foundMetadata = true;
            }
            candidateExtents.push_back(std::make_pair(regionOffset, regionLength));
        }
        if (!foundBat || !foundMetadata) return false;
        std::sort(candidateExtents.begin(), candidateExtents.end());
        for (size_t i = 1; i < candidateExtents.size(); ++i) {
            const quint64 previousEnd = candidateExtents[i - 1].first + candidateExtents[i - 1].second;
            if (candidateExtents[i].first < previousEnd) return false;
        }
        if (activeLogLength) {
            for (const auto& extent : candidateExtents) {
                const quint64 extentEnd = extent.first + extent.second;
                const quint64 logEnd = activeLogOffset + activeLogLength;
                if (activeLogOffset < extentEnd && extent.first < logEnd) return false;
            }
        }
        batOffset_ = candidateBatOffset;
        batLength_ = candidateBatLength;
        metadataOffset = candidateMetadataOffset;
        metadataLength = candidateMetadataLength;
        regionExtents.clear();
        for (const auto& extent : candidateExtents)
            regionExtents.push_back(std::make_pair(qint64(extent.first), qint64(extent.second)));
        return true;
    };
    QByteArray regionTable1(int(kRegionTableSize), Qt::Uninitialized);
    QByteArray regionTable2(int(kRegionTableSize), Qt::Uninitialized);
    if (!readFile(kRegionTableOffset1, regionTable1.data(), regionTable1.size()) ||
        !readFile(kRegionTableOffset2, regionTable2.data(), regionTable2.size()) ||
        regionTable1 != regionTable2)
        return fail(QStringLiteral("VHDX region table copies are missing or disagree."));
    if (!readRegions(kRegionTableOffset1))
        return fail(QStringLiteral("No valid VHDX region table."));
    haveBat = batOffset_ > 0 && batLength_ > 0;
    haveMetadata = metadataOffset > 0 && metadataLength >= 64 * 1024;
    if (!haveBat || !haveMetadata) return fail(QStringLiteral("Missing VHDX regions."));

    QByteArray metadata(64 * 1024, Qt::Uninitialized);
    if (!readFile(metadataOffset, metadata.data(), metadata.size()) ||
        qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(metadata.constData())) != 0x617461646174656dULL)
        return fail(QStringLiteral("Invalid VHDX metadata table."));
    const quint16 metadataCount = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(metadata.constData() + 10));
    if (metadataCount > (metadata.size() - 32) / 32 ||
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(metadata.constData() + 8)) != 0)
        return fail(QStringLiteral("Invalid VHDX metadata count or reserved header fields."));
    for (int i = 12; i < 32; ++i)
        if (metadata.at(i) != 0) return fail(QStringLiteral("Invalid VHDX metadata reserved fields."));
    bool haveFileParameters = false, haveDiskSize = false, haveVirtualDiskId = false;
    bool haveLogicalSector = false, havePhysicalSector = false;
    quint32 logicalSectorSize = 0, physicalSectorSize = 0;
    std::set<std::string> seenMetadata;
    std::vector<std::pair<quint64, quint64>> metadataExtents;
    for (quint16 i = 0; i < metadataCount; ++i) {
        const char* entry = metadata.constData() + 32 + qint64(i) * 32;
        const quint32 itemOffset = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(entry + 16));
        const quint32 itemLength = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(entry + 20));
        const quint32 itemFlags = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(entry + 24));
        const quint32 itemReserved = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(entry + 28));
        std::string itemKey(entry, 16);
        if (!seenMetadata.insert(itemKey).second || (itemFlags & ~7U) || itemReserved)
            return fail(QStringLiteral("Invalid or duplicate VHDX metadata entry."));
        const bool knownItem = hasGuid(entry, kFileParametersGuid) ||
            hasGuid(entry, kDiskSizeGuid) || hasGuid(entry, kVirtualDiskIdGuid) ||
            hasGuid(entry, kLogicalSectorGuid) || hasGuid(entry, kPhysicalSectorGuid) ||
            hasGuid(entry, kParentLocatorGuid);
        if (!knownItem && (itemFlags & 4U))
            return fail(QStringLiteral("VHDX contains an unsupported required metadata item."));
        if (itemLength == 0) {
            if (itemOffset != 0)
                return fail(QStringLiteral("Empty VHDX metadata item has a nonzero offset."));
            if (hasGuid(entry, kParentLocatorGuid)) {
                if (itemFlags != 4U)
                    return fail(QStringLiteral("Invalid VHDX parent locator metadata flags."));
            }
            continue;
        }
        if (itemOffset < 64 * 1024 || itemLength > kMiB ||
            itemOffset > quint64(metadataLength) || itemLength > quint64(metadataLength) - itemOffset)
            return fail(QStringLiteral("VHDX metadata item is out of bounds."));
        metadataExtents.push_back(std::make_pair(itemOffset, itemLength));
        QByteArray itemBytes(int(itemLength), Qt::Uninitialized);
        if (!readFile(metadataOffset + itemOffset, itemBytes.data(), itemBytes.size()))
            return fail(QStringLiteral("Cannot read VHDX metadata item."));
        const char* item = itemBytes.constData();
        if (hasGuid(entry, kFileParametersGuid)) {
            if (itemLength != 8 || itemFlags != 4U)
                return fail(QStringLiteral("Invalid required VHDX file parameters metadata."));
            blockSize_ = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(item));
            const quint32 flags = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(item + 4));
            if (flags & 2U) return fail(QStringLiteral("Differencing VHDX images are not supported."));
            if (flags & ~3U) return fail(QStringLiteral("Invalid VHDX file parameter flags."));
            haveFileParameters = true;
        } else if (hasGuid(entry, kDiskSizeGuid)) {
            if (itemLength != 8 || itemFlags != 6U)
                return fail(QStringLiteral("Invalid required VHDX virtual disk size metadata."));
            const quint64 value = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(item));
            if (value > quint64((std::numeric_limits<qint64>::max)()))
                return fail(QStringLiteral("Invalid VHDX disk size (%1).")
                            .arg(QString::number(value)));
            diskSize_ = qint64(value);
            haveDiskSize = true;
        } else if (hasGuid(entry, kVirtualDiskIdGuid)) {
            if (itemLength != 16 || itemFlags != 6U)
                return fail(QStringLiteral("Invalid required VHDX virtual disk ID metadata."));
            quint8 any = 0;
            for (int b = 0; b < itemLength; ++b) any |= quint8(item[b]);
            if (!any) return fail(QStringLiteral("VHDX virtual disk ID is zero."));
            haveVirtualDiskId = true;
        } else if (hasGuid(entry, kLogicalSectorGuid)) {
            if (itemLength != 4 || itemFlags != 6U)
                return fail(QStringLiteral("Invalid required VHDX logical sector metadata."));
            logicalSectorSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(item));
            haveLogicalSector = true;
        } else if (hasGuid(entry, kPhysicalSectorGuid)) {
            if (itemLength != 4 || itemFlags != 6U)
                return fail(QStringLiteral("Invalid required VHDX physical sector metadata."));
            physicalSectorSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(item));
            havePhysicalSector = true;
        } else if (hasGuid(entry, kParentLocatorGuid)) {
            if (itemFlags != 4U || itemLength != 0)
                return fail(QStringLiteral("VHDX parent locators are not supported."));
        }
    }
    std::sort(metadataExtents.begin(), metadataExtents.end());
    for (size_t i = 1; i < metadataExtents.size(); ++i) {
        const quint64 previousEnd = metadataExtents[i - 1].first + metadataExtents[i - 1].second;
        if (metadataExtents[i].first < previousEnd)
            return fail(QStringLiteral("VHDX metadata items overlap."));
    }
    if (!haveFileParameters || !haveDiskSize || !haveVirtualDiskId ||
        !haveLogicalSector || !havePhysicalSector || diskSize_ <= 0 ||
        quint64(diskSize_) > (64ULL * 1024 * 1024 * 1024 * 1024) ||
        (logicalSectorSize != 512 && logicalSectorSize != 4096) ||
        (physicalSectorSize != 512 && physicalSectorSize != 4096) ||
        physicalSectorSize < logicalSectorSize || diskSize_ % logicalSectorSize != 0 ||
        blockSize_ < kMiB || blockSize_ > 256 * kMiB ||
        (blockSize_ & (blockSize_ - 1)) != 0)
        return fail(QStringLiteral("Unsupported VHDX geometry or required metadata: disk=%1, block=%2, sector=%3/%4, metadata=%5/%6/%7/%8/%9.")
                    .arg(QString::number(diskSize_)).arg(QString::number(blockSize_))
                    .arg(QString::number(logicalSectorSize)).arg(QString::number(physicalSectorSize))
                    .arg(haveFileParameters).arg(haveDiskSize).arg(haveVirtualDiskId)
                    .arg(haveLogicalSector).arg(havePhysicalSector));

    logicalSectorSize_ = logicalSectorSize;

    const qint64 dataBlocks = (diskSize_ - 1) / blockSize_ + 1;
    const qint64 chunkSize = (1LL << 23) * logicalSectorSize;
    chunkRatio_ = chunkSize / blockSize_;
    if (dataBlocks <= 0 || chunkRatio_ <= 0) return fail(QStringLiteral("Invalid VHDX BAT geometry."));
    const qint64 bitmapBlocks = (dataBlocks - 1) / chunkRatio_;
    if (bitmapBlocks > (std::numeric_limits<qint64>::max)() - dataBlocks)
        return fail(QStringLiteral("VHDX BAT entry count overflows."));
    const qint64 totalEntries = dataBlocks + bitmapBlocks;
    if (totalEntries > 16LL * 1024 * 1024 || totalEntries > batLength_ / 8 ||
        totalEntries > file_->size() / 8 || totalEntries > (std::numeric_limits<int>::max)() / 8)
        return fail(QStringLiteral("Truncated VHDX BAT."));
    QByteArray rawBat(int(totalEntries * 8), Qt::Uninitialized);
    if (!readFile(batOffset_, rawBat.data(), rawBat.size()))
        return fail(QStringLiteral("Cannot read VHDX BAT."));
    bat_.resize(static_cast<std::size_t>(dataBlocks));
    std::vector<std::pair<quint64, quint64>> payloadExtents;
    for (qint64 block = 0; block < dataBlocks; ++block) {
        const qint64 chunk = block / chunkRatio_;
        const qint64 blockInChunk = block % chunkRatio_;
        const qint64 batIndex = chunk * (chunkRatio_ + 1) + blockInChunk;
        const quint64 entry = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(rawBat.constData() + batIndex * 8));
        const quint64 status = entry & kBatStatusMask;
        if ((entry & 0x00000000000ffff8ULL) || status == 4 || status == 5 || status == 7)
            return fail(QStringLiteral("VHDX contains reserved or partially-present blocks."));
        const quint64 hostOffset = ((entry >> 20) & 0xFFFFFFFFFFFULL) * quint64(kMiB);
        const bool hasPayloadExtent = status == 6 ||
            ((status == 1 || status == 3) && hostOffset != 0);
        if ((status == 0 || status == 2) && hostOffset != 0)
            return fail(QStringLiteral("VHDX unallocated block has a reserved file offset."));
        if (hasPayloadExtent) {
            if (hostOffset < quint64(kMiB) || hostOffset % quint64(kMiB) != 0 ||
                hostOffset > quint64(file_->size()) ||
                quint64(blockSize_) > quint64(file_->size()) - hostOffset)
                return fail(status == 6
                    ? QStringLiteral("VHDX block extent is out of bounds.")
                    : QStringLiteral("VHDX retained block extent is out of bounds."));
            const quint64 hostEnd = hostOffset + quint64(blockSize_);
            for (const auto& extent : regionExtents) {
                const quint64 extentStart = quint64(extent.first);
                const quint64 extentEnd = extentStart + quint64(extent.second);
                if (hostOffset < extentEnd && extentStart < hostEnd)
                    return fail(QStringLiteral("VHDX payload overlaps a metadata region."));
            }
            if (logLength_ && hostOffset < quint64(logOffset_ + logLength_) &&
                quint64(logOffset_) < hostEnd)
                return fail(QStringLiteral("VHDX payload overlaps the log region."));
            payloadExtents.push_back(std::make_pair(hostOffset, hostEnd));
        }
        bat_[static_cast<std::size_t>(block)] = entry;
    }
    std::sort(payloadExtents.begin(), payloadExtents.end());
    for (size_t i = 1; i < payloadExtents.size(); ++i)
        if (payloadExtents[i].first < payloadExtents[i - 1].second)
            return fail(QStringLiteral("VHDX payload blocks overlap."));
    for (qint64 chunk = 0; chunk < bitmapBlocks; ++chunk) {
        const qint64 bitmapIndex = chunk * (chunkRatio_ + 1) + chunkRatio_;
        const quint64 entry = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(rawBat.constData() + bitmapIndex * 8));
        if (entry != 0)
            return fail(QStringLiteral("VHDX sector bitmap blocks are not supported for fixed/dynamic disks."));
    }
    valid_ = true;
    return true;
}

qint64 VhdxImageWriter::batEntryOffset(qint64 block) const {
    const qint64 chunk = block / chunkRatio_;
    const qint64 blockInChunk = block % chunkRatio_;
    return batOffset_ + (chunk * (chunkRatio_ + 1) + blockInChunk) * 8;
}

bool VhdxImageWriter::read(qint64 offset, char* data, qint64 length) const {
    if (!valid_ || !data || offset < 0 || length < 0 || offset > diskSize_ ||
        length > diskSize_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 block = position / blockSize_;
        const qint64 inBlock = position % blockSize_;
        const qint64 count = std::min(length - done, blockSize_ - inBlock);
        const auto changed = changedBlocks_.find(block);
        if (changed != changedBlocks_.end()) {
            std::memcpy(data + done, changed->second.data() + inBlock, static_cast<std::size_t>(count));
        } else if ((bat_[static_cast<std::size_t>(block)] & kBatStatusMask) == 6) {
            const quint64 host = ((bat_[static_cast<std::size_t>(block)] >> 20) & 0xFFFFFFFFFFFULL) * quint64(kMiB);
            if (!readFile(qint64(host) + inBlock, data + done, count)) return false;
        } else {
            std::memset(data + done, 0, static_cast<std::size_t>(count));
        }
        done += count;
    }
    return true;
}

bool VhdxImageWriter::write(qint64 offset, const char* data, qint64 length) {
    if (!valid_ || !writable_ || !data || offset < 0 || length < 0 ||
        offset > diskSize_ || length > diskSize_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 block = position / blockSize_;
        const qint64 inBlock = position % blockSize_;
        const qint64 count = std::min(length - done, blockSize_ - inBlock);
        if ((bat_[static_cast<std::size_t>(block)] & kBatStatusMask) == 6) {
            const quint64 host = ((bat_[static_cast<std::size_t>(block)] >> 20) & 0xFFFFFFFFFFFULL) * quint64(kMiB);
            if (!writeFile(qint64(host) + inBlock, data + done, count)) return false;
            done += count;
            continue;
        }
        auto changed = changedBlocks_.find(block);
        if (changed == changedBlocks_.end()) {
            std::vector<char> bytes(static_cast<std::size_t>(blockSize_), 0);
            const qint64 blockStart = block * blockSize_;
            const qint64 validLength = std::min(blockSize_, diskSize_ - blockStart);
            if (!read(blockStart, bytes.data(), validLength)) return false;
            changed = changedBlocks_.insert(std::make_pair(block, std::move(bytes))).first;
        }
        std::memcpy(changed->second.data() + inBlock, data + done, static_cast<std::size_t>(count));
        done += count;
    }
    return true;
}

bool VhdxImageWriter::finish(QString* error) {
    if (!valid_ || !writable_ || !file_) {
        if (error) *error = QStringLiteral("VHDX writer is not active.");
        return false;
    }
    qint64 appendOffset = ((file_->size() + kMiB - 1) / kMiB) * kMiB;
    QByteArray zeros(int(kMiB), '\0');
    for (const auto& changed : changedBlocks_) {
        const qint64 block = changed.first;
        if (appendOffset > file_->size()) {
            const qint64 padding = appendOffset - file_->size();
            if (!writeFile(file_->size(), zeros.constData(), padding)) {
                if (error) *error = file_->errorString();
                return false;
            }
        }
        if (!writeFile(appendOffset, changed.second.data(), blockSize_)) {
            if (error) *error = file_->errorString();
            return false;
        }
        const quint64 hostMiB = quint64(appendOffset / kMiB);
        if (hostMiB > 0xFFFFFFFFFFFULL) {
            if (error) *error = QStringLiteral("VHDX allocation offset exceeds BAT limits.");
            return false;
        }
        const quint64 newEntry = (hostMiB << 20) | 6U;
        char encoded[8];
        qToLittleEndian<quint64>(newEntry, reinterpret_cast<uchar*>(encoded));
        if (!writeFile(batEntryOffset(block), encoded, sizeof(encoded))) {
            if (error) *error = file_->errorString();
            return false;
        }
        bat_[static_cast<std::size_t>(block)] = newEntry;
        appendOffset += blockSize_;
    }
    if (!changedBlocks_.empty()) {
        if (activeHeaderSequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
            if (error) *error = QStringLiteral("VHDX header sequence number cannot be advanced.");
            return false;
        }
        QByteArray header(4096, Qt::Uninitialized);
        if (!readFile(activeHeaderOffset_, header.data(), header.size())) {
            if (error) *error = QStringLiteral("Cannot read the active VHDX header.");
            return false;
        }
        const quint64 nextSequence = activeHeaderSequence_ + 1;
        qToLittleEndian<quint64>(nextSequence, reinterpret_cast<uchar*>(header.data() + 8));
        const auto writeGuid = [&](char* destination, const QUuid& guid) {
            qToLittleEndian<quint32>(guid.data1, reinterpret_cast<uchar*>(destination));
            qToLittleEndian<quint16>(guid.data2, reinterpret_cast<uchar*>(destination + 4));
            qToLittleEndian<quint16>(guid.data3, reinterpret_cast<uchar*>(destination + 6));
            std::memcpy(destination + 8, guid.data4, 8);
        };
        writeGuid(header.data() + 16, QUuid::createUuid());
        writeGuid(header.data() + 32, QUuid::createUuid());
        std::memset(header.data() + 4, 0, 4);
        qToLittleEndian<quint32>(crc32c(header.constData(), header.size()),
                                 reinterpret_cast<uchar*>(header.data() + 4));
        if (!writeFile(activeHeaderOffset_, header.constData(), header.size())) {
            if (error) *error = file_->errorString();
            return false;
        }
        activeHeaderSequence_ = nextSequence;
    }
    if (!file_->flush()) {
        if (error) *error = file_->errorString();
        return false;
    }
    changedBlocks_.clear();
    return true;
}

}  // namespace writer
}  // namespace peare
