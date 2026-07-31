#pragma once

#include "Module.h"

namespace peare {

class XuizModule final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<XuizModule> open(const QString& filePath);
    static std::unique_ptr<XuizModule> open(const QByteArray& data, const QString& logicalName);
    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }
private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
