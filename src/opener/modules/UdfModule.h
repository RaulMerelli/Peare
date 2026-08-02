#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Opener module for UDF (Universal Disk Format) optical images. Thin bridge over
// fs::UdfReader; files are UDF_FILE resources with lazy content, served through
// the common opener ABI.
class UdfModule final : public IModule, public IResourceContainer {
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
