#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Expose files from WIM images through lazy LZX/XPRESS resources.
class WimModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    // Open directly over a positioned byte source (no materialisation): used when
    // a WIM is nested inside another container, so only the chunks actually read
    // are decoded.
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
