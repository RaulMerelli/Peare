#pragma once

#include "Module.h"

#include <vector>

namespace peare {

class XzModule final : public IModule, public IResourceContainer {
public:
    static std::unique_ptr<XzModule> open(const QString& filePath);
    static std::unique_ptr<XzModule> open(const fs::ByteStorePtr& source,
                                          const QString& logicalName);
    static std::unique_ptr<XzModule> open(const QByteArray& data, const QString& logicalName);
    static std::unique_ptr<XzModule> open(std::vector<std::uint8_t> data,
                                          const QString& logicalName);
    static bool decodePayload(const std::vector<std::uint8_t>& data,
                              std::vector<std::uint8_t>* output);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
