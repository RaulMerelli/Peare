#include "VmdkStreamOptimizedWriter.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>

#include "../../common/compression/DeflateDecoder.h"
#include "VmdkImageWriter.h"
#include "../third_party/miniz/miniz.h"

namespace peare {
namespace writer {
namespace {

constexpr qint64 kSectorSize = 512;
constexpr quint32 kSparseMagic = 0x564d444b;
constexpr qint64 kMaxGrains = 4 * 1024 * 1024;
constexpr qint64 kMaxMetadataBytes = 256 * 1024 * 1024;

qint64 alignTo(qint64 value, qint64 alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

bool readAt(QFile* file, qint64 offset, char* data, qint64 length) {
    if (!file || offset < 0 || length < 0 || !file->seek(offset)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 got = file->read(data + done, length - done);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}

bool writeAt(QFile* file, qint64 offset, const char* data, qint64 length) {
    if (!file || offset < 0 || length < 0 || !file->seek(offset)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 put = file->write(data + done, length - done);
        if (put <= 0) return false;
        done += put;
    }
    return true;
}

QString failure(const QString& detail) {
    return QStringLiteral("Invalid or unsupported streamOptimized VMDK: %1").arg(detail);
}

bool writeExact(QFile* file, const char* data, qint64 length) {
    qint64 done = 0;
    while (done < length) {
        const qint64 written = file->write(data + done, length - done);
        if (written <= 0) return false;
        done += written;
    }
    return true;
}

bool writeZeros(QFile* file, qint64 length) {
    static const QByteArray zeros(4096, '\0');
    while (length > 0) {
        const qint64 count = qMin<qint64>(length, zeros.size());
        if (!writeExact(file, zeros.constData(), count)) return false;
        length -= count;
    }
    return true;
}

QByteArray sparseHeader(quint64 capacitySectors, quint64 grainSectors,
                        quint64 descriptorSectors, quint64 grainDirectorySector,
                        quint64 overheadSectors) {
    QByteArray header(kSectorSize, '\0');
    qToLittleEndian<quint32>(kSparseMagic, reinterpret_cast<uchar*>(header.data()));
    qToLittleEndian<quint32>(3, reinterpret_cast<uchar*>(header.data() + 4));
    qToLittleEndian<quint32>(0x00030001U, reinterpret_cast<uchar*>(header.data() + 8));
    qToLittleEndian<quint64>(capacitySectors, reinterpret_cast<uchar*>(header.data() + 12));
    qToLittleEndian<quint64>(grainSectors, reinterpret_cast<uchar*>(header.data() + 20));
    qToLittleEndian<quint64>(1, reinterpret_cast<uchar*>(header.data() + 28));
    qToLittleEndian<quint64>(descriptorSectors, reinterpret_cast<uchar*>(header.data() + 36));
    qToLittleEndian<quint32>(512, reinterpret_cast<uchar*>(header.data() + 44));
    qToLittleEndian<quint64>(0, reinterpret_cast<uchar*>(header.data() + 48));
    qToLittleEndian<quint64>(grainDirectorySector, reinterpret_cast<uchar*>(header.data() + 56));
    qToLittleEndian<quint64>(overheadSectors, reinterpret_cast<uchar*>(header.data() + 64));
    std::memcpy(header.data() + 73, "\n \r\n", 4);
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(header.data() + 77));
    return header;
}

QByteArray metadataMarker(quint64 sectors, quint32 type) {
    QByteArray marker(kSectorSize, '\0');
    qToLittleEndian<quint64>(sectors, reinterpret_cast<uchar*>(marker.data()));
    qToLittleEndian<quint32>(type, reinterpret_cast<uchar*>(marker.data() + 12));
    return marker;
}

}  // namespace

bool VmdkStreamOptimizedWriter::convertSparseToStreamOptimized(
    QFile* file, const QString& outputName, QString* error) {
    auto reject = [&](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!file || !file->isOpen() || !file->isReadable() ||
        outputName.isEmpty() || outputName.contains(QLatin1Char('"')) ||
        outputName.contains(QLatin1Char('\n')) || outputName.contains(QLatin1Char('\r')))
        return reject(QStringLiteral("Invalid streamOptimized VMDK destination."));

    VmdkImageWriter source(file, false, error);
    if (!source.valid()) return false;
    const quint64 capacitySectors = quint64(source.capacity() / kSectorSize);
    constexpr quint64 grainSectors = 128;
    constexpr quint64 grainBytes = grainSectors * kSectorSize;
    constexpr quint64 entriesPerTable = 512;
    const quint64 grainCount = (capacitySectors + grainSectors - 1) / grainSectors;
    const quint64 tableCount = (grainCount + entriesPerTable - 1) / entriesPerTable;
    if (!capacitySectors || grainCount > 4 * 1024 * 1024 || !tableCount || tableCount > 131072)
        return reject(QStringLiteral("VMDK geometry exceeds streamOptimized limits."));

    const QByteArray descriptor = QStringLiteral(
        "# Disk DescriptorFile\nversion=1\nencoding=\"UTF-8\"\n"
        "CID=fffffffe\nparentCID=ffffffff\ncreateType=\"streamOptimized\"\n\n"
        "# Extent description\nRW %1 SPARSE \"%2\"\n\n# The Disk Data Base\n#DDB\n"
        "ddb.adapterType = \"ide\"\n"
        "ddb.geometry.cylinders = \"%3\"\n"
        "ddb.geometry.heads = \"16\"\n"
        "ddb.geometry.sectors = \"63\"\n"
        "ddb.virtualHWVersion = \"13\"\n")
        .arg(QString::number(capacitySectors), outputName,
             QString::number(qMax<quint64>(1, capacitySectors / (16 * 63)))).toUtf8();
    constexpr quint64 descriptorSectors = 20;
    const quint64 overheadSectors = alignTo(1 + descriptorSectors, grainSectors);
    QTemporaryFile output;
    if (!output.open() || !output.resize(qint64(overheadSectors * kSectorSize)))
        return reject(QStringLiteral("Could not create streamOptimized staging image."));
    const QByteArray header = sparseHeader(capacitySectors, grainSectors,
        descriptorSectors, std::numeric_limits<quint64>::max(), overheadSectors);
    if (!writeAt(&output, 0, header.constData(), header.size()) ||
        !writeAt(&output, kSectorSize, descriptor.constData(), descriptor.size()) ||
        !output.seek(qint64(overheadSectors * kSectorSize)))
        return reject(QStringLiteral("Could not write streamOptimized header and descriptor."));

    std::vector<quint32> grainMarkerSectors(size_t(grainCount), 0);
    QByteArray grain(int(grainBytes), Qt::Uninitialized);
    QByteArray compressed(int(mz_compressBound(uLong(grainBytes))), Qt::Uninitialized);
    for (quint64 index = 0; index < grainCount; ++index) {
        const qint64 offset = qint64(index * grainBytes);
        const qint64 count = qMin<qint64>(qint64(grainBytes), source.capacity() - offset);
        std::memset(grain.data(), 0, size_t(grain.size()));
        if (!source.read(offset, grain.data(), count))
            return reject(QStringLiteral("Could not read sparse VMDK grain."));
        bool nonzero = false;
        for (char value : grain) if (value) { nonzero = true; break; }
        if (!nonzero) continue;
        uLongf compressedSize = uLongf(compressed.size());
        if (mz_compress2(reinterpret_cast<unsigned char*>(compressed.data()), &compressedSize,
                         reinterpret_cast<const unsigned char*>(grain.constData()),
                         uLong(grain.size()), 6) != MZ_OK || compressedSize == 0 ||
            compressedSize > std::numeric_limits<quint32>::max())
            return reject(QStringLiteral("Could not compress a VMDK grain."));
        const quint64 markerOffset = quint64(output.size());
        const quint64 markerSector = markerOffset / kSectorSize;
        if (markerOffset % kSectorSize || markerSector > std::numeric_limits<quint32>::max())
            return reject(QStringLiteral("StreamOptimized grain marker exceeds sector limits."));
        grainMarkerSectors[size_t(index)] = quint32(markerSector);
        QByteArray marker(12, '\0');
        qToLittleEndian<quint64>(index * grainSectors,
                                 reinterpret_cast<uchar*>(marker.data()));
        qToLittleEndian<quint32>(quint32(compressedSize),
                                 reinterpret_cast<uchar*>(marker.data() + 8));
        if (!writeExact(&output, marker.constData(), marker.size()) ||
            !writeExact(&output, compressed.constData(), qint64(compressedSize)))
            return reject(QStringLiteral("Could not write compressed VMDK grain."));
        const qint64 padding = alignTo(output.size(), kSectorSize) - output.size();
        if (!writeZeros(&output, padding))
            return reject(QStringLiteral("Could not align compressed VMDK grain."));
    }

    std::vector<quint32> grainTableSectors(static_cast<size_t>(tableCount), 0);
    for (quint64 table = 0; table < tableCount; ++table) {
        const quint64 firstGrain = table * entriesPerTable;
        const quint64 count = qMin<quint64>(entriesPerTable, grainCount - firstGrain);
        QByteArray grainTable(int(entriesPerTable * 4), '\0');
        for (quint64 entry = 0; entry < count; ++entry) {
            const quint32 sector = grainMarkerSectors[size_t(firstGrain + entry)];
            qToLittleEndian<quint32>(sector,
                reinterpret_cast<uchar*>(grainTable.data() + entry * 4));
        }
        const quint64 markerSector = quint64(output.size()) / kSectorSize;
        const quint64 tableSectors = grainTable.size() / kSectorSize;
        grainTableSectors[size_t(table)] = quint32(markerSector + 1);
        const QByteArray marker = metadataMarker(tableSectors, 1);
        if (!writeExact(&output, marker.constData(), marker.size()) ||
            !writeExact(&output, grainTable.constData(), grainTable.size()))
            return reject(QStringLiteral("Could not write VMDK grain table."));
    }

    const quint64 directorySector = quint64(output.size()) / kSectorSize + 1;
    const quint64 directoryBytes = alignTo(qint64(tableCount * 4), kSectorSize);
    QByteArray directory(int(directoryBytes), '\0');
    for (quint64 index = 0; index < tableCount; ++index)
        qToLittleEndian<quint32>(grainTableSectors[size_t(index)],
            reinterpret_cast<uchar*>(directory.data() + index * 4));
    const QByteArray directoryMarker = metadataMarker(directoryBytes / kSectorSize, 2);
    if (!writeExact(&output, directoryMarker.constData(), directoryMarker.size()) ||
        !writeExact(&output, directory.constData(), directory.size()))
        return reject(QStringLiteral("Could not write VMDK grain directory."));

    const QByteArray footerMarker = metadataMarker(1, 3);
    const QByteArray footer = sparseHeader(capacitySectors, grainSectors,
        descriptorSectors, directorySector, overheadSectors);
    const QByteArray endMarker = metadataMarker(0, 0);
    if (!writeExact(&output, footerMarker.constData(), footerMarker.size()) ||
        !writeExact(&output, footer.constData(), footer.size()) ||
        !writeExact(&output, endMarker.constData(), endMarker.size()) ||
        !output.flush() || !output.seek(0))
        return reject(QStringLiteral("Could not finalize streamOptimized VMDK."));

    VmdkStreamOptimizedWriter validation(&output, false, outputName, error);
    if (!validation.valid()) return false;
    QByteArray expected(1024 * 1024, Qt::Uninitialized);
    QByteArray actual(expected.size(), Qt::Uninitialized);
    for (qint64 offset = 0; offset < source.capacity();) {
        const qint64 count = qMin<qint64>(expected.size(), source.capacity() - offset);
        if (!source.read(offset, expected.data(), count) ||
            !validation.read(offset, actual.data(), count) ||
            std::memcmp(expected.constData(), actual.constData(), size_t(count)) != 0)
            return reject(QStringLiteral("StreamOptimized VMDK verification failed."));
        offset += count;
    }

    if (!output.seek(0) || !file->resize(0) || !file->seek(0))
        return reject(QStringLiteral("Could not replace staged VMDK image."));
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qint64 remaining = output.size();
    while (remaining > 0) {
        const qint64 count = output.read(buffer.data(), qMin<qint64>(buffer.size(), remaining));
        if (count <= 0 || !writeExact(file, buffer.constData(), count))
            return reject(QStringLiteral("Could not copy streamOptimized VMDK."));
        remaining -= count;
    }
    if (!file->flush()) return reject(QStringLiteral("Could not flush streamOptimized VMDK."));
    return true;
}

VmdkStreamOptimizedWriter::VmdkStreamOptimizedWriter(QFile* file, bool writable,
                                                     const QString& outputName,
                                                     QString* error)
    : file_(file), writable_(writable), outputName_(outputName) {
    valid_ = parse(error);
}

bool VmdkStreamOptimizedWriter::readFile(qint64 offset, char* data, qint64 length) const {
    return readAt(file_, offset, data, length);
}

bool VmdkStreamOptimizedWriter::parse(QString* error) {
    auto reject = [&](const QString& reason) {
        if (error) *error = failure(reason);
        return false;
    };
    if (!file_ || !file_->isOpen() || !file_->isReadable()) return reject(QStringLiteral("file is not readable"));
    const qint64 fileSize = file_->size();
    if (fileSize < kSectorSize) return reject(QStringLiteral("truncated header"));

    char header[kSectorSize];
    if (!readFile(0, header, sizeof(header)) || qFromLittleEndian<quint32>(header) != kSparseMagic)
        return reject(QStringLiteral("bad sparse magic"));
    const quint32 version = qFromLittleEndian<quint32>(header + 4);
    const quint32 flags = qFromLittleEndian<quint32>(header + 8);
    capacitySectors_ = qFromLittleEndian<quint64>(header + 12);
    grainSectors_ = qFromLittleEndian<quint64>(header + 20);
    descriptorOffset_ = qFromLittleEndian<quint64>(header + 28) * kSectorSize;
    descriptorLength_ = qFromLittleEndian<quint64>(header + 36) * kSectorSize;
    entriesPerTable_ = qFromLittleEndian<quint32>(header + 44);
    overheadBytes_ = qFromLittleEndian<quint64>(header + 64) * kSectorSize;
    const quint32 compression = qFromLittleEndian<quint16>(header + 77);

    if (version != 3 || compression != 1 || !(flags & 0x00020000u) || !(flags & 0x00010000u))
        return reject(QStringLiteral("requires version 3, compression, and embedded descriptor flags"));
    if (capacitySectors_ == 0 || capacitySectors_ > quint64(std::numeric_limits<qint64>::max() / kSectorSize) ||
        grainSectors_ == 0 || grainSectors_ > quint64(std::numeric_limits<qint64>::max() / kSectorSize) ||
        entriesPerTable_ == 0 || entriesPerTable_ > 1024 * 1024)
        return reject(QStringLiteral("invalid capacity or grain geometry"));
    capacityBytes_ = capacitySectors_ * kSectorSize;
    grainBytes_ = grainSectors_ * kSectorSize;
    if (grainBytes_ > 64 * 1024 * 1024 || capacitySectors_ > quint64(kMaxGrains) * grainSectors_)
        return reject(QStringLiteral("grain geometry exceeds implementation limits"));
    if (overheadBytes_ < kSectorSize || overheadBytes_ > fileSize ||
        descriptorOffset_ < kSectorSize || descriptorLength_ < kSectorSize ||
        descriptorOffset_ + descriptorLength_ > fileSize || descriptorOffset_ + descriptorLength_ > overheadBytes_)
        return reject(QStringLiteral("invalid embedded descriptor bounds"));

    descriptor_.resize(int(descriptorLength_));
    if (!readFile(descriptorOffset_, descriptor_.data(), descriptor_.size()))
        return reject(QStringLiteral("cannot read embedded descriptor"));
    const int nul = descriptor_.indexOf('\0');
    if (nul >= 0) descriptor_.truncate(nul);
    const QString descriptorText = QString::fromUtf8(descriptor_);
    const QStringList lines = descriptorText.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts);
    bool createType = false;
    int extents = 0;
    const QRegularExpression extentRe(QStringLiteral("^(?:RDONLY|RW)\\s+([0-9]+)\\s+SPARSE\\s+\"([^\"]+)\"\\s*$"));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("createType="))) {
            createType = line == QStringLiteral("createType=\"streamOptimized\"");
        }
        if (line.startsWith(QStringLiteral("parentFileNameHint=")) && line != QStringLiteral("parentFileNameHint=\"\""))
            return reject(QStringLiteral("parented disks are not supported"));
        if (line.startsWith(QStringLiteral("parentCID=")) &&
            line.compare(QStringLiteral("parentCID=ffffffff"), Qt::CaseInsensitive) != 0)
            return reject(QStringLiteral("parented disks are not supported"));
        if (line.startsWith(QStringLiteral("RW ")) || line.startsWith(QStringLiteral("RDONLY ")) ||
            line.startsWith(QStringLiteral("NOACCESS "))) {
            const auto match = extentRe.match(line);
            if (!match.hasMatch() || match.captured(1).toLongLong() != capacitySectors_)
                return reject(QStringLiteral("requires one standalone sparse extent"));
            ++extents;
        }
    }
    if (!createType || extents != 1) return reject(QStringLiteral("descriptor is not a single streamOptimized extent"));

    qint64 position = overheadBytes_;
    bool ended = false;
    qint64 markerCount = 0;
    while (position + 16 <= fileSize && markerCount++ < kMaxGrains * 2) {
        char marker[16];
        if (!readFile(position, marker, sizeof(marker))) return reject(QStringLiteral("truncated marker"));
        const quint32 size = qFromLittleEndian<quint32>(marker + 8);
        const quint32 type = qFromLittleEndian<quint32>(marker + 12);
        const quint64 value = qFromLittleEndian<quint64>(marker);
        if (size) {
            if (value % grainSectors_ || value >= capacitySectors_ || size > quint32(grainBytes_ + 65536))
                return reject(QStringLiteral("invalid grain marker"));
            const qint64 index = qint64(value / grainSectors_);
            if (grains_.find(index) != grains_.end()) return reject(QStringLiteral("duplicate grain marker"));
            if (grains_.size() >= size_t(kMaxGrains) || position + 12 + size > fileSize)
                return reject(QStringLiteral("grain index exceeds bounds"));
            grains_[index] = {position + 12, size};
            position = alignTo(position + 12 + size, kSectorSize);
            continue;
        }

        if (type == 0) {
            ended = true;
            break;
        }
        if (type < 1 || type > 3 || value > quint64(fileSize / kSectorSize))
            return reject(QStringLiteral("unknown metadata marker"));
        const qint64 bytes = qint64(value) * kSectorSize;
        if (bytes <= 0 || bytes > kMaxMetadataBytes || position + kSectorSize + bytes > fileSize)
            return reject(QStringLiteral("metadata marker exceeds bounds"));
        position += kSectorSize + bytes;
    }
    if (!ended) return reject(QStringLiteral("missing end marker"));
    streamOptimized_ = true;
    valid_ = true;
    return true;
}

