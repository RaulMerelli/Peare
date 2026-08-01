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
        const QString name = QString::fromUtf8(e.name.c_str());
        if (e.isDirectory) {
            QStringList sub = prefix;
            sub << name;
            walk(fs, full, sub, out);
        } else {
            ResourceEntry entry;
            entry.type = QStringLiteral("ISO_FILE");
            entry.name = name;
            entry.language = QStringLiteral("neutral");
            entry.dataSize = quint64(e.length);
            entry.format = ModuleFormat::ISO9660;
            // hierarchyPath is the containing directory only; the file's own name
            // is the leaf. (Putting the name here too would create a fictitious
            // folder sharing the file's name.)
            entry.hierarchyPath = prefix;
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

    // Memory-map the image so opening reads only the directory metadata; file
    // content is paged in lazily by the OS on access, never loaded up front.
    // The QFile is kept alive by the disc store (and thus by every file window
    // over it), so the mapping outlives this module if a resource is still open.
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
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
        // Fallback for the rare case mapping is unavailable: read it in.
        const QByteArray bytes = holder->readAll();
        disc = std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
    }
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
