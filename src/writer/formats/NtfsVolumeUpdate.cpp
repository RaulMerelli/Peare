#include "NtfsVolumeUpdate.h"

#ifdef PEARE_NTFS_WRITER
#include <QFile>
#include <QtEndian>

#include <cstring>

namespace {

bool plausibleBootSector(const peare_ntfs_io& io) {
    if (io.size < 512 || !io.read) return false;
    char boot[512];
    if (!io.read(io.context, 0, boot, sizeof(boot))) return false;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(boot);
    const quint16 bytesPerSector = qFromLittleEndian<quint16>(bytes + 11);
    const unsigned char sectorsPerCluster = bytes[13];
    const quint64 totalSectors = qFromLittleEndian<quint64>(bytes + 40);
    const quint64 mftCluster = qFromLittleEndian<quint64>(bytes + 48);
    const quint64 mirrorCluster = qFromLittleEndian<quint64>(bytes + 56);
    const qint8 recordSize = static_cast<qint8>(bytes[64]);
    const bool valid = std::memcmp(boot + 3, "NTFS    ", 8) == 0 &&
        static_cast<unsigned char>(boot[510]) == 0x55 &&
        static_cast<unsigned char>(boot[511]) == 0xaa &&
        (bytesPerSector == 512 || bytesPerSector == 1024 ||
         bytesPerSector == 2048 || bytesPerSector == 4096) &&
        sectorsPerCluster && !(sectorsPerCluster & (sectorsPerCluster - 1)) &&
        sectorsPerCluster <= 128 && totalSectors &&
        totalSectors <= quint64(io.size / bytesPerSector) &&
        mftCluster < totalSectors / sectorsPerCluster &&
        mirrorCluster < totalSectors / sectorsPerCluster &&
        ((recordSize < 0 && recordSize >= -31 && recordSize <= -9) ||
         (recordSize > 0 && recordSize <= 128 &&
          !(recordSize & (recordSize - 1)))) && totalSectors >= 2;
    return valid;
}

bool normalizeNtfsPath(const QByteArray& utf8, QString* path, QString* error) {
    if (!path || utf8.isEmpty()) {
        if (error) *error = QStringLiteral("A non-empty NTFS entry path is required.");
        return false;
    }
    const QString name = QString::fromUtf8(utf8.constData(), utf8.size());
    if (name.isEmpty() || name.toUtf8() != utf8 || name.contains(QChar(0)) ||
        name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        (name.size() > 1 && name.at(1) == QLatin1Char(':'))) {
        if (error) *error = QStringLiteral("The NTFS entry path is invalid UTF-8 or absolute.");
        return false;
    }
    const QStringList parts = name.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") ||
            part == QStringLiteral("..") || part.size() > 255) {
            if (error) *error = QStringLiteral("The NTFS entry path contains an invalid component.");
            return false;
        }
    }
    *path = name;
    return true;
}

int64_t readSource(void* opaque, void* buffer, int64_t length) {
    QFile* source = static_cast<QFile*>(opaque);
    if (!source || !buffer || length < 0) return -1;
    return source->read(static_cast<char*>(buffer), qint64(length));
}

bool ensureParents(peare_ntfs_handle* handle, const QString& path, QString* error) {
    int slash = path.indexOf(QLatin1Char('/'));
    while (slash >= 0) {
        const QByteArray parent = path.left(slash).toUtf8();
        if (peare_ntfs_create_directory(handle, parent.constData())) {
            if (error) *error = QStringLiteral("NTFS could not create or validate a parent directory.");
            return false;
        }
        slash = path.indexOf(QLatin1Char('/'), slash + 1);
    }
    return true;
}

}  // namespace

