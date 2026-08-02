#pragma once

#include "Module.h"

namespace peare {

class NeModule final : public IModule,
                       public IResourceContainer {
public:
    static std::unique_ptr<NeModule> open(const QString& filePath);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
