#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

class UfsModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::DiscFileSystemPtr fs_;
};

}  // namespace peare
