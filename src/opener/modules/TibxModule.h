#pragma once

#include "Module.h"

#include <QByteArray>
#include <utility>
#include <vector>

namespace peare {

class TibxModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& path, const QString& password = QString(),
                          const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());
    static ModulePtr openParts(const std::vector<std::pair<fs::ByteStorePtr, QString> >& parts,
                               const QString& name, const QString& password = QString(),
                               const QByteArray& tibxPrivateKey = QByteArray(),
                               const QString& tibxKeyPassphrase = QString());
    static ModulePtr open(const fs::ByteStorePtr& source, const QString& name,
                          const QString& subPath = QString(), const QString& password = QString(),
                          const QByteArray& tibxPrivateKey = QByteArray(),
                          const QString& tibxKeyPassphrase = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

}  // namespace peare
