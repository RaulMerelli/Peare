#pragma once

#include "Module.h"

namespace peare {

class RawDiskModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& disk, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    fs::ByteStorePtr disk_;
    QVector<ResourceEntry> resources_;
};

}  // namespace peare
