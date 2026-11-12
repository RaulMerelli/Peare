#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <cstdint>
#include <memory>
#include <vector>

namespace peare {
namespace writer {

class SevenZipArchiveWriter final {
public:
    SevenZipArchiveWriter();
    ~SevenZipArchiveWriter();

    bool beginCreate(const QString& outputPath, const QByteArray& variant,
                     const QString& password, QString* error);
    bool beginUpdate(const QString& archivePath, const QString& password,
                     QString* error);
    static bool canUpdate(const QString& archivePath);
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
        QByteArray methodId;
        QByteArray methodProperties;
        quint64 size = 0;
        quint64 packedSize = 0;
        quint32 crc = 0;
        quint32 packedCrc = 0;
        qint64 sourceFolder = -1;
        bool directory = false;
        bool hasStream = false;
    };

    bool stageFile(const QString& sourcePath, QString* stagedPath,
                   quint64* size, quint32* crc, QString* error);
    bool parseVariant(const QByteArray& variant, QString* error);
    bool writeArchive(QString* error);

    QString outputPath_;
    std::unique_ptr<QTemporaryDir> staging_;
    std::vector<Entry> entries_;
    quint64 nextStagedFile_ = 0;
    QByteArray methodId_;
    QByteArray methodProperties_;
    QByteArray headerMethodId_;
    QByteArray headerMethodProperties_;
    QByteArray headerAesProperties_;
    QString password_;
    int compressionLevel_ = 5;
    quint64 solidBlockSize_ = 0;
    bool headerCompressed_ = false;
    bool headerEncrypted_ = false;
    bool dataEncrypted_ = false;
    bool active_ = false;
};

} // namespace writer
} // namespace peare
