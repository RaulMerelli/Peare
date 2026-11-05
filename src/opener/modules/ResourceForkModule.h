#pragma once

#include "Module.h"

namespace peare {

class ResourceForkModule final : public IModule, public IResourceContainer {
public:
    static bool hasCandidateHeader(const QByteArray& prefix, quint64 totalSize);
    static bool hasHeader(const QByteArray& data);
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QByteArray& data, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
