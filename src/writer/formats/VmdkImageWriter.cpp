#include "VmdkImageWriter.h"

#include <QtEndian>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace peare {
namespace writer {
namespace {

const qint64 kSector = 512;
const quint32 kRedundantGrainDirectoryFlag = 2;
const std::size_t kMaxVmdkValidationExtents =
    (64U * 1024U * 1024U) / sizeof(std::pair<quint64, quint64>);

quint32 readLe32(const char* bytes) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes));
}

quint64 readLe64(const char* bytes) {
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(bytes));
}

bool parseDescriptor(const QByteArray& bytes, quint64 capacitySectors) {
    const QByteArray descriptor = bytes.left(bytes.indexOf('\0') < 0
        ? bytes.size() : bytes.indexOf('\0'));
    const std::string text(descriptor.constData(), static_cast<std::size_t>(descriptor.size()));
    if (text.find("# Disk DescriptorFile") == std::string::npos ||
        text.find("parentCID=ffffffff") == std::string::npos ||
        text.find("parentFileNameHint") != std::string::npos ||
        text.find("createType=\"monolithicSparse\"") == std::string::npos)
        return false;
    std::istringstream lines(text);
    std::string line;
    int extentCount = 0;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string access;
        fields >> access;
        if (access != "RW" && access != "RDONLY" && access != "NOACCESS") continue;
        std::string type, path;
        quint64 sectors = 0;
        if (!(fields >> sectors >> type)) return false;
        std::getline(fields >> std::ws, path);
        while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back())))
            path.pop_back();
        if (access != "RW" || sectors != capacitySectors || type != "SPARSE" ||
            path.size() < 3 || path.front() != '"' || path.back() != '"' ||
            path.find('"', 1) != path.size() - 1)
            return false;
        ++extentCount;
    }
    return extentCount == 1;
}

}  // namespace

VmdkImageWriter::VmdkImageWriter(QFile* file, bool writable, QString* error)
    : file_(file), writable_(writable) {
    valid_ = parse(error);
}

VmdkImageWriter::VmdkImageWriter(QFile* file, bool writable, bool externalExtent,
                                 QString* error)
    : file_(file), writable_(writable), externalExtent_(externalExtent) {
    valid_ = parse(error);
}

