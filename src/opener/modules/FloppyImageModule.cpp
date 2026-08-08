#include "FloppyImageModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/FatReader.h"
#include "../fs/FloppyImage.h"

#include <QFile>
#include <QFileInfo>

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

QString geometryDescription(const fs::FloppyGeometry& geometry)
{
    return QStringLiteral("Raw PC floppy disk image — %1; %2 cylinders × %3 × "
                          "%4 sectors/track × %5 bytes/sector")
        .arg(QString::fromStdString(geometry.nominalSize))
        .arg(geometry.cylinders)
        .arg(geometry.heads == 1 ? QStringLiteral("1 head")
                                 : QStringLiteral("%1 heads").arg(geometry.heads))
        .arg(geometry.sectorsPerTrack)
        .arg(geometry.bytesPerSector);
}

} // namespace

bool FloppyImageModule::hasSupportedExtension(const QString& sourceName)
{
    const QString suffix = QFileInfo(sourceName).suffix().toLower();
    return suffix == QStringLiteral("img") || suffix == QStringLiteral("ima") ||
           suffix == QStringLiteral("vfd");
}

ModulePtr FloppyImageModule::open(const fs::ByteStorePtr& image,
                                  const QString& sourceName,
                                  const QString& subPath)
{
    auto module = peare::makeUnique<FloppyImageModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::FLOPPY_IMAGE;
    if (!image) {
        module->info_.error = QStringLiteral("Cannot open floppy image");
        return ModulePtr(std::move(module));
    }

    const fs::FloppyGeometry geometry = fs::floppyGeometryForCapacity(image->capacity());
    if (!geometry.valid()) {
        module->info_.error = QStringLiteral("Unsupported raw floppy image capacity: %1 bytes")
                                  .arg(image->capacity());
        return ModulePtr(std::move(module));
    }

    module->image_ = image;
    module->info_.description = geometryDescription(geometry);

    auto fat = std::make_shared<fs::FatReader>(image);
    if (fat->valid()) {
        module->fs_ = fat;
        module->info_.description += QStringLiteral("; %1 filesystem")
                                         .arg(QString::fromStdString(fat->friendlyName()));
        buildFsLevel(*fat, image, subPath.toStdString(), ModuleFormat::FLOPPY_IMAGE,
                     QStringLiteral("FLOPPY_FILE"), QStringLiteral("FLOPPY_DIR"),
                     module->resources_);
        if (!module->resources_.isEmpty())
            return ModulePtr(std::move(module));
        // An empty formatted floppy still needs one navigable/exportable item.
    }

    // A floppy is itself the raw sector container. Do not invent a child file
    // for unformatted or empty media; the module-level raw view remains available.
    if (!fat->valid())
        module->info_.description += QStringLiteral("; no recognised FAT filesystem");
    else
        module->info_.description += QStringLiteral("; empty filesystem");
    return ModulePtr(std::move(module));
}

ModulePtr FloppyImageModule::open(const QString& filePath)
{
    return open(storeForFile(filePath), filePath);
}

ModulePtr FloppyImageModule::open(const QString& physicalPath, const QString& logicalName)
{
    return open(storeForFile(physicalPath), logicalName);
}

} // namespace peare
