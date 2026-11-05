#include "VmdkSplitImageWriter.h"

#include "VmdkImageWriter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <set>
#include <cstring>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace peare {
namespace writer {
namespace {

const qint64 kSectorSize = 512;
const qint64 kCopyChunkSize = 1024 * 1024;

class VmdkFlatExtentAccess final : public VmdkImageAccess {
public:
    VmdkFlatExtentAccess(QFile* file, bool writable, qint64 offset, qint64 size)
        : file_(file), writable_(writable), offset_(offset), size_(size) {
        valid_ = file_ && offset_ >= 0 && size_ > 0 && offset_ <= file_->size() &&
            size_ <= file_->size() - offset_;
    }

    bool valid() const override { return valid_; }
    qint64 capacity() const override { return size_; }

    bool read(qint64 offset, char* data, qint64 length) const override {
        if (!valid_ || !data || offset < 0 || length < 0 || offset > size_ ||
            length > size_ - offset || !file_->seek(offset_ + offset)) return false;
        qint64 done = 0;
        while (done < length) {
            const qint64 count = file_->read(data + done, length - done);
            if (count <= 0) return false;
            done += count;
        }
        return true;
    }

    bool write(qint64 offset, const char* data, qint64 length) override {
        if (!valid_ || !writable_ || !data || offset < 0 || length < 0 ||
            offset > size_ || length > size_ - offset || !file_->seek(offset_ + offset))
            return false;
        qint64 done = 0;
        while (done < length) {
            const qint64 count = file_->write(data + done, length - done);
            if (count <= 0) return false;
            done += count;
        }
        return true;
    }

