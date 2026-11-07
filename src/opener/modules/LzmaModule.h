#pragma once

#include "Module.h"

namespace peare {

class LzmaModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& source, const QString& logicalName);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
    static bool hasExtension(const QString& fileName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