bool VmdkStreamOptimizedWriter::loadGrain(qint64 index, QByteArray* bytes, QString* error) const {
    if (!bytes || index < 0 || index >= (capacitySectors_ + grainSectors_ - 1) / grainSectors_) return false;
    if (cacheIndex_ == index) {
        *bytes = cache_;
        return true;
    }
    const auto changed = changedGrains_.find(index);
    if (changed != changedGrains_.end()) {
        *bytes = QByteArray(changed->second.data(), int(changed->second.size()));
    } else {
        const auto found = grains_.find(index);
        if (found == grains_.end()) {
            *bytes = QByteArray(int(grainBytes_), '\0');
        } else {
            QByteArray compressed(int(found->second.size), Qt::Uninitialized);
            if (!readFile(found->second.offset, compressed.data(), compressed.size())) return false;
            std::vector<uint8_t> inflated;
            if (!peare::compression::inflateZlib(reinterpret_cast<const uint8_t*>(compressed.constData()),
                                                 size_t(compressed.size()), size_t(grainBytes_), &inflated) ||
                inflated.size() != size_t(grainBytes_)) {
                if (error) *error = failure(QStringLiteral("grain decompression failed"));
                return false;
            }
            *bytes = QByteArray(reinterpret_cast<const char*>(inflated.data()), int(inflated.size()));
        }
    }
    cacheIndex_ = index;
    cache_ = *bytes;
    return true;
}

