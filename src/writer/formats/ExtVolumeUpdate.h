#ifndef PEARE_WRITER_EXT_VOLUME_UPDATE_H
#define PEARE_WRITER_EXT_VOLUME_UPDATE_H

#include <QByteArray>
#include <QString>

#include <memory>
#include <mutex>

struct ext4_blockdev;

struct peare_ext_io {
    void* context;
    int64_t size;
    int (*read)(void* context, int64_t offset, void* buffer, int64_t length);
    int (*write)(void* context, int64_t offset, const void* buffer, int64_t length);
    int (*flush)(void* context);
};

namespace peare {
namespace writer {

class ExtVolumeUpdate final {
public:
    ExtVolumeUpdate();
    ~ExtVolumeUpdate();

    static bool probe(const peare_ext_io& io, bool* ext3 = nullptr,
                      bool* ext4 = nullptr, QString* error = nullptr);
    static bool format(const peare_ext_io& io, bool ext3, bool ext4,
                       QString* error);
    bool begin(const peare_ext_io& io, QString* error,
               const peare_ext_io* journalIo = nullptr);
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
    bool dirty() const { return dirty_; }

private:
    struct State;

    static int blockOpen(struct ext4_blockdev* device);
    static int blockClose(struct ext4_blockdev* device);
    static int blockRead(struct ext4_blockdev* device, void* buffer,
                         uint64_t blockId, uint32_t blockCount);
    static int blockWrite(struct ext4_blockdev* device, const void* buffer,
                          uint64_t blockId, uint32_t blockCount);
    bool acquireMmp(QString* error);
    bool pulseMmp();
    bool releaseMmp(QString* error);
    bool unshareSharedBlocks(QString* error);
    bool mount(const peare_ext_io& io, bool readOnly, QString* error,
               const peare_ext_io* journalIo = nullptr);
    bool unmount(QString* error);
    bool updateQuotaAccounting(QString* error);
    bool fail(QString* error, const QString& message);
    bool writeFile(const QString& sourcePath, const QByteArray& entryNameUtf8,
                   bool replace, QString* error);
    bool replaceVerityFile(const QString& sourcePath, const QString& path,
                           const QByteArray& destination, qint64 sourceSize,
                           QString* error);
    bool checkDeleteMetadata(const QByteArray& path, bool directory,
                             bool recursive, QString* error);

    peare_ext_io io_ = {};
    peare_ext_io journalIo_ = {};
    bool hasJournalIo_ = false;
    std::unique_ptr<State> state_;
    std::unique_lock<std::mutex> lock_;
    bool registered_ = false;
    bool journalRegistered_ = false;
    bool mounted_ = false;
    bool failed_ = false;
    bool dirty_ = false;
    bool quotaDirty_ = false;
    bool journalStarted_ = false;
};

}  // namespace writer
}  // namespace peare

#endif
