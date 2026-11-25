#include "FatModule.h"
#include "Compat.h"

#include "../fs/FatReader.h"
#include "../fs/TibLegacyReader.h"
#include "../fs/TibxVolumeStore.h"

#include <QFile>

#include <utility>

namespace peare {

ModulePtr FatModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath) {
    auto module = peare::makeUnique<FatModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::FAT;
    module->info_.description = QStringLiteral("FAT volume");

    fs::ByteStorePtr filter;
    const auto legacy = std::dynamic_pointer_cast<fs::TibLegacyFilteredVolume>(disc);
    if (legacy) filter = legacy->fatClusterFilter();
    const auto tibx = std::dynamic_pointer_cast<fs::TibxVolumeStore>(disc);
    if (tibx) {
        const auto status = tibx->openFatClusterFilter(&filter);
        if (status != fs::TibxVolumeStore::StreamResult::Found &&
            status != fs::TibxVolumeStore::StreamResult::NotFound) {
            module->info_.error = QStringLiteral("Cannot resolve TIBX FAT cluster filter");
            return ModulePtr(std::move(module));
        }
    }
    auto reader = std::make_shared<fs::FatReader>(disc, filter);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    std::vector<fs::DiscEntry> entries;
    std::vector<fs::ByteStorePtr> contents;
    if (!reader->listWithContent(subPath.toStdString(), &entries, &contents)) {
        module->info_.error = QStringLiteral("Cannot read FAT directory");
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName()) + QStringLiteral(" volume");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const fs::DiscEntry& item = entries[i];
        ResourceEntry entry;
        entry.name = QString::fromUtf8(item.name.c_str());
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::FAT;
        if (item.isDirectory) {
            entry.type = QStringLiteral("FAT_DIR");
            entry.isDirectory = true;
            entry.containerSubPath = subPath.isEmpty() ? entry.name : subPath + QLatin1Char('/') + entry.name;
            entry.content = disc;
        } else {
            entry.type = QStringLiteral("FAT_FILE");
            entry.isEmbeddedFile = true;
            entry.dataSize = quint64(item.length);
            entry.content = contents[i];
        }
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

ModulePtr FatModule::open(const QString& filePath) {
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<FatModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::FAT;
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
