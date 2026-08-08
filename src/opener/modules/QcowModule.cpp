#include "QcowModule.h"
#include "Compat.h"

#include "../fs/PartitionTable.h"
#include "../fs/QcowDisk.h"

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
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        std::size_t(bytes.size()));
}

ModuleFormat formatForQcowFile(const fs::ByteStorePtr& file)
{
    if (!file) return ModuleFormat::QCOW;
    const std::vector<std::uint8_t> header = file->readRange(0, 8);
    if (header.size() < 8 || header[0] != 0x51 || header[1] != 0x46 ||
        header[2] != 0x49 || header[3] != 0xFB)
        return ModuleFormat::QCOW;
    const std::uint32_t version = (std::uint32_t(header[4]) << 24) |
        (std::uint32_t(header[5]) << 16) | (std::uint32_t(header[6]) << 8) |
        std::uint32_t(header[7]);
    return version == 1 ? ModuleFormat::QCOW : ModuleFormat::QCOW2;
}

}  // namespace

ModulePtr QcowModule::buildFromDisk(const fs::ByteStorePtr& disk,
                                    const QString& sourceName,
                                    ModuleFormat format,
                                    const QString& error) {
    auto module = peare::makeUnique<QcowModule>();
    module->info_.filePath = sourceName;
    module->info_.format = format;
    module->info_.description = format == ModuleFormat::QCOW2
        ? QStringLiteral("QEMU Copy-On-Write disk image (QCOW2)")
        : QStringLiteral("QEMU Copy-On-Write disk image (QCOW)");
    if (!disk) {
        module->info_.error = error.isEmpty() ? QStringLiteral("Cannot open QCOW image") : error;
        return ModulePtr(std::move(module));
    }
    module->disk_ = disk;

    const std::vector<fs::PartitionInfo> parts = fs::readPartitionTable(disk);
    auto addPartition = [&](const QString& name, std::int64_t off, std::int64_t len,
                            const fs::ByteStorePtr& content = fs::ByteStorePtr()) {
        ResourceEntry entry;
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.isEmbeddedFile = true;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(len);
        entry.dataOffset = quint64(off);
        entry.format = format;
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
            addPartition(partitionName, p.offset, p.length, p.content);
        }
    }
    return ModulePtr(std::move(module));
}

ModulePtr QcowModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    const ModuleFormat format = formatForQcowFile(file);
    std::string err;
    const fs::ByteStorePtr disk = fs::openQcowDisk(file, &err);
    return buildFromDisk(disk, sourceName, format, QString::fromStdString(err));
}

ModulePtr QcowModule::open(const QString& filePath) {
    const fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<QcowModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::QCOW;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
