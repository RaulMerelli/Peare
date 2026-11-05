#ifndef PEARE_WRITER_VMDK_SPLIT_IMAGE_WRITER_H
#define PEARE_WRITER_VMDK_SPLIT_IMAGE_WRITER_H

#include "VmdkImageAccess.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QTemporaryFile>

#include <memory>
#include <vector>

class QLockFile;

namespace peare {
namespace writer {

class VmdkImageWriter;

class VmdkSplitImageWriter final : public VmdkImageAccess {
public:
    VmdkSplitImageWriter(const QString& descriptorPath, bool writable,
                         QString* error = nullptr);
    ~VmdkSplitImageWriter() override;

    bool valid() const override { return valid_; }
    qint64 capacity() const override { return capacityBytes_; }
    bool read(qint64 offset, char* data, qint64 length) const override;
    bool write(qint64 offset, const char* data, qint64 length) override;
    bool finish(QString* error) override;

private:
    struct Extent {
        QString sourcePath;
        QString sourceName;
        QString sourceType;
        QString committedName;
        qint64 start = 0;
        qint64 size = 0;
        qint64 fileOffset = 0;
        qint64 sourceSize = 0;
        QDateTime sourceModified;
        int descriptorLine = -1;
        std::unique_ptr<QFile> source;
        std::shared_ptr<void> sourceGuard;
        std::unique_ptr<VmdkImageAccess> reader;
        std::unique_ptr<QTemporaryFile> staged;
        std::unique_ptr<VmdkImageAccess> writer;
        QByteArray stagedSourceHash;
    };

    bool parse(QString* error);
    void cleanupOrphanedFiles();
    bool hasExternalLock(QString* error) const;
    bool stageExtent(Extent* extent, QString* error);
    QByteArray hashFile(QFile* file, bool* ok) const;
    void removePreparedFiles();

    QString descriptorPath_;
    QString descriptorKey_;
    std::unique_ptr<QLockFile> updateLock_;
    QByteArray descriptorBytes_;
    QByteArray descriptorHash_;
    std::vector<QByteArray> descriptorLines_;
    std::vector<Extent> extents_;
    std::vector<QString> preparedFiles_;
    qint64 capacityBytes_ = 0;
    bool writable_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

}  // namespace writer
}  // namespace peare

#endif
