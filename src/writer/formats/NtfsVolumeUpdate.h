#ifndef PEARE_WRITER_NTFS_VOLUME_UPDATE_H
#define PEARE_WRITER_NTFS_VOLUME_UPDATE_H

#include "third_party/ntfs-3g/libntfs-3g/peare_bridge.h"

#include <QByteArray>
#include <QString>

namespace peare {
namespace writer {

class NtfsVolumeUpdate final {
public:
    NtfsVolumeUpdate();
    ~NtfsVolumeUpdate();

    static bool probe(const peare_ntfs_io& io);
    bool begin(const peare_ntfs_io& io, QString* error);
    bool addFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                 QString* error);
    bool createDirectory(const QByteArray& entryNameUtf8, QString* error);
    bool replaceFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                     QString* error);
    bool renameEntry(const QByteArray& entryNameUtf8,
                     const QByteArray& newEntryNameUtf8, QString* error);
    bool deleteEntry(const QByteArray& entryNameUtf8, bool recursive, QString* error);
    bool finish(QString* error);
    void abort();
    bool dirty() const { return dirty_; }

private:
    bool writeFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                   bool replace, QString* error);
    bool fail(QString* error, const QString& message);

    peare_ntfs_handle* handle_ = nullptr;
    bool failed_ = false;
    bool dirty_ = false;
};

}  // namespace writer
}  // namespace peare

#endif
