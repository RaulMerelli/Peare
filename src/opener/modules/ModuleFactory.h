#pragma once

#include "Module.h"

#include <QByteArray>

namespace peare {

class ModuleFactory final {
public:
    static ModulePtr open(const QString& filePath);
    static ModulePtr open(const QString& physicalPath, const QString& logicalName);
    static ModulePtr open(const QByteArray& data, const QString& logicalName);
};

} // namespace peare
