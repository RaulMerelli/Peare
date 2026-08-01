#pragma once

#include "Module.h"

#include <QByteArray>

namespace peare {

class ModuleFactory final {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QString& physicalPath, const QString& logicalName);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
    // Open over a positioned byte source. Filesystem formats (ISO, WIM) are read
    // straight from the source with no materialisation; other formats fall back
    // to reading it fully.
    static ModulePtr open(const fs::ByteStorePtr& disc, const QString& sourceName);
};

} // namespace peare
