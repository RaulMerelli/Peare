#include "ImzModule.h"
#include "Compat.h"
#include "FloppyImageModule.h"
#include "ZipModule.h"
#include "../fs/FloppyImage.h"

#include <QFileInfo>

namespace peare {
namespace {

QString resourcePath(const ResourceEntry& entry)
{
    QStringList parts = entry.hierarchyPath;
    parts.push_back(entry.name);
    return parts.join(QLatin1Char('/'));
}

} // namespace

ModulePtr ImzModule::open(const fs::ByteStorePtr& file, const QString& sourceName,
                          const QString& subPath)
{
    auto module = peare::makeUnique<ImzModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::FLOPPY_IMAGE;
    module->info_.description = QStringLiteral("WinImage compressed floppy image");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open IMZ image");
        return ModulePtr(std::move(module));
    }

    ModulePtr archive = ZipModule::open(file, sourceName);
    if (!archive->info().isValid()) {
        module->info_.error = archive->info().error;
        return ModulePtr(std::move(module));
    }
    auto* entries = dynamic_cast<IResourceContainer*>(archive.get());
    if (!entries) {
        module->info_.error = QStringLiteral("IMZ archive has no file entries");
        return ModulePtr(std::move(module));
    }

    const ResourceEntry* image = nullptr;
    for (const ResourceEntry& entry : entries->resources()) {
        if (entry.isDirectory) continue;
        const QString suffix = QFileInfo(resourcePath(entry)).suffix().toLower();
        if ((suffix != QStringLiteral("ima") && suffix != QStringLiteral("img")) ||
            !fs::floppyGeometryForCapacity(qint64(entry.dataSize)).valid())
            continue;
        if (image) {
            module->info_.error = QStringLiteral("IMZ archive contains multiple floppy images");
            return ModulePtr(std::move(module));
        }
        image = &entry;
    }
    if (!image || !image->content) {
        module->info_.error = QStringLiteral("IMZ archive has no supported raw floppy image");
        return ModulePtr(std::move(module));
    }

    module->floppy_ = FloppyImageModule::open(image->content, sourceName, subPath);
    if (!module->floppy_->info().isValid()) {
        module->info_.error = module->floppy_->info().error;
        return ModulePtr(std::move(module));
    }
    module->info_.description += QStringLiteral("; %1").arg(module->floppy_->info().description);
    if (auto* floppyEntries = dynamic_cast<IResourceContainer*>(module->floppy_.get()))
        module->resources_ = floppyEntries->resources();
    return ModulePtr(std::move(module));
}

} // namespace peare
