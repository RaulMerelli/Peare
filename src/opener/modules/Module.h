#pragma once

#include "ModuleFormat.h"
#include "../fs/DiscStore.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace peare {

struct ModuleInfo {
    QString filePath;
    ModuleFormat format = ModuleFormat::Unknown;
    QString description;
    quint64 headerOffset = 0;
    QString error;

    bool isValid() const noexcept { return error.isEmpty(); }
};

struct ResourceEntry {
    QString type;
    QString name;
    QString language;
    quint64 dataOffset = 0;
    quint64 dataSize = 0;
    quint32 codePage = 0;
    ModuleFormat format = ModuleFormat::Unknown;
    bool isOs2 = false;
    int baseId = 0;
    QStringList hierarchyPath;
    QByteArray data;
    // Module-declared: this resource is a whole embedded file (an OS2 pack
    // member, an ISO file, ...), i.e. a candidate to be opened as a nested
    // container. Sub-resources (RT_* icons, sections) leave it false so they are
    // never probed. Not a heuristic — the module states it.
    bool isEmbeddedFile = false;
    // Computed by the session for isEmbeddedFile resources: a cheap header peek
    // recognised an openable format, so the consumer may offer to expand it.
    bool isContainer = false;
    // Lazy, layer-backed content (DiscUtils-compatible fs stack). When set it is
    // the authoritative content, read on demand when the resource is opened;
    // otherwise `data` (the flat array) is used. This single seam is what lets
    // the common opener ABI hide whether a resource is array- or layer-backed.
    peare::fs::ByteStorePtr content;
    // Directory-level container: a filesystem opener lists one directory at a
    // time; a subdirectory is exposed as this kind of entry. `content` holds the
    // whole filesystem image (the volume/partition store) and `containerSubPath`
    // the directory path inside it. Navigating it reopens the same image rooted
    // at that path — the lazy-enumeration seam for the DiscUtils fs stack.
    bool isDirectory = false;
    QString containerSubPath;
};

class IModule {
public:
    virtual ~IModule() = default;
    virtual const ModuleInfo& info() const noexcept = 0;
};

class IResourceContainer {
public:
    virtual ~IResourceContainer() = default;
    virtual const QVector<ResourceEntry>& resources() const noexcept = 0;
};

class ISectionContainer {
public:
    virtual ~ISectionContainer() = default;
};

class ISymbolContainer {
public:
    virtual ~ISymbolContainer() = default;
};

using ModulePtr = std::unique_ptr<IModule>;

} // namespace peare
