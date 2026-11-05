#ifndef PEARE_WRITER_VHDX_IMAGE_WRITER_H
#define PEARE_WRITER_VHDX_IMAGE_WRITER_H

#include <QFile>
#include <QString>

#include <cstdint>
#include <map>
#include <vector>

namespace peare {
namespace writer {

class VhdxImageWriter final {
public:
    explicit VhdxImageWriter(QFile* file, bool writable, QString* error = nullptr);

    bool valid() const { return valid_; }
    qint64 capacity() const { return diskSize_; }
    qint64 logicalSectorSize() const { return logicalSectorSize_; }
    bool read(qint64 offset, char* data, qint64 length) const;
    bool write(qint64 offset, const char* data, qint64 length);
    bool finish(QString* error);

private:
    bool parse(QString* error);
    bool readFile(qint64 offset, char* data, qint64 length) const;
    bool writeFile(qint64 offset, const char* data, qint64 length);
    qint64 batEntryOffset(qint64 block) const;

    QFile* file_ = nullptr;
    bool writable_ = false;
    bool valid_ = false;
    qint64 diskSize_ = 0;
    qint64 logicalSectorSize_ = 512;
    qint64 activeHeaderOffset_ = 0;
    std::uint64_t activeHeaderSequence_ = 0;
    qint64 logOffset_ = 0;
    qint64 logLength_ = 0;
    qint64 batOffset_ = 0;
    qint64 batLength_ = 0;
    qint64 blockSize_ = 0;
    qint64 chunkRatio_ = 0;
    std::vector<std::uint64_t> bat_;
    std::map<qint64, std::vector<char>> changedBlocks_;
};

}  // namespace writer
}  // namespace peare

#endif
