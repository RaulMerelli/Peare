#include "HfsPlusModule.h"

#include "Compat.h"
#include "FsLevel.h"

#include "../fs/HfsPlusReader.h"

#include <QFile>

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

ModulePtr HfsPlusModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                              const QString& subPath) {
    auto module = peare::makeUnique<HfsPlusModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::HFSPLUS;

    fs::HfsPlusReader reader(disc);
    if (!reader.valid()) {
        module->info_.error = QString::fromStdString(reader.error());
        return ModulePtr(std::move(module));
    }

    module->info_.description = QString::fromStdString(reader.friendlyName());
    buildFsLevel(reader, disc, subPath.toStdString(), ModuleFormat::HFSPLUS,
                 QStringLiteral("HFS_FILE"), QStringLiteral("HFS_DIR"),
                 module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr HfsPlusModule::open(const QString& filePath) {
    fs::ByteStorePtr disc = storeForFile(filePath);
    if (!disc) {
        auto module = peare::makeUnique<HfsPlusModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::HFSPLUS;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(disc, filePath);
}

}  // namespace peare
