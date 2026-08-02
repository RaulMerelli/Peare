#include "PeImageModule.h"

#include "PeModuleCommon.h"
#include "PeResources.h"

#include <QFile>

namespace peare {

std::unique_ptr<PeImageModule> PeImageModule::open(const QByteArray& image,
                                                   const QString& logicalName)
{
    auto module = std::unique_ptr<PeImageModule>(new PeImageModule);
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::PE;
    module->info_.description = QStringLiteral("PE loaded image");

    const PeResourceResult parsed = PeResources::listImage(image);
    if (!parsed.isValid()) {
        module->info_.error = parsed.error;
        return module;
    }
    detail::appendPeStructure(module->resources_, image, PeStorageLayout::LoadedImage);
    detail::appendPeResources(module->resources_, parsed);
    return module;
}

std::unique_ptr<PeImageModule> PeImageModule::openFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = std::unique_ptr<PeImageModule>(new PeImageModule);
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::PE;
        module->info_.description = QStringLiteral("PE loaded image");
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

} // namespace peare
