#pragma once

#include "Module.h"

namespace peare {

// Windows Phone Store XAP envelope protected with PlayReady. The clear-text
// header describes one encrypted payload: the original ZIP/XAP archive. There
// is no clear central directory to enumerate before decryption.
class PlayReadyXapModule final : public IModule, public IResourceContainer {
public:
    static bool isHeader(const QByteArray& data);

    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName);

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
    fs::ByteStorePtr file_;
};

} // namespace peare
