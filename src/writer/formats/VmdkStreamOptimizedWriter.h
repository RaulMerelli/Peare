#ifndef PEARE_WRITER_VMDK_STREAM_OPTIMIZED_WRITER_H
#define PEARE_WRITER_VMDK_STREAM_OPTIMIZED_WRITER_H

#include <QByteArray>
#include <QFile>
#include <QString>

#include <map>
#include <vector>

#include "VmdkImageAccess.h"

namespace peare {
namespace writer {

class VmdkStreamOptimizedWriter final : public VmdkImageAccess {
public:
    static bool convertSparseToStreamOptimized(QFile* file,
                                               const QString& outputName,
                                               QString* error);

    VmdkStreamOptimizedWriter(QFile* file, bool writable,
                              const QString& outputName = QString(),
                              QString* error = nullptr);

    bool valid() const override { return valid_; }
    qint64 capacity() const override { return capacityBytes_; }
    bool read(qint64 offset, char* data, qint64 length) const override;
    bool write(qint64 offset, const char* data, qint64 length) override;
    bool finish(QString* error) override;

private:
    struct GrainRef {
        qint64 offset = 0;
        quint32 size = 0;

        GrainRef() = default;
        GrainRef(qint64 grainOffset, quint32 grainSize)
            : offset(grainOffset), size(grainSize) {}
    };

    bool parse(QString* error);
    bool loadGrain(qint64 index, QByteArray* bytes, QString* error = nullptr) const;
    bool convertToSparse(QString* error);
    bool readFile(qint64 offset, char* data, qint64 length) const;

    QFile* file_ = nullptr;
    bool writable_ = false;
    bool valid_ = false;
    bool streamOptimized_ = false;
    qint64 capacityBytes_ = 0;
    qint64 capacitySectors_ = 0;
    qint64 grainSectors_ = 0;
    qint64 grainBytes_ = 0;
    qint64 entriesPerTable_ = 0;
    qint64 overheadBytes_ = 0;
    qint64 descriptorOffset_ = 0;
    qint64 descriptorLength_ = 0;
    QString outputName_;
    QByteArray descriptor_;
    std::map<qint64, GrainRef> grains_;
    std::map<qint64, std::vector<char>> changedGrains_;
    mutable qint64 cacheIndex_ = -1;
    mutable QByteArray cache_;
};

}  // namespace writer
}  // namespace peare

#endif
