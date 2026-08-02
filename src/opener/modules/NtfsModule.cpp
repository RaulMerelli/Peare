#include "NtfsModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/NtfsReader.h"

#include <QFile>

#include <utility>

namespace peare {

ModulePtr NtfsModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                           const QString& subPath) {
    auto module = peare::makeUnique<NtfsModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::NTFS;
    module->info_.description = QStringLiteral("NTFS volume");

    auto reader = std::make_shared<fs::NtfsReader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName()) + QStringLiteral(" volume");
    buildFsLevel(*reader, disc, subPath.toStdString(), ModuleFormat::NTFS,
                 QStringLiteral("NTFS_FILE"), QStringLiteral("NTFS_DIR"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr NtfsModule::open(const QString& filePath) {
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<NtfsModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::NTFS;
        module->info_.error = holder->errorString();
        return ModulePtr(std::move(module));
    }
    const qint64 size = holder->size();
    fs::ByteStorePtr disc;
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        disc = std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    } else {
        const QByteArray bytes = holder->readAll();
        disc = std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
    }
    return open(disc, filePath);
}

}  // namespace peare