bool VmdkStreamOptimizedWriter::read(qint64 offset, char* data, qint64 length) const {
    if (!valid_ || offset < 0 || length < 0 || offset > capacityBytes_ || length > capacityBytes_ - offset || (!data && length))
        return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 absolute = offset + done;
        const qint64 index = absolute / grainBytes_;
        const qint64 within = absolute % grainBytes_;
        const qint64 count = std::min(length - done, grainBytes_ - within);
        QByteArray grain;
        if (!loadGrain(index, &grain)) return false;
        std::memcpy(data + done, grain.constData() + within, size_t(count));
        done += count;
    }
    return true;
}

bool VmdkStreamOptimizedWriter::write(qint64 offset, const char* data, qint64 length) {
    if (!valid_ || !writable_ || offset < 0 || length < 0 || offset > capacityBytes_ ||
        length > capacityBytes_ - offset || (!data && length)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 absolute = offset + done;
        const qint64 index = absolute / grainBytes_;
        const qint64 within = absolute % grainBytes_;
        const qint64 count = std::min(length - done, grainBytes_ - within);
        QByteArray grain;
        if (!loadGrain(index, &grain)) return false;
        std::vector<char>& changed = changedGrains_[index];
        if (changed.empty()) changed.assign(grain.constData(), grain.constData() + grain.size());
        std::memcpy(changed.data() + within, data + done, size_t(count));
        cacheIndex_ = -1;
        done += count;
    }
    return true;
}

