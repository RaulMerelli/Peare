#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace peare {
namespace writer {

enum class RarArchiveVersion {
    Rar4,
    Rar5,
    Rar7
};

class RarArchiveWriter final {
public:
    RarArchiveWriter();
    ~RarArchiveWriter();

    bool beginCreate(const QString& outputPath, RarArchiveVersion version,
                     unsigned compressionMethod, QString* error);
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
    struct DataChunk {
        QString path;
        qint64 offset;
        quint64 size;

        DataChunk() : offset(0), size(0) {}
        DataChunk(const QString& sourcePath, qint64 sourceOffset,
                  quint64 sourceSize)
            : path(sourcePath), offset(sourceOffset), size(sourceSize) {}
    };

    struct Entry {
        QByteArray name;
        QString stagedPath;
        QByteArray rawHeader;
        qint64 rawDataOffset = 0;
        quint64 rawDataSize = 0;
        quint64 unpackedSize = 0;
        quint64 dictionarySize = 128 * 1024;
        quint32 unpackedCrc = 0;
        unsigned packedMethod = 0;
        unsigned algorithmVersion = 0;
        std::vector<DataChunk> rawChunks;
        int rawNameOffset = -1;
        int rawNameLengthOffset = -1;
        int rawNameLengthBytes = 0;
        int rawHeaderSizeLength = 0;
        bool directory = false;
        bool raw = false;
        bool editable = true;
        bool solid = false;
        bool encrypted = false;
        bool extendedDistances = false;
        bool split = false;
        bool recoveryRecord = false;
        bool hasRar4Salt = false;
        quint8 encryptionFlags = 0;
        quint8 kdfCount = 0;
        std::array<std::uint8_t, 8> rar4Salt = {};
        std::array<std::uint8_t, 16> rar5Salt = {};
        std::array<std::uint8_t, 16> rar5Iv = {};
    };

    bool stageFile(const QString& sourcePath, QString* stagedPath,
                   QString* error);
    bool parseUpdateArchive(QString* error);
    bool clearSolidFlag(QString* error);
    bool clearRar5SolidFlag(QString* error);
    bool materializeSolidMembers(QString* error);
    bool materializeRar5SolidMembers(QString* error);
    bool readPackedData(const Entry& entry, std::vector<std::uint8_t>* packed,
                        QString* error) const;
    bool rewriteRawName(Entry* entry, const QByteArray& name, QString* error);
    bool writeArchive(QString* error);
    bool writeRar4MultiVolumeArchive(QString* error);
    bool writeRar5MultiVolumeArchive(QString* error);
    bool writeRar4RecoveryArchive(QString* error);
    bool writeRar5RecoveryArchive(QString* error);

    QString outputPath_;
    QString updateSourcePath_;
    QString password_;
    QByteArray archivePrefix_;
    QByteArray archiveEnd_;
    std::vector<QString> sourceVolumePaths_;
    quint64 volumeSizeLimit_ = 0;
    RarArchiveVersion version_ = RarArchiveVersion::Rar5;
    unsigned compressionMethod_ = 0;
    std::size_t nextStageIndex_ = 0;
    std::unique_ptr<QTemporaryDir> staging_;
    std::vector<Entry> entries_;
    bool active_ = false;
    bool updateMode_ = false;
    bool archiveSolid_ = false;
    bool archiveNeedsRewrite_ = false;
    bool archiveMultiVolume_ = false;
    bool archiveRecovery_ = false;
    quint32 recoverySectors_ = 0;
    quint8 recoveryPercent_ = 0;
    bool writingRecoveryArchive_ = false;
    bool capabilityProbe_ = false;
};

} // namespace writer
} // namespace peare
