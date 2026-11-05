#pragma once

#include "ModuleFormat.h"
#include "../fs/DiscStore.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace peare {

enum class ModuleOpenError {
    None,
    PasswordRequired,
    PasswordInvalidOrCorrupt
};

struct ModuleInfo {
    QString filePath;
    ModuleFormat format = ModuleFormat::Unknown;
    QString description;
    quint64 headerOffset = 0;
    QString error;
    ModuleOpenError openError = ModuleOpenError::None;

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
    // True when this resource is a complete embedded file eligible for nesting.
    bool isEmbeddedFile = false;
    // Computed by the session for isEmbeddedFile resources: a cheap header peek
    // recognised an openable format, so the consumer may offer to expand it.
    bool isContainer = false;
    // When set, this lazy store is authoritative; otherwise the flat `data` is used.
    peare::fs::ByteStorePtr content;
    // Directory containers retain the image store and the selected directory path.
    // Navigation reopens the same image at that path.
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
