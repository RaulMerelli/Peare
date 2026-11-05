#pragma once

#include "Module.h"

namespace peare {

// Parse CUE sheets and expose referenced BIN files and track views.
// Normalize data tracks to 2048-byte sectors for ISO/UDF readers.
class CueBinModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    QVector<fs::ByteStorePtr> backingStores_;
};

} // namespace peare
