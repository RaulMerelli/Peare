#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/ModuleFormat.h"

#include <QHash>
#include <QString>

namespace peare {
namespace resources {

class RT_ACCELERATOR {
public:
    static QString Get(const QByteArray& data, ModuleFormat format);
    static ResourcePreview preview(const ResourceEntry& entry);
    static const QHash<quint16, QString>& virtualKeyCodeMap();
    static const QHash<quint16, QString>& controlCharMap();
};

} // namespace resources
} // namespace peare
