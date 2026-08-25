#pragma once

#include "Module.h"

namespace peare {

class AsarModule final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<AsarModule> open(const QString& filePath);
    static std::unique_ptr<AsarModule> open(const QString& physicalPath, const QString& logicalName);
    static std::unique_ptr<AsarModule> open(const QByteArray& data, const QString& logicalName);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    // Strong prefix check for Electron's Pickle-wrapped JSON header. It needs
    // only the first bytes and is therefore safe for nested-container probing.
    static bool isHeader(const QByteArray& data);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    static ModulePtr openStore(const fs::ByteStorePtr& file, const QString& sourceName,
                               const QString& physicalPath);

    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr file_;
};

} // namespace peare
