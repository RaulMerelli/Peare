#pragma once

#include "Module.h"

#include <vector>

namespace peare {

// Microsoft Full Flash Update (FFU) image. The parser follows the layout used
// by Windows Phone Internals: security/image/store headers describe a sparse
// block stream which is projected as one or more GPT disks. Peare exposes the
// real GPT partitions, not the FFU's technical header records.
class FfuModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    fs::ByteStorePtr file_;
    std::vector<fs::ByteStorePtr> disks_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
