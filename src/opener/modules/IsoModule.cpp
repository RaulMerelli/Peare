#include "IsoModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/Iso9660Reader.h"

#include <QFile>

#include <utility>

namespace peare {

ModulePtr IsoModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath) {
    auto module = peare::makeUnique<IsoModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ISO9660;
    module->info_.description = QStringLiteral("ISO 9660 image");

    fs::ByteStorePtr content = disc;
    if (content && !fs::Iso9660Reader::detect(*content)) {
        if (fs::OpticalMode1Store::detectIso9660(*content))
            content = std::make_shared<fs::OpticalMode1Store>(content);
        else if (fs::OpticalMode2Store::detectIso9660(*content))
            content = std::make_shared<fs::OpticalMode2Store>(content);
    }

    auto reader = std::make_shared<fs::Iso9660Reader>(content);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName());
    buildFsLevel(*reader, content, subPath.toStdString(), ModuleFormat::ISO9660,
                 QStringLiteral("ISO_FILE"), QStringLiteral("ISO_DIR"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr IsoModule::open(const QString& filePath) {
    // Memory-map the image so opening reads only the directory metadata; file
    // content is paged in lazily by the OS on access, never loaded up front.
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<IsoModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::ISO9660;
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
