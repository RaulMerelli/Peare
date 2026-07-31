#pragma once

#include "ModuleFormat.h"

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
