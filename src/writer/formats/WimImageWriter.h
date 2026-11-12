#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <vector>

namespace peare {
namespace writer {

class WimImageWriter final {
public:
    WimImageWriter();
    ~WimImageWriter();

    bool beginCreate(const QString& outputPath, const QByteArray& variant,
                     QString* error);
    bool beginUpdate(const QString& imagePath, QString* error);
    static bool canUpdate(const QString& imagePath);
    bool addFile(const QString& sourcePath, const QByteArray& entryName,
                 QString* error);
    bool addEmptyFile(const QByteArray& entryName, QString* error);
    bool addDirectory(const QByteArray& entryName, QString* error);
    bool replaceFile(const QString& sourcePath, const QByteArray& entryName,
                     QString* error);
    bool renameEntry(const QByteArray& oldName, const QByteArray& newName,
                     QString* error);
    bool deleteEntry(const QByteArray& entryName, bool recursive,
                     QString* error);
    bool finish(QString* error);
    void abort();

private:
    struct Entry {
        QByteArray name;
        QString stagedPath;
        QByteArray originalHash;
        QByteArray shortName;
        QByteArray streamData;
        std::vector<QByteArray> alternateStreamHashes;
        quint32 securityId = 0;
        quint16 streamCount = 0;
        int primaryStreamHashOffset = -1;
        bool fromExisting = false;
        bool directory = false;
        quint32 attributes = 0;
        quint64 creationTime = 0;
        quint64 accessTime = 0;
        quint64 writeTime = 0;
    };

    struct ExistingResource {
        QByteArray hash;
        QByteArray tableRecord;
        quint64 size = 0;
        quint64 offset = 0;
        quint64 originalSize = 0;
        quint32 refs = 0;
        quint8 flags = 0;
    };

    bool stageFile(const QString& sourcePath, QString* stagedPath,
                   QString* error);
    bool loadExisting(const QString& imagePath, QString* error);
    bool writeImage(QString* error);

    QString outputPath_;
    QString sourcePath_;
    QByteArray sourceHeader_;
    QByteArray sourceSecurityTable_;
    QByteArray sourceXml_;
    std::vector<ExistingResource> sourceResources_;
    std::size_t selectedMetadataIndex_ = 0;
    quint32 imageCount_ = 1;
    std::unique_ptr<QTemporaryDir> staging_;
    std::vector<Entry> entries_;
    quint64 nextStagedFile_ = 0;
    quint32 compressionFlag_ = 0;
    quint32 chunkSize_ = 0;
    quint32 integrityChunkSize_ = 0;
    bool active_ = false;
};

}  // namespace writer
}  // namespace peare
