#ifndef PEARE_WRITER_VHD_IMAGE_WRITER_H
#define PEARE_WRITER_VHD_IMAGE_WRITER_H

#include <QFile>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace peare {
namespace writer {

class VhdImageWriter final {
public:
    explicit VhdImageWriter(QFile* file, bool writable, QString* error = nullptr);

    bool valid() const { return valid_; }
    qint64 capacity() const { return diskSize_; }
    bool read(qint64 offset, char* data, qint64 length) const;
    bool write(qint64 offset, const char* data, qint64 length);
    bool finish(QString* error);

private:
    bool parse(QString* error);
    bool readFile(qint64 offset, char* data, qint64 length) const;
    bool writeFile(qint64 offset, const char* data, qint64 length);

    QFile* file_ = nullptr;
    bool writable_ = false;
    bool valid_ = false;
    qint64 diskSize_ = 0;
    qint64 headerOffset_ = 0;
    qint64 tableOffset_ = 0;
    qint64 blockSize_ = 0;
    qint64 bitmapSize_ = 0;
    qint64 requiredBlocks_ = 0;
    std::vector<quint32> bat_;
    std::vector<QByteArray> bitmaps_;
    std::map<qint64, std::vector<char>> changedBlocks_;
    std::map<qint64, std::set<qint64>> changedBitmapBytes_;
};

}  // namespace writer
}  // namespace peare

#endif
