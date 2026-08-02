#pragma once

#include "Module.h"

namespace peare {

class DmgModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);
    static ModulePtr open(const QString& filePath);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

}  // namespace peare
