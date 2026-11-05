#pragma once

#include "Module.h"

#include <vector>

namespace peare {

class LhaModule final : public IModule, public IResourceContainer {
public:
    static bool hasHeader(const QByteArray& data);
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
    static ModulePtr open(std::vector<std::uint8_t> data, const QString& logicalName);
    static ModulePtr open(const fs::ByteStorePtr& source, const QString& logicalName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    static ModulePtr openNative(const std::vector<std::uint8_t>& data,
                                const QString& logicalName);

    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

}  // namespace peare
