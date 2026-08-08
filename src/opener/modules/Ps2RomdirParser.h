#pragma once

#include "../fs/DiscStore.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {

struct Ps2RomdirEntryInfo {
    QString name;
    quint16 extInfoSize = 0;
    quint32 fileSize = 0;
    quint64 dataOffset = 0;
    quint64 extInfoOffset = 0;
    QString extInfoSummary;
};

struct Ps2RomdirImageInfo {
    quint64 baseOffset = 0;
    quint32 romdirSize = 0;
    quint32 extinfoSize = 0;
    QVector<Ps2RomdirEntryInfo> entries;
};

bool parsePs2Romdir(const fs::ByteStorePtr& store, quint64 baseOffset,
                    Ps2RomdirImageInfo* image, QString* error = nullptr);
qint64 findPs2Romdir(const fs::ByteStorePtr& store,
                     quint64 scanLimit = 16ULL * 1024ULL * 1024ULL);
qint64 findPs2Romdir(const QByteArray& data);

} // namespace peare