    bool finish(QString* error) override {
        if (!valid_ || !writable_ || !file_ || !file_->flush()) {
            if (error) *error = file_ ? file_->errorString()
                                      : QStringLiteral("Flat VMDK extent is not open.");
            return false;
        }
        return true;
    }

private:
    QFile* file_ = nullptr;
    bool writable_ = false;
    bool valid_ = false;
    qint64 offset_ = 0;
    qint64 size_ = 0;
};

bool copyFile(QFile* input, QIODevice* output, QCryptographicHash* hash,
              QString* error) {
    if (!input || !output || !input->seek(0)) return false;
    QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
    while (true) {
        const qint64 count = input->read(buffer.data(), buffer.size());
        if (count < 0) {
            if (error) *error = input->errorString();
            return false;
        }
        if (count == 0) break;
        qint64 written = 0;
        while (written < count) {
            const qint64 part = output->write(buffer.constData() + written, count - written);
            if (part <= 0) {
                if (error) *error = output->errorString();
                return false;
            }
            written += part;
        }
        if (hash) hash->addData(buffer.constData(), int(count));
    }
    return true;
}

bool copyAllocatedRanges(QFile* input, QFile* output, QString* error) {
#ifdef Q_OS_WIN
    if (!input || !output || input->size() < 0) {
        if (error) *error = QStringLiteral("VMDK extent staging received an invalid file.");
        return false;
    }
    const intptr_t inputHandle = _get_osfhandle(int(input->handle()));
    const intptr_t outputHandle = _get_osfhandle(int(output->handle()));
    if (inputHandle == -1 || outputHandle == -1) {
        if (error) *error = QStringLiteral("Could not access a VMDK extent staging handle.");
        return false;
    }
    DWORD returned = 0;
    auto fallbackCopy = [&]() {
        return output->resize(0) && output->seek(0) &&
            copyFile(input, output, nullptr, error);
    };
    if (!DeviceIoControl(reinterpret_cast<HANDLE>(outputHandle), FSCTL_SET_SPARSE,
                         nullptr, 0, nullptr, 0, &returned, nullptr)) {
        const DWORD sparseError = GetLastError();
        if (sparseError == ERROR_INVALID_FUNCTION || sparseError == ERROR_INVALID_PARAMETER ||
            sparseError == ERROR_NOT_SUPPORTED)
            return fallbackCopy();
        if (error) *error = QStringLiteral("Could not mark the staged VMDK extent as sparse (Windows error %1).")
            .arg(sparseError);
        return false;
    }
    const qint64 fileSize = input->size();
    if (!output->resize(fileSize)) {
        if (error) *error = output->errorString();
        return false;
    }
    FILE_ALLOCATED_RANGE_BUFFER request = {};
    FILE_ALLOCATED_RANGE_BUFFER ranges[128] = {};
    qint64 nextOffset = 0;
    QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
    while (nextOffset < fileSize) {
        request.FileOffset.QuadPart = nextOffset;
        request.Length.QuadPart = fileSize - nextOffset;
        returned = 0;
        const BOOL queried = DeviceIoControl(
            reinterpret_cast<HANDLE>(inputHandle), FSCTL_QUERY_ALLOCATED_RANGES,
            &request, sizeof(request), ranges, sizeof(ranges), &returned, nullptr);
        const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
        if (!queried && queryError != ERROR_MORE_DATA) {
            if (queryError == ERROR_INVALID_FUNCTION || queryError == ERROR_INVALID_PARAMETER ||
                queryError == ERROR_NOT_SUPPORTED)
                return fallbackCopy();
            if (error) *error = QStringLiteral("Could not query allocated VMDK extent ranges (Windows error %1).")
                .arg(queryError);
            return false;
        }
        const DWORD count = returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        if (!count) {
            if (queried) break;
            if (error) *error = QStringLiteral("Sparse VMDK extent range query made no progress.");
            return false;
        }
        for (DWORD i = 0; i < count; ++i) {
            const qint64 rangeFirst = ranges[i].FileOffset.QuadPart;
            const qint64 length = ranges[i].Length.QuadPart;
            if (rangeFirst < 0 || length <= 0 || rangeFirst > fileSize ||
                length > fileSize - rangeFirst) {
                if (error) *error = QStringLiteral("Sparse VMDK extent returned an invalid allocated range.");
                return false;
            }
            const qint64 first = qMax(rangeFirst, nextOffset);
            const qint64 end = rangeFirst + length;
            if (first >= end) continue;
            if (!input->seek(first) || !output->seek(first)) {
                if (error) *error = input->errorString().isEmpty()
                    ? output->errorString() : input->errorString();
                return false;
            }
            const qint64 copyLength = end - first;
            qint64 copied = 0;
            while (copied < copyLength) {
                const qint64 chunk = qMin<qint64>(buffer.size(), copyLength - copied);
                if (input->read(buffer.data(), chunk) != chunk ||
                    output->write(buffer.constData(), chunk) != chunk) {
                    if (error) *error = input->errorString().isEmpty()
                        ? output->errorString() : input->errorString();
                    return false;
                }
                copied += chunk;
            }
            nextOffset = end;
        }
        if (queried) break;
    }
    if (!output->flush()) {
        if (error) *error = output->errorString();
        return false;
    }
    return true;
#else
    return copyFile(input, output, nullptr, error);
#endif
}

bool tryCloneAllocatedRanges(QFile* input, QFile* output) {
#ifdef Q_OS_WIN
    if (!input || !output || input->size() < 0) return false;
    const qint64 size = input->size();
    if (!size) return output->resize(0);
    const intptr_t sourceHandle = _get_osfhandle(int(input->handle()));
    const intptr_t targetHandle = _get_osfhandle(int(output->handle()));
    if (sourceHandle == -1 || targetHandle == -1 || !output->resize(size)) return false;
    DUPLICATE_EXTENTS_DATA request = {};
    request.FileHandle = reinterpret_cast<HANDLE>(sourceHandle);
    request.SourceFileOffset.QuadPart = 0;
    request.TargetFileOffset.QuadPart = 0;
    request.ByteCount.QuadPart = size;
    DWORD returned = 0;
    if (DeviceIoControl(reinterpret_cast<HANDLE>(targetHandle),
                        FSCTL_DUPLICATE_EXTENTS_TO_FILE, &request,
                        sizeof(request), nullptr, 0, &returned, nullptr))
        return output->seek(0);
    output->resize(0);
    return false;
#else
    Q_UNUSED(input);
    Q_UNUSED(output);
    return false;
#endif
}

bool syncFileToDisk(QFileDevice* file, QString* error) {
    if (!file || !file->flush()) {
        if (error) *error = file ? file->errorString()
                                 : QStringLiteral("No VMDK extent is open for synchronization.");
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(int(file->handle()));
    if (nativeHandle == -1 ||
        !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle))) {
        if (error) *error = QStringLiteral("Flushing the VMDK extent to storage failed (Windows error %1).")
            .arg(GetLastError());
        return false;
    }
#else
    if (::fsync(int(file->handle())) != 0) {
        if (error) *error = QStringLiteral("Flushing the VMDK extent to storage failed: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#endif
    return true;
}

#ifndef Q_OS_WIN
bool syncDirectoryToDisk(const QString& path, QString* error) {
    const QByteArray encodedPath = QFile::encodeName(path);
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(encodedPath.constData(), flags);
    if (descriptor < 0) {
        if (error) *error = QStringLiteral("Could not open the VMDK directory for synchronization: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const int syncResult = ::fsync(descriptor);
    const int syncError = errno;
    ::close(descriptor);
    if (syncResult != 0) {
        if (error) *error = QStringLiteral("Could not synchronize the VMDK directory: %1")
            .arg(QString::fromLocal8Bit(std::strerror(syncError)));
        return false;
    }
    return true;
}
#endif

}  // namespace

VmdkSplitImageWriter::VmdkSplitImageWriter(const QString& descriptorPath, bool writable,
                                           QString* error)
    : descriptorPath_(QFileInfo(descriptorPath).absoluteFilePath()), writable_(writable) {
    QString keyPath = descriptorPath_;
#ifdef Q_OS_WIN
    keyPath = keyPath.toCaseFolded();
#endif
    descriptorKey_ = QString::fromLatin1(QCryptographicHash::hash(
        keyPath.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (writable_) {
        updateLock_.reset(new QLockFile(descriptorPath_ + QStringLiteral(".peare.lock")));
        updateLock_->setStaleLockTime(0);
        if (!updateLock_->tryLock(0)) {
            if (error) *error = QStringLiteral("Another Peare update holds the VMDK transaction lock.");
            return;
        }
    }
    valid_ = parse(error);
    if (valid_ && writable_) cleanupOrphanedFiles();
}

VmdkSplitImageWriter::~VmdkSplitImageWriter() {
    if (!committed_) removePreparedFiles();
}

bool VmdkSplitImageWriter::parse(QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QFile descriptor(descriptorPath_);
    if (!descriptor.open(QIODevice::ReadOnly)) return fail(descriptor.errorString());
    if (descriptor.size() <= 0 || descriptor.size() > 1024 * 1024)
        return fail(QStringLiteral("VMDK descriptor size is outside supported limits."));
    descriptorBytes_ = descriptor.readAll();
    if (descriptor.error() != QFileDevice::NoError)
        return fail(descriptor.errorString());
    descriptorHash_ = QCryptographicHash::hash(descriptorBytes_, QCryptographicHash::Sha256);

    const QByteArray text = descriptorBytes_;
    int start = 0;
    while (start < text.size()) {
        const int newline = text.indexOf('\n', start);
        const int end = newline < 0 ? text.size() : newline;
        descriptorLines_.push_back(text.mid(start, end - start));
        start = newline < 0 ? text.size() : newline + 1;
    }
    if (text.isEmpty() || text.endsWith('\n')) descriptorLines_.push_back(QByteArray());

    bool foundHeader = false;
    bool foundCreateType = false;
    bool foundCid = false;
    bool foundParentCid = false;
    quint64 totalSectors = 0;
    std::set<QString> names;
    const QRegularExpression extentPattern(
        QStringLiteral("^RW\\s+([0-9]+)\\s+(SPARSE|FLAT)\\s+\\\"([^\\\"]+)\\\"(?:\\s+([0-9]+))?\\s*$"));
    QString createType;
    for (int lineIndex = 0; lineIndex < int(descriptorLines_.size()); ++lineIndex) {
        const QByteArray& rawLine = descriptorLines_[static_cast<std::size_t>(lineIndex)];
        const QString line = QString::fromLocal8Bit(rawLine).trimmed();
        if (line.contains(QStringLiteral("# Disk DescriptorFile"))) foundHeader = true;
        if (line.startsWith(QStringLiteral("CID="))) {
            static const QRegularExpression cidPattern(QStringLiteral("^CID=[0-9a-fA-F]{1,8}$"));
            if (foundCid || !cidPattern.match(line).hasMatch())
                return fail(QStringLiteral("VMDK descriptor CID is invalid."));
            foundCid = true;
        }
        if (line.startsWith(QStringLiteral("parentCID="))) {
            if (foundParentCid || line != QStringLiteral("parentCID=ffffffff"))
                return fail(QStringLiteral("Parent VMDK disks are not supported."));
            foundParentCid = true;
        }
        if (line.startsWith(QStringLiteral("parentFileNameHint")))
            return fail(QStringLiteral("Parent VMDK disks are not supported."));
        if (line.startsWith(QStringLiteral("createType="))) {
            if (foundCreateType)
                return fail(QStringLiteral("VMDK descriptor has duplicate createType fields."));
            foundCreateType = true;
            createType = line.mid(QStringLiteral("createType=").size());
            if (createType != QStringLiteral("\"twoGbMaxExtentSparse\"") &&
                createType != QStringLiteral("\"twoGbMaxExtentFlat\"") &&
                createType != QStringLiteral("\"monolithicFlat\""))
                return fail(QStringLiteral("Unsupported VMDK createType for update."));
        }
        if (line.startsWith(QStringLiteral("RW "))) {
            const QRegularExpressionMatch match = extentPattern.match(line);
            if (!match.hasMatch())
                return fail(QStringLiteral("Unsupported VMDK extent descriptor line."));
            bool ok = false;
            const quint64 sectors = match.captured(1).toULongLong(&ok);
            const QString type = match.captured(2);
            const QString name = match.captured(3);
            bool offsetOk = true;
            const quint64 fileOffsetSectors = match.captured(4).isEmpty()
                ? 0 : match.captured(4).toULongLong(&offsetOk);
            if (!ok || !sectors || sectors > quint64((std::numeric_limits<qint64>::max)()) / kSectorSize ||
                !offsetOk || fileOffsetSectors > quint64((std::numeric_limits<qint64>::max)()) / kSectorSize ||
                name == QStringLiteral(".") || name == QStringLiteral("..") ||
                QFileInfo(name).isAbsolute() || name.contains(QLatin1Char('/')) ||
                name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char(':')) ||
                names.count(name.toCaseFolded()) ||
                (type == QStringLiteral("SPARSE") && !match.captured(4).isEmpty()))
                return fail(QStringLiteral("VMDK extent name or sector count is invalid."));
            const bool flatDisk = createType == QStringLiteral("\"monolithicFlat\"") ||
                createType == QStringLiteral("\"twoGbMaxExtentFlat\"");
            if (!createType.isEmpty() && flatDisk != (type == QStringLiteral("FLAT")))
                return fail(QStringLiteral("VMDK createType and extent type do not match."));
            names.insert(name.toCaseFolded());
            if (totalSectors > quint64((std::numeric_limits<qint64>::max)()) / kSectorSize - sectors)
                return fail(QStringLiteral("VMDK disk capacity overflows supported limits."));
            Extent extent;
            extent.sourceName = name;
            extent.sourceType = type;
            extent.fileOffset = qint64(fileOffsetSectors) * kSectorSize;
            extent.sourcePath = QFileInfo(descriptorPath_).dir().filePath(name);
            extent.start = qint64(totalSectors) * kSectorSize;
            extent.size = qint64(sectors) * kSectorSize;
            extent.descriptorLine = lineIndex;
#ifdef Q_OS_WIN
            if (writable_) {
                HANDLE guard = CreateFileW(
                    reinterpret_cast<LPCWSTR>(extent.sourcePath.utf16()), GENERIC_READ,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (guard == INVALID_HANDLE_VALUE)
                    return fail(QStringLiteral("VMDK extent '%1' is in use or cannot be write-guarded.")
                                .arg(name));
                extent.sourceGuard = std::shared_ptr<void>(guard, [](void* handle) {
                    CloseHandle(static_cast<HANDLE>(handle));
                });
            }
#endif
            extent.source.reset(new QFile(extent.sourcePath));
            if (!extent.source->open(QIODevice::ReadOnly))
                return fail(QStringLiteral("Cannot open VMDK extent '%1': %2")
                            .arg(name, extent.source->errorString()));
            extent.sourceSize = extent.source->size();
            extent.sourceModified = QFileInfo(extent.sourcePath).lastModified();
            if (writable_ && QFileInfo(extent.sourcePath + QStringLiteral(".lck")).exists())
                return fail(QStringLiteral("A VMDK extent lock exists; refusing to update an active disk."));
            if (type == QStringLiteral("SPARSE")) {
                QString extentError;
                extent.reader.reset(new VmdkImageWriter(
                    extent.source.get(), false, true, &extentError));
                if (!extent.reader->valid())
                    return fail(QStringLiteral("Extent '%1': %2").arg(name, extentError));
            } else {
                extent.reader.reset(new VmdkFlatExtentAccess(
                    extent.source.get(), false, extent.fileOffset, extent.size));
            }
            if (!extent.reader->valid() || extent.reader->capacity() != extent.size)
                return fail(error && !error->isEmpty() ? *error :
                            QStringLiteral("VMDK extent geometry does not match its descriptor."));
            totalSectors += sectors;
            extents_.push_back(std::move(extent));
        } else if (line.startsWith(QStringLiteral("RDONLY ")) ||
                   line.startsWith(QStringLiteral("NOACCESS "))) {
            return fail(QStringLiteral("Read-only and inaccessible VMDK extents are not supported."));
        }
    }
    const bool flatDisk = createType == QStringLiteral("\"monolithicFlat\"") ||
        createType == QStringLiteral("\"twoGbMaxExtentFlat\"");
    for (const Extent& extent : extents_)
        if (flatDisk != (extent.sourceType == QStringLiteral("FLAT")))
            return fail(QStringLiteral("VMDK createType and extent type do not match."));
    if (!foundHeader || !foundCreateType || !foundCid || !foundParentCid ||
        extents_.empty() || !totalSectors ||
        (createType == QStringLiteral("\"monolithicFlat\"") && extents_.size() != 1))
        return fail(QStringLiteral("VMDK descriptor is not a supported standalone disk."));
    capacityBytes_ = qint64(totalSectors) * kSectorSize;
    if (writable_ && QFileInfo(descriptorPath_ + QStringLiteral(".lck")).exists())
        return fail(QStringLiteral("A VMDK descriptor lock exists; refusing to update an active disk."));
    return true;
}

bool VmdkSplitImageWriter::hasExternalLock(QString* error) const {
    if (!writable_) return false;
    if (QFileInfo(descriptorPath_ + QStringLiteral(".lck")).exists()) {
        if (error) *error = QStringLiteral("A VMDK descriptor lock exists; refusing to update an active disk.");
        return true;
    }
    for (const Extent& extent : extents_) {
        if (QFileInfo(extent.sourcePath + QStringLiteral(".lck")).exists()) {
            if (error) *error = QStringLiteral("A VMDK extent lock exists; refusing to update an active disk.");
            return true;
        }
    }
    return false;
}

QByteArray VmdkSplitImageWriter::hashFile(QFile* file, bool* ok) const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file || !file->seek(0)) {
        *ok = false;
        return QByteArray();
    }
    QByteArray buffer(int(kCopyChunkSize), Qt::Uninitialized);
    while (true) {
        const qint64 count = file->read(buffer.data(), buffer.size());
        if (count < 0) {
            *ok = false;
            return QByteArray();
        }
        if (!count) break;
        hash.addData(buffer.constData(), int(count));
    }
    *ok = file->error() == QFileDevice::NoError;
    return *ok ? hash.result() : QByteArray();
}

bool VmdkSplitImageWriter::stageExtent(Extent* extent, QString* error) {
    if (!extent || !writable_) return false;
    if (extent->writer) return true;
    const qint64 sizeBefore = extent->source->size();
    const QDateTime modifiedBefore = QFileInfo(extent->sourcePath).lastModified();
    if (sizeBefore != extent->sourceSize || modifiedBefore != extent->sourceModified) {
        if (error) *error = QStringLiteral("VMDK extent changed before it was staged.");
        return false;
    }
    extent->staged.reset(new QTemporaryFile);
    extent->staged->setFileTemplate(QFileInfo(descriptorPath_).dir().filePath(
        QStringLiteral(".peare-extent-%1-XXXXXX")
            .arg(descriptorKey_)));
    extent->staged->setAutoRemove(true);
    if (!extent->staged->open()) {
        if (error) *error = extent->staged->errorString();
        return false;
    }
#ifdef Q_OS_WIN
    if (!tryCloneAllocatedRanges(extent->source.get(), extent->staged.get()) &&
        !copyAllocatedRanges(extent->source.get(), extent->staged.get(), error))
        return false;
#else
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!copyFile(extent->source.get(), extent->staged.get(), &hash, error))
        return false;
    extent->stagedSourceHash = hash.result();
#endif
    if (!extent->staged->flush() || extent->source->size() != sizeBefore ||
        QFileInfo(extent->sourcePath).lastModified() != modifiedBefore) {
        if (error && error->isEmpty())
            *error = QStringLiteral("VMDK extent changed while it was staged.");
        return false;
    }
    if (extent->sourceType == QStringLiteral("SPARSE")) {
        extent->writer.reset(new VmdkImageWriter(extent->staged.get(), true, true, error));
    } else {
        extent->writer.reset(new VmdkFlatExtentAccess(
            extent->staged.get(), true, extent->fileOffset, extent->size));
    }
    return extent->writer->valid() && extent->writer->capacity() == extent->size;
}

bool VmdkSplitImageWriter::read(qint64 offset, char* data, qint64 length) const {
    if (!valid_ || !data || offset < 0 || length < 0 || offset > capacityBytes_ ||
        length > capacityBytes_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        std::size_t index = 0;
        while (index + 1 < extents_.size() &&
               position >= extents_[index + 1].start) ++index;
        const Extent& extent = extents_[index];
        const qint64 within = position - extent.start;
        const qint64 count = qMin(length - done, extent.size - within);
        const VmdkImageAccess* reader = extent.writer ? extent.writer.get() : extent.reader.get();
        if (!reader->read(within, data + done, count)) return false;
        done += count;
    }
    return true;
}

bool VmdkSplitImageWriter::write(qint64 offset, const char* data, qint64 length) {
    if (!valid_ || !writable_ || !data || offset < 0 || length < 0 ||
        offset > capacityBytes_ || length > capacityBytes_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        std::size_t index = 0;
        while (index + 1 < extents_.size() &&
               position >= extents_[index + 1].start) ++index;
        Extent& extent = extents_[index];
        const qint64 within = position - extent.start;
        const qint64 count = qMin(length - done, extent.size - within);
        if (!stageExtent(&extent, nullptr) || !extent.writer->write(within, data + done, count))
            return false;
        done += count;
    }
    return true;
}

bool VmdkSplitImageWriter::finish(QString* error) {
    if (!valid_ || !writable_) {
        if (error) *error = QStringLiteral("Split VMDK writer is not active.");
        return false;
    }
    bool hasChanges = false;
    for (Extent& extent : extents_) {
        if (!extent.writer) continue;
        hasChanges = true;
        if (!extent.writer->finish(error)) return false;
        if (extent.sourceType == QStringLiteral("SPARSE")) {
            VmdkImageWriter verify(extent.staged.get(), false, true, error);
            if (!verify.valid() || verify.capacity() != extent.size) return false;
        } else {
            VmdkFlatExtentAccess verify(
                extent.staged.get(), false, extent.fileOffset, extent.size);
            if (!verify.valid() || verify.capacity() != extent.size) return false;
        }
    }
    if (!hasChanges) {
        if (error) *error = QStringLiteral("The VMDK update contains no changed extents.");
        return false;
    }
    if (hasExternalLock(error)) return false;

    QFile descriptorCheck(descriptorPath_);
    if (!descriptorCheck.open(QIODevice::ReadOnly)) {
        if (error) *error = descriptorCheck.errorString();
        return false;
    }
    const QByteArray currentDescriptor = descriptorCheck.readAll();
    descriptorCheck.close();
    if (currentDescriptor != descriptorBytes_) {
        if (error) *error = QStringLiteral("The VMDK descriptor changed during the update.");
        return false;
    }
    for (const Extent& extent : extents_) {
        if (extent.source->size() != extent.sourceSize ||
            QFileInfo(extent.sourcePath).lastModified() != extent.sourceModified) {
            if (error) *error = QStringLiteral("A VMDK extent changed during the update.");
            return false;
        }
        if (!extent.writer) continue;
#ifdef Q_OS_WIN
        if (!extent.sourceGuard) {
            if (error) *error = QStringLiteral("A VMDK extent is not write-guarded.");
            return false;
        }
#else
        bool ok = false;
        const QByteArray sourceHash = hashFile(extent.source.get(), &ok);
        if (!ok || sourceHash != extent.stagedSourceHash) {
            if (error) *error = QStringLiteral("A VMDK extent changed during the update.");
            return false;
        }
#endif
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
    for (std::size_t i = 0; i < extents_.size(); ++i) {
        Extent& extent = extents_[i];
        if (!extent.writer) continue;
        const QString name = QStringLiteral("peare-%1-%2-%3.vmdk")
            .arg(descriptorKey_).arg(token).arg(i + 1, 3, 10, QLatin1Char('0'));
        const QString path = QFileInfo(descriptorPath_).dir().filePath(name);
        if (!extent.staged || !extent.staged->isOpen() || !extent.staged->flush() ||
            !syncFileToDisk(extent.staged.get(), error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("Could not synchronize the staged VMDK extent.");
            removePreparedFiles();
            return false;
        }
        extent.writer.reset();
        extent.staged->close();
        if (!extent.staged->rename(path)) {
            if (error) *error = QStringLiteral("Cannot publish VMDK extent without replacing an existing file: %1")
                .arg(extent.staged->errorString());
            removePreparedFiles();
            return false;
        }
        extent.staged->setAutoRemove(false);
        extent.committedName = name;
        preparedFiles_.push_back(path);
    }

#ifndef Q_OS_WIN
    if (!syncDirectoryToDisk(QFileInfo(descriptorPath_).absolutePath(), error)) {
        removePreparedFiles();
        return false;
    }
#endif

    for (Extent& extent : extents_) {
        if (extent.committedName.isEmpty()) continue;
        QByteArray& line = descriptorLines_[static_cast<std::size_t>(extent.descriptorLine)];
        const int firstQuote = line.indexOf('"');
        const int secondQuote = line.indexOf('"', firstQuote + 1);
        if (firstQuote < 0 || secondQuote < 0) {
            if (error) *error = QStringLiteral("VMDK extent descriptor changed unexpectedly.");
            removePreparedFiles();
            return false;
        }
        line = line.left(firstQuote + 1) + extent.committedName.toLatin1() +
            line.mid(secondQuote);
    }
    QByteArray updatedDescriptor;
    for (std::size_t i = 0; i < descriptorLines_.size(); ++i) {
        updatedDescriptor += descriptorLines_[i];
        if (i + 1 < descriptorLines_.size() || descriptorBytes_.endsWith('\n'))
            updatedDescriptor += '\n';
    }
    if (hasExternalLock(error)) {
        removePreparedFiles();
        return false;
    }
    QSaveFile descriptorOutput(descriptorPath_);
    if (!descriptorOutput.open(QIODevice::WriteOnly) ||
        descriptorOutput.write(updatedDescriptor) != updatedDescriptor.size()) {
        if (error) *error = descriptorOutput.errorString();
        removePreparedFiles();
        return false;
    }
    if (!syncFileToDisk(&descriptorOutput, error)) {
        removePreparedFiles();
        return false;
    }
    if (!descriptorOutput.commit()) {
        if (error) *error = descriptorOutput.errorString();
        removePreparedFiles();
        return false;
    }
    committed_ = true;
    return true;
}

void VmdkSplitImageWriter::removePreparedFiles() {
    for (const QString& path : preparedFiles_) QFile::remove(path);
    preparedFiles_.clear();
}

void VmdkSplitImageWriter::cleanupOrphanedFiles() {
    const QRegularExpression stagedPattern(
        QStringLiteral("^\\.peare-extent-%1-[A-Za-z0-9]{6}$").arg(descriptorKey_));
    const QRegularExpression publishedPattern(
        QStringLiteral("^peare-%1-[0-9a-f]{32}-[0-9]{3}\\.vmdk$")
            .arg(descriptorKey_));
    std::set<QString> referenced;
    for (const Extent& extent : extents_)
        referenced.insert(QFileInfo(extent.sourcePath).fileName());

    QDir directory(QFileInfo(descriptorPath_).absolutePath());
    const QFileInfoList files = directory.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    auto referencedBySiblingDescriptor = [&](const QFileInfo& candidate) {
        QByteArray quotedName("\"");
        quotedName += candidate.fileName().toUtf8();
        quotedName += '\"';
        for (const QFileInfo& sibling : files) {
            if (sibling.absoluteFilePath() == descriptorPath_ ||
                sibling.suffix().compare(QStringLiteral("vmdk"), Qt::CaseInsensitive) != 0 ||
                sibling.size() > 1024 * 1024) continue;
            QFile descriptor(sibling.absoluteFilePath());
            if (!descriptor.open(QIODevice::ReadOnly)) return true;
            const QByteArray contents = descriptor.readAll();
            if (descriptor.error() != QFileDevice::NoError || contents.contains(quotedName))
                return true;
        }
        return false;
    };
    for (const QFileInfo& file : files) {
        const QString name = file.fileName();
        if (file.isSymLink() || !file.isFile() || referenced.count(name) ||
            (!stagedPattern.match(name).hasMatch() &&
             !publishedPattern.match(name).hasMatch()) ||
            referencedBySiblingDescriptor(file)) continue;
        QFile::remove(file.absoluteFilePath());
    }
}

}  // namespace writer
}  // namespace peare
