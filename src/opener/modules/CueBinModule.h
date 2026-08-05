#pragma once

#include "Module.h"

namespace peare {

// CDRWIN BIN/CUE image opener. The CUE sheet is parsed in-process; referenced
// BINARY files are mapped lazily and exposed both as source files and as
// per-track views. Data tracks are normalised to 2048-byte logical sectors so
// the existing ISO9660/UDF readers can open them recursively.
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