bool VmdkStreamOptimizedWriter::convertToSparse(QString* error) {
    auto reject = [&](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (outputName_.isEmpty() || outputName_.contains(QLatin1Char('"')) ||
        outputName_.contains(QLatin1Char('\n')) || outputName_.contains(QLatin1Char('\r')))
        return reject(QStringLiteral("missing or invalid destination image name"));

    const qint64 tableCount = (capacitySectors_ + grainSectors_ * entriesPerTable_ - 1) /
                              (grainSectors_ * entriesPerTable_);
    if (tableCount <= 0 || tableCount > 131072) return reject(QStringLiteral("too many grain tables"));
    const qint64 tableBytes = entriesPerTable_ * 4;
    const qint64 directoryBytes = alignTo(tableCount * 4, kSectorSize);
    const qint64 tableSpan = alignTo(tableBytes, kSectorSize);
    QByteArray descriptorText = QString::fromUtf8(descriptor_).toUtf8();
    QString text = QString::fromUtf8(descriptorText);
    QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts);
    QStringList outputLines;
    bool hasEncoding = false;
    for (QString line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("createType="))) line = QStringLiteral("createType=\"monolithicSparse\"");
        else if (trimmed.startsWith(QStringLiteral("encoding="))) {
            line = QStringLiteral("encoding=\"UTF-8\"");
            hasEncoding = true;
        } else if (trimmed.startsWith(QStringLiteral("RW ")) || trimmed.startsWith(QStringLiteral("RDONLY "))) {
            line = QStringLiteral("RW %1 SPARSE \"%2\"").arg(capacitySectors_).arg(outputName_);
        }
        outputLines.push_back(line);
    }
    if (!hasEncoding) outputLines.insert(1, QStringLiteral("encoding=\"UTF-8\""));
    const QByteArray descriptor = (outputLines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    const qint64 descriptorSectors = std::max(
        alignTo(descriptor.size(), kSectorSize) / kSectorSize,
        descriptorLength_ / kSectorSize);
    if (descriptorSectors <= 0 || descriptorSectors > std::numeric_limits<quint32>::max())
        return reject(QStringLiteral("destination descriptor is too large"));
    const qint64 directorySectors = directoryBytes / kSectorSize;
    const qint64 tableSectors = tableSpan / kSectorSize;
    const qint64 overheadSectors = alignTo(1 + descriptorSectors + 2 * directorySectors +
        2 * tableCount * (tableSpan / kSectorSize), grainSectors_);
    const qint64 overheadBytes = overheadSectors * kSectorSize;
    if (overheadBytes > kMaxMetadataBytes) return reject(QStringLiteral("output metadata exceeds implementation limits"));

    QTemporaryFile output;
    if (!output.open() || !output.resize(overheadBytes)) return reject(QStringLiteral("cannot create conversion staging file"));
    QByteArray header(kSectorSize, '\0');
    qToLittleEndian<quint32>(kSparseMagic, reinterpret_cast<uchar*>(header.data()));
    qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(header.data() + 4));
    qToLittleEndian<quint32>(3, reinterpret_cast<uchar*>(header.data() + 8));
    qToLittleEndian<quint64>(capacitySectors_, reinterpret_cast<uchar*>(header.data() + 12));
    qToLittleEndian<quint64>(grainSectors_, reinterpret_cast<uchar*>(header.data() + 20));
    qToLittleEndian<quint64>(1, reinterpret_cast<uchar*>(header.data() + 28));
    qToLittleEndian<quint64>(descriptorSectors, reinterpret_cast<uchar*>(header.data() + 36));
    qToLittleEndian<quint32>(quint32(entriesPerTable_), reinterpret_cast<uchar*>(header.data() + 44));
    qToLittleEndian<quint64>(1 + descriptorSectors, reinterpret_cast<uchar*>(header.data() + 48));
    qToLittleEndian<quint64>(1 + descriptorSectors + directorySectors +
                                  tableCount * tableSectors,
                              reinterpret_cast<uchar*>(header.data() + 56));
    qToLittleEndian<quint64>(overheadSectors, reinterpret_cast<uchar*>(header.data() + 64));
    header[72] = 0;
    header[73] = '\n';
    header[74] = ' ';
    header[75] = '\r';
    header[76] = '\n';
    qToLittleEndian<quint16>(0, reinterpret_cast<uchar*>(header.data() + 77));
    if (!writeAt(&output, 0, header.constData(), header.size()) ||
        !writeAt(&output, kSectorSize, descriptor.constData(), descriptor.size()))
        return reject(QStringLiteral("cannot write destination header"));

    const qint64 rgdSector = 1 + descriptorSectors;
    const qint64 redundantTableStart = rgdSector + directorySectors;
    const qint64 gdSector = redundantTableStart + tableCount * tableSectors;
    const qint64 grainTableStart = gdSector + directorySectors;
    QByteArray rgd(int(directoryBytes), '\0');
    QByteArray gd(int(directoryBytes), '\0');
    std::vector<QByteArray> redTables(size_t(tableCount), QByteArray(int(tableSpan), '\0'));
    std::vector<QByteArray> grainTables(size_t(tableCount), QByteArray(int(tableSpan), '\0'));
    for (qint64 table = 0; table < tableCount; ++table) {
        qToLittleEndian<quint32>(quint32(redundantTableStart + table * tableSectors),
            reinterpret_cast<uchar*>(rgd.data() + table * 4));
        qToLittleEndian<quint32>(quint32(grainTableStart + table * tableSectors),
            reinterpret_cast<uchar*>(gd.data() + table * 4));
    }
    if (!writeAt(&output, rgdSector * kSectorSize, rgd.constData(), rgd.size()) ||
        !writeAt(&output, gdSector * kSectorSize, gd.constData(), gd.size()))
        return reject(QStringLiteral("cannot write grain directories"));

    qint64 dataSector = overheadSectors;
    const qint64 grainCount = (capacitySectors_ + grainSectors_ - 1) / grainSectors_;
    for (qint64 index = 0; index < grainCount; ++index) {
        QByteArray grain;
        if (!loadGrain(index, &grain, error)) return reject(error && !error->isEmpty() ? *error : QStringLiteral("cannot read source grain"));
        bool nonzero = false;
        for (char byte : grain) if (byte) { nonzero = true; break; }
        if (!nonzero) continue;
        if (dataSector > std::numeric_limits<quint32>::max() ||
            dataSector + grainSectors_ > std::numeric_limits<quint32>::max())
            return reject(QStringLiteral("destination exceeds sparse VMDK sector address limits"));
        if (!writeAt(&output, dataSector * kSectorSize, grain.constData(), grain.size()))
            return reject(QStringLiteral("cannot write destination grain"));
        const qint64 table = index / entriesPerTable_;
        const qint64 entry = index % entriesPerTable_;
        const quint32 sector = quint32(dataSector);
        qToLittleEndian<quint32>(sector, reinterpret_cast<uchar*>(redTables[size_t(table)].data() + entry * 4));
        qToLittleEndian<quint32>(sector, reinterpret_cast<uchar*>(grainTables[size_t(table)].data() + entry * 4));
        dataSector += grainSectors_;
    }
    for (qint64 table = 0; table < tableCount; ++table) {
        if (!writeAt(&output, (redundantTableStart + table * tableSectors) * kSectorSize,
                     redTables[size_t(table)].constData(), redTables[size_t(table)].size()) ||
            !writeAt(&output, (grainTableStart + table * tableSectors) * kSectorSize,
                     grainTables[size_t(table)].constData(), grainTables[size_t(table)].size()))
            return reject(QStringLiteral("cannot write grain tables"));
    }
    const qint64 outputSize = std::max(overheadBytes, dataSector * kSectorSize);
    if (!output.resize(outputSize) || !output.flush() || !output.seek(0))
        return reject(QStringLiteral("cannot finalize converted VMDK"));

    if (!file_->resize(0) || !file_->seek(0)) return reject(QStringLiteral("cannot replace staged streamOptimized image"));
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!output.atEnd()) {
        const qint64 got = output.read(buffer.data(), buffer.size());
        if (got <= 0 || file_->write(buffer.constData(), got) != got)
            return reject(QStringLiteral("cannot copy converted VMDK to transaction staging"));
    }
    if (!file_->resize(outputSize) || !file_->flush())
        return reject(QStringLiteral("cannot flush converted VMDK"));
    streamOptimized_ = false;
    grains_.clear();
    changedGrains_.clear();
    cache_.clear();
    cacheIndex_ = -1;
    return true;
}

bool VmdkStreamOptimizedWriter::finish(QString* error) {
    if (!valid_ || !writable_) {
        if (error) *error = QStringLiteral("streamOptimized image is not writable");
        return false;
    }
    if (!streamOptimized_) return true;
    if (changedGrains_.empty()) return true;
    return convertToSparse(error);
}

}  // namespace writer
}  // namespace peare
