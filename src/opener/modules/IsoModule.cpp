#include "IsoModule.h"
#include "Compat.h"

#include "../fs/Iso9660Reader.h"

#include <QFile>

#include <string>
#include <utility>

namespace peare {
namespace {

// Walk the file-system tree, emitting one ResourceEntry per file. Only metadata
// is produced here (names, sizes, hierarchy) — file content stays a lazy fs
// window that is read on demand when the resource is opened.
void walk(const fs::IDiscFileSystem& fs, const std::string& dir,
          const QStringList& prefix, QVector<ResourceEntry>& out) {
    const std::vector<fs::DiscEntry> entries = fs.list(dir);
    for (const fs::DiscEntry& e : entries) {
        const std::string full = dir.empty() ? e.name : dir + "/" + e.name;
        QStringList path = prefix;
        path << QString::fromUtf8(e.name.c_str());
        if (e.isDirectory) {
            walk(fs, full, path, out);
        } else {
            ResourceEntry entry;
            entry.type = QStringLiteral("ISO_FILE");
            entry.name = QString::fromUtf8(e.name.c_str());
            entry.language = QStringLiteral("neutral");
            entry.dataSize = quint64(e.length);
            entry.format = ModuleFormat::ISO9660;
            entry.hierarchyPath = path;
            entry.content = fs.openFile(full);  // lazy window over the image
            out.push_back(std::move(entry));
        }
    }
}

}  // namespace

ModulePtr IsoModule::open(const QString& filePath) {
    auto module = peare::makeUnique<IsoModule>();
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::ISO9660;
    module->info_.description = QStringLiteral("ISO 9660 image");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        module->info_.error = file.errorString();
        return ModulePtr(std::move(module));
    }
    const QByteArray bytes = file.readAll();
    fs::ByteStorePtr disc = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
    auto reader = std::make_shared<fs::Iso9660Reader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName());
    walk(*reader, std::string(), QStringList(), module->resources_);
    return ModulePtr(std::move(module));
}

}  // namespace peare
