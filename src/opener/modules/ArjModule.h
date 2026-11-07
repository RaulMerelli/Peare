#pragma once

#include "Module.h"

namespace peare {

class ArjModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath, const QString& password = QString());
    static ModulePtr open(const QByteArray& data, const QString& logicalName,
                          const QString& password = QString());
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName,
                          const QString& password = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
