#pragma once

#include "../fs/DiscStore.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace peare {

struct OleCompoundStream {
    QString name;
    QStringList hierarchyPath;
    quint64 size = 0;
    quint64 dataOffset = 0;
    bool miniStream = false;
    fs::ByteStorePtr content;
};

struct OleCompoundContents {
    QVector<OleCompoundStream> streams;
    int storageCount = 0;
};

bool hasOleCompoundMagic(const QByteArray& data);
fs::ByteStorePtr oleStoreForFile(const QString& path);
bool enumerateOleCompound(const fs::ByteStorePtr& file,
                          OleCompoundContents* contents,
                          QString* error = nullptr);

} // namespace peare
