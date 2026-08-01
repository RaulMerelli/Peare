#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Opener module for ISO 9660 (+ Joliet) images. It is a thin bridge: the actual
// parsing and lazy content live in the DiscUtils-compatible fs stack
// (fs::Iso9660Reader over an fs::IByteStore). Files are exposed as ordinary
// ResourceEntry nodes whose content is a lazy fs window, so the common opener
// ABI serves them like any other format without knowing a layered stack is
// behind them.
class IsoModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::DiscFileSystemPtr fs_;  // keeps the reader (and its disc) alive
};

}  // namespace peare
