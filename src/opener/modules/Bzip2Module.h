#pragma once

#include "Module.h"

#include <vector>

namespace peare {

class Bzip2Module final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<Bzip2Module> open(const QString& filePath);
    static std::unique_ptr<Bzip2Module> open(const fs::ByteStorePtr& source,
                                             const QString& logicalName);
    static std::unique_ptr<Bzip2Module> open(const QByteArray& data, const QString& logicalName);
    static std::unique_ptr<Bzip2Module> open(std::vector<std::uint8_t> data,
                                             const QString& logicalName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
