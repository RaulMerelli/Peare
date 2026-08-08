#pragma once

#include "Module.h"

namespace peare {

class MsiModule final : public IModule, public IResourceContainer {
public:
    static bool hasCompoundMagic(const QByteArray& data);
    static bool hasInstallerExtension(const QString& name);

    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    fs::ByteStorePtr file_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
