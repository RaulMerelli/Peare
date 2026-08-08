#pragma once

#include "Module.h"

namespace peare {

class ZipModule final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<ZipModule> open(const QString& filePath);
    static std::unique_ptr<ZipModule> open(const QString& physicalPath, const QString& logicalName);
    static std::unique_ptr<ZipModule> open(const QByteArray& data, const QString& logicalName);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr file_;
};

}  // namespace peare
