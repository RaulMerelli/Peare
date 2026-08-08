#pragma once
#include "Module.h"
namespace peare {
class WadModule final : public IModule, public IResourceContainer {
public:
 static ModulePtr open(const QString& path);
 static ModulePtr open(const QByteArray& data,const QString& name);
 static ModulePtr open(const fs::ByteStorePtr& file,const QString& name);
 const ModuleInfo& info() const noexcept override { return info_; }
 const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }
private: ModuleInfo info_; fs::ByteStorePtr file_; QVector<ResourceEntry> resources_;
};
}
