#include "DynamicDiskModule.h"
#include "Compat.h"

#include "../fs/DynamicDisk.h"

#include <QFile>

#include <utility>

namespace peare {
namespace {

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

}  // namespace

ModulePtr DynamicDiskModule::open(const fs::ByteStorePtr& disk, const QString& sourceName) {
    auto module = peare::makeUnique<DynamicDiskModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::DYNAMIC_DISK;
    module->info_.description = QStringLiteral("Windows Dynamic Disk (LDM)");
    if (!disk) {
        module->info_.error = QStringLiteral("Cannot open Windows Dynamic Disk");
        return ModulePtr(std::move(module));
    }
    module->disk_ = disk;

    std::string error;
    std::vector<fs::DynamicVolumeInfo> volumes = fs::readDynamicDiskVolumes(disk, &error);
    if (volumes.empty()) {
        module->info_.error = QString::fromStdString(error);
        return ModulePtr(std::move(module));
    }
    for (const fs::DynamicVolumeInfo& volume : volumes) {
        ResourceEntry entry;
        entry.type = QStringLiteral("LDM_VOLUME");
        entry.isEmbeddedFile = true;
        entry.name = QString::fromStdString(volume.name);
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(volume.sizeSectors * 512ULL);
        entry.format = ModuleFormat::DYNAMIC_DISK;
        entry.content = volume.content;
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

ModulePtr DynamicDiskModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

}  // namespace peare