namespace peare {
namespace writer {

NtfsVolumeUpdate::NtfsVolumeUpdate() = default;

NtfsVolumeUpdate::~NtfsVolumeUpdate() {
    abort();
}

bool NtfsVolumeUpdate::probe(const peare_ntfs_io& io) {
    if (!plausibleBootSector(io)) return false;
    const int result = peare_ntfs_probe(&io);
    return result == 0;
}

bool NtfsVolumeUpdate::begin(const peare_ntfs_io& io, QString* error) {
    if (handle_ || failed_) return fail(error, QStringLiteral("An NTFS update is already active."));
    if (peare_ntfs_open(&io, &handle_))
        return fail(error, QStringLiteral("The NTFS volume could not be mounted for update."));
    return true;
}

bool NtfsVolumeUpdate::fail(QString* error, const QString& message) {
    failed_ = true;
    if (error) *error = message;
    return false;
}

bool NtfsVolumeUpdate::writeFile(const QString& sourcePath,
                                 const QByteArray& entryNameUtf8,
                                 bool replace, QString* error) {
    if (!handle_ || failed_) return fail(error, QStringLiteral("No active NTFS update is available."));
    QString path;
    if (!normalizeNtfsPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(error, source.errorString());
    }
    const qint64 sourceSize = source.size();
    if (sourceSize < 0) return fail(error, source.errorString());
    if (!ensureParents(handle_, path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray pathUtf8 = path.toUtf8();
    peare_ntfs_source input = {&source, sourceSize, readSource};
    const int writeResult = peare_ntfs_write_file(handle_, pathUtf8.constData(), &input, replace);
    if (writeResult)
        return fail(error, QStringLiteral("NTFS failed to create or replace the file."));
    if (source.size() != sourceSize || !source.atEnd())
        return fail(error, QStringLiteral("The source file changed during the NTFS update."));
    dirty_ = true;
    return true;
}

bool NtfsVolumeUpdate::addFile(const QString& sourcePath,
                               const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, false, error);
}

bool NtfsVolumeUpdate::replaceFile(const QString& sourcePath,
                                   const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, true, error);
}

bool NtfsVolumeUpdate::createDirectory(const QByteArray& entryNameUtf8,
                                       QString* error) {
    if (!handle_ || failed_) return fail(error, QStringLiteral("No active NTFS update is available."));
    QString path;
    if (!normalizeNtfsPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    if (!ensureParents(handle_, path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray pathUtf8 = path.toUtf8();
    if (peare_ntfs_create_directory(handle_, pathUtf8.constData()))
        return fail(error, QStringLiteral("NTFS failed to create the directory."));
    dirty_ = true;
    return true;
}

bool NtfsVolumeUpdate::renameEntry(const QByteArray& entryNameUtf8,
                                   const QByteArray& newEntryNameUtf8,
                                   QString* error) {
    if (!handle_ || failed_) return fail(error, QStringLiteral("No active NTFS update is available."));
    QString oldPath, newPath;
    if (!normalizeNtfsPath(entryNameUtf8, &oldPath, error) ||
        !normalizeNtfsPath(newEntryNameUtf8, &newPath, error)) {
        failed_ = true;
        return false;
    }
    if (newPath == oldPath) return true;
    if (newPath.startsWith(oldPath + QLatin1Char('/')))
        return fail(error, QStringLiteral("An NTFS directory cannot be moved inside itself."));
    if (!ensureParents(handle_, newPath, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray oldUtf8 = oldPath.toUtf8();
    const QByteArray newUtf8 = newPath.toUtf8();
    if (peare_ntfs_rename(handle_, oldUtf8.constData(), newUtf8.constData()))
        return fail(error, QStringLiteral("NTFS failed to rename the entry."));
    dirty_ = true;
    return true;
}

bool NtfsVolumeUpdate::deleteEntry(const QByteArray& entryNameUtf8,
                                   bool recursive, QString* error) {
    if (!handle_ || failed_) return fail(error, QStringLiteral("No active NTFS update is available."));
    QString path;
    if (!normalizeNtfsPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray pathUtf8 = path.toUtf8();
    if (peare_ntfs_delete(handle_, pathUtf8.constData(), recursive ? 1 : 0))
        return fail(error, QStringLiteral("NTFS failed to delete the entry."));
    dirty_ = true;
    return true;
}

bool NtfsVolumeUpdate::finish(QString* error) {
    if (!handle_) return fail(error, QStringLiteral("No active NTFS update is available."));
    if (failed_) {
        abort();
        return fail(error, QStringLiteral("The NTFS transaction failed and cannot be committed."));
    }
    const int result = peare_ntfs_close(handle_);
    handle_ = nullptr;
    if (result) return fail(error, QStringLiteral("NTFS could not flush and close the staged volume."));
    return true;
}

void NtfsVolumeUpdate::abort() {
    if (handle_) {
        peare_ntfs_close(handle_);
        handle_ = nullptr;
    }
}

}  // namespace writer
}  // namespace peare
#else
namespace peare {
namespace writer {

NtfsVolumeUpdate::NtfsVolumeUpdate() = default;
NtfsVolumeUpdate::~NtfsVolumeUpdate() = default;

bool NtfsVolumeUpdate::probe(const peare_ntfs_io&) { return false; }

bool NtfsVolumeUpdate::begin(const peare_ntfs_io&, QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

bool NtfsVolumeUpdate::fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

bool NtfsVolumeUpdate::writeFile(const QString&, const QByteArray&, bool,
                                 QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

bool NtfsVolumeUpdate::addFile(const QString& sourcePath,
                               const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, false, error);
}

bool NtfsVolumeUpdate::replaceFile(const QString& sourcePath,
                                   const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, true, error);
}

bool NtfsVolumeUpdate::createDirectory(const QByteArray&, QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

bool NtfsVolumeUpdate::renameEntry(const QByteArray&, const QByteArray&,
                                   QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

bool NtfsVolumeUpdate::deleteEntry(const QByteArray&, bool, QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

bool NtfsVolumeUpdate::finish(QString* error) {
    return fail(error, QStringLiteral("NTFS writing is unavailable in this build."));
}

void NtfsVolumeUpdate::abort() {}

}  // namespace writer
}  // namespace peare
#endif
