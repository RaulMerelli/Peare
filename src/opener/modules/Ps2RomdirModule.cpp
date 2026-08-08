#include "Ps2RomdirModule.h"
#include "Ps2RomdirParser.h"
#include "Compat.h"

#include <QFile>
#include <memory>
#include <utility>

namespace peare {
namespace {

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    }
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

} // namespace

ModulePtr Ps2RomdirModule::open(const QString& filePath)
{
    return open(storeForFile(filePath), filePath);
}

ModulePtr Ps2RomdirModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName);
}

ModulePtr Ps2RomdirModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<Ps2RomdirModule>();
    module->file_ = file;
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::PS2_ROMDIR;
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open PS2 ROMDIR image");
        return ModulePtr(std::move(module));
    }

    const qint64 base = findPs2Romdir(file);
    if (base < 0) {
        module->info_.error = QStringLiteral("PS2 ROMDIR directory not found");
        return ModulePtr(std::move(module));
    }

    Ps2RomdirImageInfo image;
    QString error;
    if (!parsePs2Romdir(file, quint64(base), &image, &error)) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }

    module->info_.headerOffset = image.baseOffset;
    module->info_.description = QStringLiteral("PlayStation 2 ROMDIR image — %1 files; ROMDIR at 0x%2")
        .arg(image.entries.size())
        .arg(image.baseOffset, 0, 16);

    for (int i = 0; i < image.entries.size(); ++i) {
        const Ps2RomdirEntryInfo& entry = image.entries.at(i);
        ResourceEntry resource;
        resource.type = QStringLiteral("PS2_ROM_FILE");
        resource.name = entry.name;
        resource.language = entry.extInfoSummary;
        resource.dataOffset = entry.dataOffset;
        resource.dataSize = entry.fileSize;
        resource.baseId = i;
        resource.format = ModuleFormat::PS2_ROMDIR;
        resource.isEmbeddedFile = entry.fileSize > 0;
        resource.content = std::make_shared<fs::SubStore>(file, qint64(entry.dataOffset),
                                                          qint64(entry.fileSize));
        module->resources_.push_back(std::move(resource));
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
