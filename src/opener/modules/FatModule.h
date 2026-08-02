#pragma once

#include "Module.h"

#include "../fs/DiscFileSystem.h"

namespace peare {

// Opener module for FAT12/16/32 volumes (floppy images, EFI System Partitions,
// USB media). Thin bridge over fs::FatReader; files are FAT_FILE resources with
// lazy content, served through the common opener ABI.
class FatModule final : public IModule, public IResourceContainer {
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
