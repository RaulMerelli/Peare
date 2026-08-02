#pragma once

#include "Module.h"

namespace peare {

class PeModuleBase : public IModule,
                     public IResourceContainer {
public:
    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

protected:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
