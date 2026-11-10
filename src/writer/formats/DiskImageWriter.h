#ifndef PEARE_WRITER_DISK_IMAGE_WRITER_H
#define PEARE_WRITER_DISK_IMAGE_WRITER_H

#include <QByteArray>
#include <QString>

#include <memory>
#include <cstddef>
#include <vector>

namespace peare { namespace writer { class VhdxImageWriter; class VhdImageWriter; class VmdkImageAccess; } }

class QTemporaryFile;
class QLockFile;

namespace peare {
namespace writer {

struct DiskImageIoContext;
class NtfsVolumeUpdate;
class ExtVolumeUpdate;

class ExternalWriteGuard;

struct DiskImageTarget {
    qint64 offset = 0;
    qint64 size = 0;
    QString label;
    bool efiSystem = false;
    bool xdf = false;
    bool ntfs = false;
    bool ext = false;
    bool ext3 = false;
    bool ext4 = false;
    bool exfat = false;
    qint64 protectedOffset = 0;
    qint64 protectedSize = 0;
};

class DiskImageUpdate final {
public:
    static std::vector<DiskImageTarget> targets(const QString& imagePath);
    static bool canUpdate(const QString& imagePath);

    DiskImageUpdate();
    ~DiskImageUpdate();

    bool begin(const QString& imagePath, QString* error);
    bool begin(const QString& imagePath, size_t targetIndex, QString* error,
               qint64 journalOffset = 0, qint64 journalSize = 0);
    bool beginNew(const QString& imagePath, const QString& variant, QString* error);
    bool beginNew(const QString& imagePath, const QString& format,
                  const QString& variant, const QString& filesystem, QString* error);
    bool addFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                 QString* error);
    bool createDirectory(const QByteArray& entryNameUtf8, QString* error);
    bool replaceFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                     QString* error);
    bool enableFsVerity(const QByteArray& entryNameUtf8, QString* error);
    bool renameEntry(const QByteArray& entryNameUtf8,
                     const QByteArray& newEntryNameUtf8, QString* error);
    bool deleteEntry(const QByteArray& entryNameUtf8, bool recursive, QString* error);
    bool finish(QString* error);
    void abort();

private:
    std::unique_ptr<QTemporaryFile> staging_;
    std::unique_ptr<QLockFile> updateLock_;
    std::unique_ptr<ExternalWriteGuard> externalWriteGuard_;
    QString imagePath_;
    QByteArray originalSha256_;
    qint64 originalSize_ = 0;
    bool createTargetExisted_ = false;
    qint64 imageLogicalSize_ = 0;
    qint64 fatVolumeOffset_ = 0;
    qint64 fatVolumeSize_ = 0;
    qint64 fatSectorSize_ = 512;
    bool xdfVolume_ = false;
    bool vmdkSplitCreate_ = false;
    bool vmdkStreamOptimizedCreate_ = false;
    int vmdkFlatCreateMode_ = 0;
    qint64 xdfProtectedOffset_ = 0;
    QByteArray xdfProtectedBytes_;
    std::unique_ptr<VhdxImageWriter> vhdx_;
    std::unique_ptr<VhdImageWriter> vhd_;
    std::unique_ptr<VmdkImageAccess> vmdk_;
    std::unique_ptr<DiskImageIoContext> ntfsContext_;
    std::unique_ptr<NtfsVolumeUpdate> ntfsUpdate_;
    std::unique_ptr<DiskImageIoContext> extContext_;
    std::unique_ptr<DiskImageIoContext> extJournalContext_;
    std::unique_ptr<ExtVolumeUpdate> extUpdate_;
    bool ntfsVolume_ = false;
    bool extVolume_ = false;
    qint64 extJournalOffset_ = 0;
    qint64 extJournalSize_ = 0;
    bool splitVmdk_ = false;
    bool createMode_ = false;
};

}  // namespace writer
}  // namespace peare

#endif
