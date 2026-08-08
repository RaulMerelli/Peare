#pragma once

#include "Module.h"
#include "../fs/DiscFileSystem.h"

namespace peare {

class FloppyImageModule final : public IModule, public IResourceContainer {
public:
    static bool hasSupportedExtension(const QString& sourceName);
    static ModulePtr open(const fs::ByteStorePtr& image, const QString& sourceName,
                          const QString& subPath = QString());
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QString& physicalPath, const QString& logicalName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr image_;
    std::shared_ptr<fs::IDiscFileSystem> fs_;
};

} // namespace peare
