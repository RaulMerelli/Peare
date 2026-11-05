#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Open ISO 9660 and Joliet images through the lazy file-system reader.
class IsoModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    // Open directly over a positioned byte source (no materialisation): used when
    // an ISO is nested inside another container.
    static ModulePtr open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::DiscFileSystemPtr fs_;  // keeps the reader (and its disc) alive
};

}  // namespace peare
