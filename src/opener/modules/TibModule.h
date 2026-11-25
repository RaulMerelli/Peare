#pragma once

#include "Module.h"

namespace peare {

class TibModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& path, const QString& password = QString());
    static ModulePtr openParts(const std::vector<std::pair<fs::ByteStorePtr, QString> >& parts,
                               const QString& password = QString());
    static ModulePtr openMac(const fs::ByteStorePtr& source, const fs::ByteStorePtr& companion,
                             const QString& name, const QString& companionName);
    static ModulePtr open(const fs::ByteStorePtr& source, const QString& name,
                          const QString& subPath = QString(), const QString& password = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
