#pragma once

// Shared helper for the DiscUtils-family filesystem opener modules (ISO, WIM,
// FAT, exFAT, ext, UDF). Instead of walking the whole tree at open time, a module
// lists ONE directory level: files become <PREFIX>_FILE leaves with lazy content,
// and subdirectories become <PREFIX>_DIR container entries that reopen the same
// image rooted at the child path. This is the lazy-enumeration seam — the entire
// tree is never materialised, so a full OS image opens instantly.

#include "Module.h"

#include "../fs/DiscFileSystem.h"

#include <QString>
#include <QVector>

#include <string>
#include <utility>

namespace peare {

// Lists the single directory `subPath` ("" == root) of `fsys` into `out`.
// `disc` is the whole-filesystem image store, carried by each directory entry so
// navigating it can reopen the image at the child path.
inline void buildFsLevel(const fs::IDiscFileSystem& fsys, const fs::ByteStorePtr& disc,
                         const std::string& subPath, ModuleFormat format,
                         const QString& fileType, const QString& dirType,
                         QVector<ResourceEntry>& out) {
    const std::vector<fs::DiscEntry> entries = fsys.list(subPath);
    for (const fs::DiscEntry& e : entries) {
        const std::string full = subPath.empty() ? e.name : subPath + "/" + e.name;
        const QString name = QString::fromUtf8(e.name.c_str());
        ResourceEntry entry;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.format = format;
        if (e.isDirectory) {
            entry.type = dirType;
            entry.isDirectory = true;
            entry.containerSubPath = QString::fromStdString(full);
            entry.content = disc;  // reopened at containerSubPath on navigation
        } else {
            entry.type = fileType;
            entry.isEmbeddedFile = true;  // a whole file: nested-open candidate
            entry.dataSize = quint64(e.length);
            entry.content = fsys.openFile(full);  // lazy window over the image
        }
        out.push_back(std::move(entry));
    }
}

}  // namespace peare
