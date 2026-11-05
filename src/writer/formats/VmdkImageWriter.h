#ifndef PEARE_WRITER_VMDK_IMAGE_WRITER_H
#define PEARE_WRITER_VMDK_IMAGE_WRITER_H

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdint>
#include <map>
#include <vector>
#include "VmdkImageAccess.h"

namespace peare {
namespace writer {

class VmdkImageWriter final : public VmdkImageAccess {
public:
    explicit VmdkImageWriter(QFile* file, bool writable, QString* error = nullptr);
    VmdkImageWriter(QFile* file, bool writable, bool externalExtent, QString* error);

    bool valid() const override { return valid_; }
    qint64 capacity() const override { return capacityBytes_; }
    bool read(qint64 offset, char* data, qint64 length) const override;
    bool write(qint64 offset, const char* data, qint64 length) override;
    bool finish(QString* error) override;

private:
    bool parse(QString* error);
    bool readFile(qint64 offset, char* data, qint64 length) const;
    bool writeFile(qint64 offset, const char* data, qint64 length);

    QFile* file_ = nullptr;
    bool writable_ = false;
    bool externalExtent_ = false;
    bool valid_ = false;
    bool hasRedundantDirectory_ = false;
    bool zeroedGrainEntries_ = false;
    qint64 capacityBytes_ = 0;
    qint64 capacitySectors_ = 0;
    qint64 grainSectors_ = 0;
    qint64 grainBytes_ = 0;
    qint64 entriesPerTable_ = 0;
    qint64 grainTablesPerDirectory_ = 0;
    qint64 grainTableBytes_ = 0;
    qint64 grainTableSpan_ = 0;
    qint64 grainDirectoryOffset_ = 0;
    qint64 redundantDirectoryOffset_ = 0;
    qint64 grainDirectorySpan_ = 0;
    qint64 descriptorOffset_ = 0;
    qint64 descriptorLength_ = 0;
    std::vector<quint32> grainDirectory_;
    std::vector<quint32> redundantDirectory_;
    std::vector<QByteArray> grainTables_;
    std::vector<QByteArray> redundantGrainTables_;
    std::map<qint64, std::vector<char>> changedGrains_;
};

}  // namespace writer
}  // namespace peare

#endif
