#include "LinuxRaidModule.h"
#include "Compat.h"

#include "../fs/LinuxRaid.h"

#include <QFile>

#include <utility>

namespace peare {
namespace {

const std::int64_t kSector = 512;

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

ModulePtr LinuxRaidModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<LinuxRaidModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::LINUX_RAID;
    module->info_.description = QStringLiteral("Linux MD RAID member");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open Linux RAID member");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    fs::LinuxRaidSuperblock sb;
    if (!fs::readLinuxRaidSuperblock(file, &sb)) {
        module->info_.error = QStringLiteral("Linux RAID superblock not found");
        return ModulePtr(std::move(module));
    }
    if (sb.raidLevel != 1) {
        module->info_.error = QStringLiteral("Unsupported Linux RAID level %1").arg(sb.raidLevel);
        return ModulePtr(std::move(module));
    }

    const std::uint64_t byteStart = sb.dataOffsetSectors * std::uint64_t(kSector);
    const std::uint64_t byteLength = sb.arraySizeSectors * std::uint64_t(kSector);
    ResourceEntry entry;
    entry.type = QStringLiteral("LINUX_RAID_VOLUME");
    entry.isEmbeddedFile = true;
    entry.name = QStringLiteral("%1 RAID1 volume").arg(QString::fromStdString(sb.arrayName));
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = quint64(byteStart);
    entry.dataSize = quint64(byteLength);
    entry.format = ModuleFormat::LINUX_RAID;
    entry.content = std::make_shared<fs::SubStore>(file, static_cast<std::int64_t>(byteStart),
                                                   static_cast<std::int64_t>(byteLength));
    module->resources_.push_back(std::move(entry));
    return ModulePtr(std::move(module));
}

ModulePtr LinuxRaidModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

}  // namespace peare
