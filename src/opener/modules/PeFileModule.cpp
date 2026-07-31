#include "PeFileModule.h"

#include "PeModuleCommon.h"
#include "PeResources.h"

#include <QFile>

namespace peare {

std::unique_ptr<PeFileModule> PeFileModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<PeFileModule>(new PeFileModule);
    const ModuleFormatInfo detected = ModuleFormatDetector::detectFile(filePath);
    module->info_.filePath = filePath;
    module->info_.format = detected.format;
    module->info_.description = detected.description;
    module->info_.headerOffset = detected.headerOffset;
    module->info_.error = detected.error;
    if (!module->info_.isValid()) return module;

    const PeResourceResult parsed = PeResources::listFile(filePath);
    if (!parsed.isValid()) {
        module->info_.error = parsed.error;
        return module;
    }
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly))
        detail::appendPeStructure(module->resources_, file.readAll(), PeStorageLayout::File);
    detail::appendPeResources(module->resources_, parsed);
    return module;
}

std::unique_ptr<PeFileModule> PeFileModule::open(const QByteArray& fileData,
                                                 const QString& logicalName)
{
    auto module = std::unique_ptr<PeFileModule>(new PeFileModule);
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::PE;
    module->info_.description = QStringLiteral("PE executable");

    const PeResourceResult parsed = PeResources::listFile(fileData);
    if (!parsed.isValid()) {
        module->info_.error = parsed.error;
        return module;
    }
    detail::appendPeStructure(module->resources_, fileData, PeStorageLayout::File);
    detail::appendPeResources(module->resources_, parsed);
    return module;
}

} // namespace peare
