#pragma once

#include "Module.h"

#include <vector>

namespace peare {

// Parse FFU security, image, and store headers as sparse GPT disk images.
// Expose GPT partitions instead of FFU header records.
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
