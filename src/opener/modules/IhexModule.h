#pragma once

#include "Module.h"

namespace peare {

class IhexModule final : public IModule, public IResourceContainer {
public:
    static bool hasHeader(const QByteArray& data);
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& logicalName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

}  // namespace peare
