#include "AndroidSuperModule.h"

#include "Compat.h"
#include "../fs/AndroidSuperStore.h"

namespace peare {

ModulePtr AndroidSuperModule::open(const fs::ByteStorePtr& source,
                                   const QString& sourceName) {
    auto module = peare::makeUnique<AndroidSuperModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ANDROID_SUPER;
    module->info_.description = QStringLiteral("Android dynamic-partition super image");
    std::string error;
    const auto image = fs::AndroidSuperStore::open(source, &error);
    if (!image) {
        module->info_.error = QString::fromStdString(error);
        return ModulePtr(std::move(module));
    }
    for (const fs::AndroidLogicalPartition& partition : image->partitions()) {
        ResourceEntry entry;
        entry.type = QStringLiteral("ANDROID_LOGICAL_PARTITION");
        entry.name = QString::fromLatin1(partition.name.data(),
                                         static_cast<int>(partition.name.size()));
        entry.language = QStringLiteral("neutral");
        entry.dataSize = static_cast<quint64>(partition.content->capacity());
        entry.format = ModuleFormat::ANDROID_SUPER;
        entry.isEmbeddedFile = true;
        entry.content = partition.content;
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
