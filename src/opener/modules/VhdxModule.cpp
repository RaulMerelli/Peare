#include "VhdxModule.h"
#include "Compat.h"

#include "../fs/PartitionTable.h"
#include "../fs/VhdxDisk.h"

#include <QFile>

#include <cstdint>
#include <string>
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

ModulePtr VhdxModule::buildFromDisk(const fs::ByteStorePtr& disk, const QString& sourceName,
                                    const QString& error) {
    auto module = peare::makeUnique<VhdxModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::VHDX;
    module->info_.description = QStringLiteral("Microsoft Virtual Hard Disk v2");
    if (!disk) {
        module->info_.error = error.isEmpty() ? QStringLiteral("Cannot open VHDX") : error;
        return ModulePtr(std::move(module));
    }
    module->disk_ = disk;

    std::vector<fs::PartitionInfo> parts = fs::readPartitionTable(disk);
    auto addPartition = [&](const QString& name, std::int64_t off, std::int64_t len) {
        ResourceEntry entry;
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.isEmbeddedFile = true;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(len);
        entry.dataOffset = quint64(off);
        entry.content = std::make_shared<fs::SubStore>(disk, off, len);
        module->resources_.push_back(std::move(entry));
    };

    if (parts.empty()) {
        addPartition(QStringLiteral("Whole disk"), 0, disk->capacity());
    } else {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const fs::PartitionInfo& p = parts[i];
            addPartition(QStringLiteral("Partition %1 (%2)")
                             .arg(i + 1)
                             .arg(QString::fromStdString(p.typeName)),
                         p.offset, p.length);
        }
    }
    return ModulePtr(std::move(module));
}

ModulePtr VhdxModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    std::string err;
    fs::ByteStorePtr disk = fs::openVhdxDisk(file, &err);
    return buildFromDisk(disk, sourceName, QString::fromStdString(err));
}

ModulePtr VhdxModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<VhdxModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::VHDX;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
