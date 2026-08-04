#pragma once

#include "Module.h"

namespace peare {

// Windows CE ROM/filesystem opener. B000FF, raw NB0/XIP, NOSAJ,
// ARNOLDBOOTBLOCK, iPAQ NBF, CE 1.x/2.x structural ROMs and IMGFS are
// presented as a Windows directory containing the real embedded files.
// Non-archive firmware wrappers are intentionally not claimed.
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
