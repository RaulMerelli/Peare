#include "VmdkModule.h"
#include "Compat.h"

#include "../fs/VmdkDisk.h"
#include "../fs/PartitionTable.h"
#include "../fs/QFileByteStore.h"

#include <QDir>
#include <QFileInfo>

#include <string>
#include <utility>

namespace peare {
namespace {

fs::ByteStorePtr storeForFile(const QString& path) {
    auto store = std::make_shared<fs::QFileByteStore>(path);
    return store->validate() ? store : nullptr;
}

}  // namespace

ModulePtr VmdkModule::buildFromDisk(const fs::ByteStorePtr& disk, const QString& sourceName,
                                    const QString& error) {
    auto module = peare::makeUnique<VmdkModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::VMDK;
    module->info_.description = QStringLiteral("VMware Virtual Disk");
    if (!disk) {
        module->info_.error = error.isEmpty() ? QStringLiteral("Cannot open VMDK") : error;
        return ModulePtr(std::move(module));
    }
    module->disk_ = disk;

    std::vector<fs::PartitionInfo> parts = fs::readPartitionTable(disk);
    const auto contextStores = fs::makePartitionContextStores(disk, parts);
    auto addPartition = [&](const QString& name, std::int64_t off, std::int64_t len,
                            const fs::ByteStorePtr& content = fs::ByteStorePtr()) {
        ResourceEntry entry;
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.isEmbeddedFile = true;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(len);
        entry.dataOffset = quint64(off);
        entry.content = content ? content : std::make_shared<fs::SubStore>(disk, off, len);
        module->resources_.push_back(std::move(entry));
    };

    if (parts.empty()) {
        addPartition(QStringLiteral("Whole disk"), 0, disk->capacity());
    } else {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const fs::PartitionInfo& p = parts[i];
            const QString partitionName = p.typeName.empty()
                ? QStringLiteral("Partition %1").arg(i + 1)
                : QStringLiteral("Partition %1 — %2").arg(i + 1)
                      .arg(QString::fromStdString(p.typeName));
            addPartition(partitionName, p.offset, p.length, p.content ? p.content : contextStores[i]);
        }
    }
    return ModulePtr(std::move(module));
}

ModulePtr VmdkModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    std::string err;
    fs::ByteStorePtr disk = fs::openVmdkDisk(file, &err);  // KDMV single extent
    return buildFromDisk(disk, sourceName, QString::fromStdString(err));
}

ModulePtr VmdkModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<VmdkModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::VMDK;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    // Resolve sibling extent files (split disks) relative to the descriptor's dir.
    const QDir dir = QFileInfo(filePath).absoluteDir();
    auto resolver = [dir](const std::string& name) -> fs::ByteStorePtr {
        return storeForFile(dir.absoluteFilePath(QString::fromStdString(name)));
    };

    std::string err;
    fs::ByteStorePtr disk = fs::openVmdkDisk(file, resolver, &err);
    return buildFromDisk(disk, filePath, QString::fromStdString(err));
}

}  // namespace peare