bool VmdkImageWriter::readFile(qint64 offset, char* data, qint64 length) const {
    if (!file_ || offset < 0 || length < 0 || offset > file_->size() ||
        length > file_->size() - offset || !file_->seek(offset)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 count = file_->read(data + done, length - done);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool VmdkImageWriter::writeFile(qint64 offset, const char* data, qint64 length) {
    if (!file_ || !writable_ || offset < 0 || length < 0 ||
        offset > (std::numeric_limits<qint64>::max)() - length || !file_->seek(offset))
        return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 count = file_->write(data + done, length - done);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool VmdkImageWriter::parse(QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const quint64 fileSize = file_ ? quint64(file_->size()) : 0;
    if (!file_ || fileSize < quint64(kSector) || fileSize % quint64(kSector))
        return fail(QStringLiteral("Truncated or unaligned sparse VMDK extent."));
    QByteArray header(int(kSector), Qt::Uninitialized);
    if (!readFile(0, header.data(), header.size()) ||
        std::memcmp(header.constData(), "KDMV", 4) != 0)
        return fail(QStringLiteral("Invalid sparse VMDK header."));
    const quint32 version = readLe32(header.constData() + 4);
    const quint32 flags = readLe32(header.constData() + 8);
    const quint64 capacitySectors = readLe64(header.constData() + 12);
    const quint64 grainSectors = readLe64(header.constData() + 20);
    const quint64 descriptorSector = readLe64(header.constData() + 28);
    const quint64 descriptorSectors = readLe64(header.constData() + 36);
    const quint32 entriesPerTable = readLe32(header.constData() + 44);
    const quint64 redundantDirectorySector = readLe64(header.constData() + 48);
    const quint64 directorySector = readLe64(header.constData() + 56);
    const quint64 overheadSectors = readLe64(header.constData() + 64);
    const quint8 cleanShutdown = quint8(header.at(72));
    const quint16 compression = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(header.constData() + 77));
    hasRedundantDirectory_ = redundantDirectorySector != 0;
    zeroedGrainEntries_ = version == 2 && bool(flags & 4U);
    if ((version != 1 && version != 2) || (version == 1 && (flags & 4U)))
        return fail(QStringLiteral("Only version 1 and supported version 2 sparse VMDK extents are writable."));
    if ((flags & ~7U) || !hasRedundantDirectory_ || !capacitySectors || !grainSectors ||
        grainSectors > quint64(64 * 1024 * 1024) / kSector ||
        (grainSectors & (grainSectors - 1)) || !entriesPerTable ||
        entriesPerTable > 16384 || cleanShutdown > 1 || compression != 0 ||
        (externalExtent_ ? ((descriptorSector == 0) != (descriptorSectors == 0))
                         : (!descriptorSector || !descriptorSectors)) ||
        !overheadSectors ||
        capacitySectors > quint64((std::numeric_limits<qint64>::max)()) / kSector ||
        (directorySector == 0) ||
        header.at(73) != '\n' || header.at(74) != ' ' ||
        header.at(75) != '\r' || header.at(76) != '\n' ||
        bool(flags & kRedundantGrainDirectoryFlag) != (redundantDirectorySector != 0))
        return fail(QStringLiteral("Unsupported or invalid sparse VMDK geometry."));
    if (grainSectors > quint64((std::numeric_limits<qint64>::max)()) / entriesPerTable ||
        descriptorSector > quint64((std::numeric_limits<qint64>::max)()) / kSector ||
        descriptorSectors > quint64((std::numeric_limits<qint64>::max)()) / kSector ||
        directorySector > quint64((std::numeric_limits<qint64>::max)()) / kSector ||
        redundantDirectorySector > quint64((std::numeric_limits<qint64>::max)()) / kSector)
        return fail(QStringLiteral("Sparse VMDK metadata offsets exceed supported limits."));
    capacitySectors_ = qint64(capacitySectors);
    capacityBytes_ = capacitySectors_ * kSector;
    grainSectors_ = qint64(grainSectors);
    grainBytes_ = grainSectors_ * kSector;
    entriesPerTable_ = entriesPerTable;
    const quint64 grains = (capacitySectors - 1) / grainSectors + 1;
    const quint64 directoryCount = (grains - 1) / entriesPerTable + 1;
    if (directoryCount > 131072 || directoryCount > quint64((std::numeric_limits<int>::max)()) / 4)
        return fail(QStringLiteral("Sparse VMDK grain directory exceeds resource limits."));
    grainTablesPerDirectory_ = qint64(directoryCount);
    grainTableBytes_ = qint64(entriesPerTable) * 4;
    grainTableSpan_ = ((grainTableBytes_ + kSector - 1) / kSector) * kSector;
    const qint64 directoryBytes = grainTablesPerDirectory_ * 4;
    grainDirectorySpan_ = ((directoryBytes + kSector - 1) / kSector) * kSector;
    descriptorOffset_ = qint64(descriptorSector * kSector);
    descriptorLength_ = qint64(descriptorSectors * kSector);
    grainDirectoryOffset_ = qint64(directorySector * kSector);
    hasRedundantDirectory_ = redundantDirectorySector != 0;
    redundantDirectoryOffset_ = hasRedundantDirectory_
        ? qint64(redundantDirectorySector * kSector) : 0;
    if (grainTableSpan_ > 64 * 1024 ||
        grainTablesPerDirectory_ > (64 * 1024 * 1024) / (grainTableSpan_ * (hasRedundantDirectory_ ? 2 : 1)) ||
        descriptorSectors > 1024 || descriptorSector > fileSize / kSector ||
        descriptorSectors > fileSize / kSector ||
        descriptorSector + descriptorSectors > fileSize / kSector ||
        directorySector > fileSize / kSector ||
        (hasRedundantDirectory_ && redundantDirectorySector > fileSize / kSector) ||
        descriptorLength_ > qint64(fileSize) - descriptorOffset_ ||
        (descriptorLength_ && descriptorOffset_ < kSector) ||
        directorySector > fileSize / kSector ||
        quint64(grainDirectorySpan_) > fileSize - quint64(grainDirectoryOffset_) ||
        (hasRedundantDirectory_ &&
         quint64(grainDirectorySpan_) > fileSize - quint64(redundantDirectoryOffset_)) ||
        overheadSectors > fileSize / kSector)
        return fail(QStringLiteral("Sparse VMDK descriptor or grain directory is out of bounds."));
    if (!externalExtent_) {
        QByteArray descriptor(int(descriptorLength_), Qt::Uninitialized);
        if (!readFile(descriptorOffset_, descriptor.data(), descriptor.size()) ||
            !parseDescriptor(descriptor, capacitySectors))
            return fail(QStringLiteral("VMDK descriptor is not a standalone monolithicSparse disk."));
    }

    QByteArray rawDirectory(int(directoryBytes), Qt::Uninitialized);
    if (!readFile(grainDirectoryOffset_, rawDirectory.data(), rawDirectory.size()))
        return fail(QStringLiteral("Cannot read sparse VMDK grain directory."));
    grainDirectory_.resize(static_cast<std::size_t>(grainTablesPerDirectory_));
    for (qint64 i = 0; i < grainTablesPerDirectory_; ++i)
        grainDirectory_[static_cast<std::size_t>(i)] = readLe32(rawDirectory.constData() + i * 4);
    if (hasRedundantDirectory_) {
        QByteArray rawRedundant(int(directoryBytes), Qt::Uninitialized);
        if (!readFile(redundantDirectoryOffset_, rawRedundant.data(), rawRedundant.size()))
            return fail(QStringLiteral("Cannot read redundant sparse VMDK grain directory."));
        redundantDirectory_.resize(static_cast<std::size_t>(grainTablesPerDirectory_));
        for (qint64 i = 0; i < grainTablesPerDirectory_; ++i) {
            redundantDirectory_[static_cast<std::size_t>(i)] = readLe32(rawRedundant.constData() + i * 4);
        }
    }

    std::vector<std::pair<quint64, quint64>> extents;
    extents.push_back({0, kSector});
    if (descriptorLength_)
        extents.push_back({quint64(descriptorOffset_), quint64(descriptorOffset_ + descriptorLength_)});
    extents.push_back({quint64(grainDirectoryOffset_), quint64(grainDirectoryOffset_ + grainDirectorySpan_)});
    if (hasRedundantDirectory_)
        extents.push_back({quint64(redundantDirectoryOffset_),
                           quint64(redundantDirectoryOffset_ + grainDirectorySpan_)});
    const quint64 grainExtentLimit = qMin<quint64>(grains, 1000000);
    const quint64 tableExtentLimit = quint64(grainTablesPerDirectory_) *
        (hasRedundantDirectory_ ? 2 : 1);
    extents.reserve(extents.size() + static_cast<std::size_t>(
        grainExtentLimit + tableExtentLimit));
    grainTables_.resize(static_cast<std::size_t>(grainTablesPerDirectory_));
    if (hasRedundantDirectory_)
        redundantGrainTables_.resize(static_cast<std::size_t>(grainTablesPerDirectory_));
    std::vector<quint64> mainOffsets(static_cast<std::size_t>(grainTablesPerDirectory_));
    std::vector<quint64> redundantOffsets(static_cast<std::size_t>(grainTablesPerDirectory_));
    for (qint64 table = 0; table < grainTablesPerDirectory_; ++table) {
        const quint32 mainSector = grainDirectory_[static_cast<std::size_t>(table)];
        const quint32 redundantSector = hasRedundantDirectory_
            ? redundantDirectory_[static_cast<std::size_t>(table)] : mainSector;
        if ((mainSector == 0) != (redundantSector == 0))
            return fail(QStringLiteral("Sparse VMDK directory entries disagree at table %1.").arg(table));
        if (!mainSector) continue;
        if (hasRedundantDirectory_ && mainSector == redundantSector)
            return fail(QStringLiteral("Sparse VMDK redundant directories reference the same grain table."));
        const quint64 mainOffset = quint64(mainSector) * kSector;
        const quint64 redundantOffset = quint64(redundantSector) * kSector;
        if (mainOffset > fileSize ||
            quint64(grainTableSpan_) > fileSize - mainOffset ||
            (hasRedundantDirectory_ &&
             (redundantOffset > fileSize ||
              quint64(grainTableSpan_) > fileSize - redundantOffset)))
            return fail(QStringLiteral("Sparse VMDK grain table is out of bounds."));
        mainOffsets[static_cast<std::size_t>(table)] = mainOffset;
        redundantOffsets[static_cast<std::size_t>(table)] = redundantOffset;
    }

    for (qint64 table = 0; table < grainTablesPerDirectory_; ++table) {
        if (!mainOffsets[static_cast<std::size_t>(table)]) continue;
        QByteArray& mainTable = grainTables_[static_cast<std::size_t>(table)];
        mainTable.resize(int(grainTableSpan_));
        if (!readFile(qint64(mainOffsets[static_cast<std::size_t>(table)]),
                      mainTable.data(), mainTable.size()))
            return fail(QStringLiteral("Cannot read sparse VMDK grain table."));
        if (hasRedundantDirectory_) {
            QByteArray& redundantTable = redundantGrainTables_[static_cast<std::size_t>(table)];
            redundantTable.resize(int(grainTableSpan_));
            if (!readFile(qint64(redundantOffsets[static_cast<std::size_t>(table)]),
                          redundantTable.data(), redundantTable.size()))
                return fail(QStringLiteral("Cannot read redundant sparse VMDK grain table."));
        }
    }

    for (qint64 table = 0; table < grainTablesPerDirectory_; ++table) {
        const quint64 mainOffset = mainOffsets[static_cast<std::size_t>(table)];
        if (!mainOffset) continue;
        const quint64 redundantOffset = redundantOffsets[static_cast<std::size_t>(table)];
        const QByteArray& mainTable = grainTables_[static_cast<std::size_t>(table)];
        extents.push_back({mainOffset, mainOffset + quint64(grainTableSpan_)});
        if (hasRedundantDirectory_) {
            const QByteArray& redundantTable = redundantGrainTables_[static_cast<std::size_t>(table)];
            if (redundantTable != mainTable)
                return fail(QStringLiteral("Sparse VMDK redundant tables diverge at table %1 (%2 vs %3).")
                            .arg(table).arg(grainDirectory_[static_cast<std::size_t>(table)])
                            .arg(redundantDirectory_[static_cast<std::size_t>(table)]));
            if (redundantOffset != mainOffset)
                extents.push_back({redundantOffset, redundantOffset + quint64(grainTableSpan_)});
        }
        const qint64 grainCountInTable = qMin<qint64>(entriesPerTable,
            qMax<qint64>(0, qint64(grains) - table * entriesPerTable));
        for (qint64 index = 0; index < grainCountInTable; ++index) {
            const quint32 sector = readLe32(mainTable.constData() + index * 4);
            if (!sector || (zeroedGrainEntries_ && sector == 1)) continue;
            const quint64 start = quint64(sector) * kSector;
            const quint64 length = quint64(grainBytes_);
            if (quint64(sector) < overheadSectors || start > fileSize ||
                length > fileSize - start)
                return fail(QStringLiteral("Sparse VMDK grain payload is out of bounds."));
            if (extents.size() >= kMaxVmdkValidationExtents)
                return fail(QStringLiteral("Sparse VMDK allocation map exceeds the 64 MiB validation budget."));
            extents.push_back({start, start + length});
        }
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i].first < extents[i - 1].second)
            return fail(QStringLiteral("Sparse VMDK metadata and grain payloads overlap."));
    valid_ = true;
    return true;
}

bool VmdkImageWriter::read(qint64 offset, char* data, qint64 length) const {
    if (!valid_ || !data || offset < 0 || length < 0 || offset > capacityBytes_ ||
        length > capacityBytes_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 grain = position / grainBytes_;
        const qint64 inGrain = position % grainBytes_;
        const qint64 count = std::min(length - done, grainBytes_ - inGrain);
        const auto changed = changedGrains_.find(grain);
        if (changed != changedGrains_.end()) {
            std::memcpy(data + done, changed->second.data() + inGrain,
                        static_cast<std::size_t>(count));
        } else {
            const qint64 table = grain / entriesPerTable_;
            const qint64 index = grain % entriesPerTable_;
            const QByteArray& entries = grainTables_[static_cast<std::size_t>(table)];
            const quint32 sector = entries.isEmpty() ? 0 : readLe32(entries.constData() + index * 4);
            if (!sector || (zeroedGrainEntries_ && sector == 1)) {
                std::memset(data + done, 0, static_cast<std::size_t>(count));
            } else if (!readFile(qint64(sector) * kSector + inGrain, data + done, count)) {
                return false;
            }
        }
        done += count;
    }
    return true;
}

bool VmdkImageWriter::write(qint64 offset, const char* data, qint64 length) {
    if (!valid_ || !writable_ || !data || offset < 0 || length < 0 ||
        offset > capacityBytes_ || length > capacityBytes_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 grain = position / grainBytes_;
        const qint64 inGrain = position % grainBytes_;
        const qint64 count = std::min(length - done, grainBytes_ - inGrain);
        const qint64 table = grain / entriesPerTable_;
        const qint64 index = grain % entriesPerTable_;
        const QByteArray& entries = grainTables_[static_cast<std::size_t>(table)];
        const quint32 grainSector = entries.isEmpty()
            ? 0 : readLe32(entries.constData() + index * 4);
        if (grainSector && !(zeroedGrainEntries_ && grainSector == 1)) {
            const qint64 host = qint64(grainSector) * kSector + inGrain;
            if (!writeFile(host, data + done, count)) return false;
            done += count;
            continue;
        }
        auto changed = changedGrains_.find(grain);
        if (changed == changedGrains_.end()) {
            std::vector<char> bytes(static_cast<std::size_t>(grainBytes_), 0);
            const qint64 validBytes = std::min(grainBytes_, capacityBytes_ - grain * grainBytes_);
            if (!read(grain * grainBytes_, bytes.data(), validBytes)) return false;
            changed = changedGrains_.insert(std::make_pair(grain, std::move(bytes))).first;
        }
        std::memcpy(changed->second.data() + inGrain, data + done,
                    static_cast<std::size_t>(count));
        done += count;
    }
    return true;
}

bool VmdkImageWriter::finish(QString* error) {
    if (!valid_ || !writable_ || !file_) {
        if (error) *error = QStringLiteral("VMDK writer is not active.");
        return false;
    }
    qint64 appendOffset = file_->size();
    for (const auto& changed : changedGrains_) {
        const qint64 grain = changed.first;
        const qint64 table = grain / entriesPerTable_;
        const qint64 index = grain % entriesPerTable_;
        QByteArray& mainEntries = grainTables_[static_cast<std::size_t>(table)];
        QByteArray& redundantEntries = hasRedundantDirectory_
            ? redundantGrainTables_[static_cast<std::size_t>(table)] : mainEntries;
        quint32 mainTableSector = grainDirectory_[static_cast<std::size_t>(table)];
        quint32 redundantTableSector = hasRedundantDirectory_
            ? redundantDirectory_[static_cast<std::size_t>(table)] : mainTableSector;
        if (mainEntries.isEmpty()) {
            const qint64 remainder = appendOffset % kSector;
            if (remainder) appendOffset += kSector - remainder;
            const auto appendTable = [&](quint32* sector, const QByteArray& entries) {
                if (appendOffset % kSector ||
                    quint64(appendOffset / kSector) > quint64((std::numeric_limits<quint32>::max)()) ||
                    appendOffset > (std::numeric_limits<qint64>::max)() - grainTableSpan_) {
                    if (error) *error = QStringLiteral("Sparse VMDK grain table allocation exceeds format limits.");
                    return false;
                }
                *sector = quint32(appendOffset / kSector);
                if (!writeFile(appendOffset, entries.constData(), entries.size())) {
                    if (error) *error = file_->errorString();
                    return false;
                }
                appendOffset += grainTableSpan_;
                return true;
            };
            mainEntries = QByteArray(int(grainTableSpan_), '\0');
            if (!appendTable(&mainTableSector, mainEntries)) return false;
            grainDirectory_[static_cast<std::size_t>(table)] = mainTableSector;
            char directoryEntry[4];
            qToLittleEndian<quint32>(mainTableSector, reinterpret_cast<uchar*>(directoryEntry));
            if (!writeFile(grainDirectoryOffset_ + table * 4, directoryEntry, sizeof(directoryEntry))) {
                if (error) *error = file_->errorString();
                return false;
            }
            if (hasRedundantDirectory_) {
                redundantEntries = mainEntries;
                if (!appendTable(&redundantTableSector, redundantEntries)) return false;
                redundantDirectory_[static_cast<std::size_t>(table)] = redundantTableSector;
                qToLittleEndian<quint32>(redundantTableSector,
                                         reinterpret_cast<uchar*>(directoryEntry));
                if (!writeFile(redundantDirectoryOffset_ + table * 4,
                               directoryEntry, sizeof(directoryEntry))) {
                    if (error) *error = file_->errorString();
                    return false;
                }
            }
        }
        const quint32 existingGrainSector = mainEntries.isEmpty()
            ? 0 : readLe32(mainEntries.constData() + index * 4);
        if (existingGrainSector && !(zeroedGrainEntries_ && existingGrainSector == 1)) {
            const qint64 grainOffset = qint64(existingGrainSector) * kSector;
            if (!writeFile(grainOffset, changed.second.data(), grainBytes_)) {
                if (error) *error = file_->errorString();
                return false;
            }
            continue;
        }
        const qint64 grainRemainder = appendOffset % grainBytes_;
        if (grainRemainder) {
            const qint64 padding = grainBytes_ - grainRemainder;
            if (appendOffset > (std::numeric_limits<qint64>::max)() - padding) {
                if (error) *error = QStringLiteral("Sparse VMDK grain alignment exceeds file limits.");
                return false;
            }
            appendOffset += padding;
        }
        if (appendOffset % kSector || appendOffset / kSector > (std::numeric_limits<quint32>::max)() ||
            appendOffset > (std::numeric_limits<qint64>::max)() - grainBytes_) {
            if (error) *error = QStringLiteral("Sparse VMDK grain allocation exceeds format limits.");
            return false;
        }
        const quint32 newGrainSector = quint32(appendOffset / kSector);
        if (!writeFile(appendOffset, changed.second.data(), grainBytes_)) {
            if (error) *error = file_->errorString();
            return false;
        }
        char grainPointer[4];
        qToLittleEndian<quint32>(newGrainSector, reinterpret_cast<uchar*>(grainPointer));
        std::memcpy(mainEntries.data() + index * 4, grainPointer, sizeof(grainPointer));
        if (hasRedundantDirectory_)
            std::memcpy(redundantEntries.data() + index * 4, grainPointer, sizeof(grainPointer));
        const qint64 mainTableOffset = qint64(mainTableSector) * kSector;
        if (!writeFile(mainTableOffset + index * 4, grainPointer, sizeof(grainPointer))) {
            if (error) *error = file_->errorString();
            return false;
        }
        if (hasRedundantDirectory_) {
            const qint64 redundantTableOffset = qint64(redundantTableSector) * kSector;
            if (!writeFile(redundantTableOffset + index * 4, grainPointer, sizeof(grainPointer))) {
                if (error) *error = file_->errorString();
                return false;
            }
        }
        appendOffset += grainBytes_;
    }
    if (!file_->flush()) {
        if (error) *error = file_->errorString();
        return false;
    }
    changedGrains_.clear();
    return true;
}

}  // namespace writer
}  // namespace peare
