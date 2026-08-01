#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Opener module for WIM (Windows Imaging) images. Thin bridge over the
// DiscUtils-compatible fs stack (fs::WimReader): files are exposed as
// ResourceEntry nodes whose content is a lazy chunked (LZX/XPRESS) resource,
// served through the common opener ABI like any other format.
class WimModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::DiscFileSystemPtr fs_;
};

}  // namespace peare
