#include "RegistryModule.h"

#include "Compat.h"
#include "FsLevel.h"

#include "../fs/RegistryHiveReader.h"

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

ModulePtr RegistryModule::open(const fs::ByteStorePtr& store, const QString& sourceName,
                               const QString& subPath) {
    auto module = peare::makeUnique<RegistryModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::REGISTRY;

    fs::RegistryHiveReader reader(store);
    if (!reader.valid()) {
        module->info_.error = QString::fromStdString(reader.error());
        return ModulePtr(std::move(module));
    }

    module->info_.description = QString::fromStdString(reader.friendlyName());
    buildFsLevel(reader, store, subPath.toStdString(), ModuleFormat::REGISTRY,
                 QStringLiteral("REG_VALUE"), QStringLiteral("REG_KEY"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr RegistryModule::open(const QString& filePath) {
    fs::ByteStorePtr store = storeForFile(filePath);
    if (!store) {
        auto module = peare::makeUnique<RegistryModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::REGISTRY;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(store, filePath);
}

}  // namespace peare

