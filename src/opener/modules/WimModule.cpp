#include "WimModule.h"
#include "Compat.h"

#include "../fs/WimReader.h"

#include <QFile>

#include <string>
#include <utility>

namespace peare {
namespace {

void walk(const fs::IDiscFileSystem& fs, const std::string& dir,
          const QStringList& prefix, QVector<ResourceEntry>& out) {
    const std::vector<fs::DiscEntry> entries = fs.list(dir);
    for (const fs::DiscEntry& e : entries) {
        const std::string full = dir.empty() ? e.name : dir + "/" + e.name;
        const QString name = QString::fromUtf8(e.name.c_str());
        if (e.isDirectory) {
            QStringList sub = prefix;
            sub << name;
            walk(fs, full, sub, out);
        } else {
            ResourceEntry entry;
            entry.type = QStringLiteral("WIM_FILE");
            entry.isEmbeddedFile = true;
            entry.name = name;
            entry.language = QStringLiteral("neutral");
            entry.dataSize = quint64(e.length);
            entry.format = ModuleFormat::WIM;
            entry.hierarchyPath = prefix;
            entry.content = fs.openFile(full);  // lazy chunked resource
            out.push_back(std::move(entry));
        }
    }
}

}  // namespace

ModulePtr WimModule::open(const fs::ByteStorePtr& disc, const QString& sourceName) {
    auto module = peare::makeUnique<WimModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::WIM;
    module->info_.description = QStringLiteral("Windows Imaging (WIM) image");

    auto reader = std::make_shared<fs::WimReader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName());
    walk(*reader, std::string(), QStringList(), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr WimModule::open(const QString& filePath) {
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<WimModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::WIM;
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
