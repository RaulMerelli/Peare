#pragma once

#include "Module.h"

namespace peare {

class NsisModule final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<NsisModule> open(const QString& filePath, quint32 headerOffset);
    static std::unique_ptr<NsisModule> open(const QByteArray& data, const QString& logicalName);
    static std::unique_ptr<NsisModule> open(const QByteArray& data, const QString& logicalName,
                                            quint32 headerOffset);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

}
