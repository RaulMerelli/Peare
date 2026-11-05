#pragma once

#include "Module.h"

namespace peare {

class SiemensAdonisModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);

    // Strict enough for direct file opening: validates the ARM vector shape and
    // requires Siemens/ADONIS markers somewhere in the complete image.
    static bool isImage(const QByteArray& data) noexcept;

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
