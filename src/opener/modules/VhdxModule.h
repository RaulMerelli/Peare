#pragma once

#include "Module.h"

#include "../fs/DiscStore.h"

namespace peare {

class VhdxModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    static ModulePtr buildFromDisk(const fs::ByteStorePtr& disk, const QString& sourceName,
                                   const QString& error);

    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr disk_;
};

}  // namespace peare
