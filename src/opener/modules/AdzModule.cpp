#include "AdzModule.h"

#include "Compat.h"
#include "FloppyImageModule.h"
#include "GzipModule.h"
#include "../fs/AmigaFsReader.h"
#include "../fs/FloppyImage.h"

namespace peare {

ModulePtr AdzModule::open(const fs::ByteStorePtr& source, const QString& sourceName,
                          const QString& subPath)
{
    auto module = peare::makeUnique<AdzModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::FLOPPY_IMAGE;
    module->info_.description = QStringLiteral("Amiga compressed floppy image (ADZ)");
    if (!source) {
        module->info_.error = QStringLiteral("Cannot open ADZ source");
        return ModulePtr(std::move(module));
    }
    if (source->capacity() < 18) {
        module->info_.error = QStringLiteral("ADZ GZIP stream is truncated");
        return ModulePtr(std::move(module));
    }
    std::uint8_t trailer[4];
    if (source->read(source->capacity() - 4, trailer, sizeof(trailer)) != sizeof(trailer)) {
        module->info_.error = QStringLiteral("Cannot read ADZ GZIP trailer");
        return ModulePtr(std::move(module));
    }
    const std::uint32_t declaredSize = std::uint32_t(trailer[0]) |
        (std::uint32_t(trailer[1]) << 8) | (std::uint32_t(trailer[2]) << 16) |
        (std::uint32_t(trailer[3]) << 24);
    if (declaredSize != 901120) {
        module->info_.error = QStringLiteral("ADZ is not a standard 880 KiB ADF image");
        return ModulePtr(std::move(module));
    }

    std::unique_ptr<GzipModule> gzip = GzipModule::open(source, sourceName);
    if (!gzip->info().isValid() || gzip->memberCount() != 1 || gzip->resources().size() != 1) {
        module->info_.error = gzip->info().error.isEmpty()
            ? QStringLiteral("ADZ must contain exactly one GZIP member")
            : gzip->info().error;
        return ModulePtr(std::move(module));
    }
    const ResourceEntry& member = gzip->resources().front();
    if (!member.content || member.dataSize != 901120) {
        module->info_.error = QStringLiteral("ADZ payload is not a standard 880 KiB ADF image");
        return ModulePtr(std::move(module));
    }

    auto amiga = std::make_shared<fs::AmigaFsReader>(member.content);
    if (!amiga->valid()) {
        module->info_.error = QStringLiteral("ADZ payload does not contain a valid Amiga DOS filesystem");
        return ModulePtr(std::move(module));
    }
    module->floppy_ = FloppyImageModule::open(member.content, sourceName, subPath);
    if (!module->floppy_->info().isValid()) {
        module->info_.error = module->floppy_->info().error;
        return ModulePtr(std::move(module));
    }
    module->info_.description += QStringLiteral(" — %1")
        .arg(module->floppy_->info().description);
    if (auto* entries = dynamic_cast<IResourceContainer*>(module->floppy_.get()))
        module->resources_ = entries->resources();
    return ModulePtr(std::move(module));
}

} // namespace peare
