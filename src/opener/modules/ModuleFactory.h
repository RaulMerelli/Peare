#pragma once

#include "Module.h"

#include <QByteArray>
#include <utility>
#include <vector>

namespace peare {

class ModuleFactory final {
public:
    static ModulePtr open(const QString& filePath, const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());
    static ModulePtr openWithPassword(const QString& filePath, const QString& password,
                                      const QByteArray& tibxPrivateKey = QByteArray(),
                                      const QString& tibxKeyPassphrase = QString());
    static ModulePtr open(const QString& physicalPath, const QString& logicalName,
                          const QString& password = QString(), const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());
    static ModulePtr open(const QByteArray& data, const QString& logicalName,
                          const QString& password = QString(), const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());
    // Open over a positioned byte source. Filesystem formats (ISO, WIM) are read
    // straight from the source with no materialisation; other formats fall back
    // to reading it fully.
    static ModulePtr open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath = QString(),
                          const QString& password = QString(), const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());
    static ModulePtr openParts(const std::vector<std::pair<fs::ByteStorePtr, QString> >& parts,
                               const QString& password = QString(), const QByteArray& tibxPrivateKey = QByteArray(),
                               const QString& tibxKeyPassphrase = QString());
};

} // namespace peare
