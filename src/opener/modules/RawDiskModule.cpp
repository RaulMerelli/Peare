#include "RawDiskModule.h"
#include "Compat.h"

#include "../fs/PartitionTable.h"

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

ModulePtr RawDiskModule::open(const fs::ByteStorePtr& disk, const QString& sourceName) {
    auto module = peare::makeUnique<RawDiskModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::RAW_DISK;
    module->info_.description = QStringLiteral("Raw disk image");
    if (!disk) {
        module->info_.error = QStringLiteral("Cannot open raw disk image");
        return ModulePtr(std::move(module));
    }
    module->disk_ = disk;

    const std::vector<fs::PartitionInfo> parts = fs::readPartitionTable(disk);
    if (parts.empty()) {
        module->info_.error = QStringLiteral("No MBR/GPT partition table found");
        return ModulePtr(std::move(module));
    }

    for (std::size_t i = 0; i < parts.size(); ++i) {
        const fs::PartitionInfo& p = parts[i];
        ResourceEntry entry;
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.isEmbeddedFile = true;
        entry.name = QStringLiteral("Partition %1 (%2)")
                         .arg(i + 1)
                         .arg(QString::fromStdString(p.typeName));
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(p.offset);
        entry.dataSize = quint64(p.length);
        entry.format = ModuleFormat::RAW_DISK;
        entry.content = std::make_shared<fs::SubStore>(disk, p.offset, p.length);
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

ModulePtr RawDiskModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

}  // namespace peare
