#pragma once

#include "Module.h"

#include "../fs/DiscStore.h"

namespace peare {

// Opener module for VMware Virtual Disk (VMDK) images. It exposes the logical
// disk's partitions (MBR/GPT) as nested containers: each partition's content is a
// window over the disk, so the file-system openers (FAT, exFAT, ...) open inside
// it through the common lazy nesting path.
class VmdkModule final : public IModule, public IResourceContainer {
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
    fs::ByteStorePtr disk_;  // keeps the logical-disk store alive for the windows
};

}  // namespace peare
