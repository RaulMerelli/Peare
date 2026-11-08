#pragma once

#include "Module.h"

namespace peare {

class AdzModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const fs::ByteStorePtr& source, const QString& sourceName,
                          const QString& subPath = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    ModulePtr floppy_;
};

} // namespace peare
