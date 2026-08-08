#pragma once
#include "Module.h"
#include "../fs/DiscStore.h"
namespace peare {
class ParallelsModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString&);
    static ModulePtr open(const fs::ByteStorePtr&, const QString&);
    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr disk_;
};
} // namespace peare
