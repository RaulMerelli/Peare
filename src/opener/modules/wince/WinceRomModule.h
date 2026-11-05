#pragma once

// Ported from the MIT-licensed CERF project by Yaroslav Kibysh (gweslab).
// See CERF-LICENSE.txt in this directory.

#include "../Module.h"

namespace peare {

// Open Windows CE ROM, IMGFS, and supported firmware wrappers as file trees.
// Unknown firmware wrappers are not recognized.
class WinceRomModule final : public IModule, public IResourceContainer {
public:
    static ModulePtr open(const QString& filePath, const QString& subPath = QString());
    static ModulePtr open(const QByteArray& data, const QString& logicalName,
                          const QString& subPath = QString());
    static ModulePtr open(const fs::ByteStorePtr& file, const QString& sourceName,
                          const QString& subPath = QString());

    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    fs::ByteStorePtr source_;
    fs::ByteStorePtr flat_;
    QVector<ResourceEntry> resources_;
};

} // namespace peare
