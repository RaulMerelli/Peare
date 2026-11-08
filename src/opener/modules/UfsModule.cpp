#include "UfsModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/QFileByteStore.h"
#include "../fs/UfsReader.h"

#include <utility>

namespace peare {

ModulePtr UfsModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath) {
    auto module = peare::makeUnique<UfsModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::UFS;
    module->info_.description = QStringLiteral("Unix File System (UFS)");
    auto reader = std::make_shared<fs::UfsReader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QStringLiteral("Unix File System (%1)")
        .arg(QString::fromStdString(reader->friendlyName()));
    buildFsLevel(*reader, disc, subPath.toStdString(), ModuleFormat::UFS,
                 QStringLiteral("UFS_FILE"), QStringLiteral("UFS_DIR"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr UfsModule::open(const QString& filePath) {
    fs::ByteStorePtr disc = std::make_shared<fs::QFileByteStore>(filePath);
    if (!disc || disc->capacity() < 0) {
        auto module = peare::makeUnique<UfsModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::UFS;
        module->info_.error = QStringLiteral("Cannot open UFS image");
        return ModulePtr(std::move(module));
    }
    return open(disc, filePath);
}

}  // namespace peare
