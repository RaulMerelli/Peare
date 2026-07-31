#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/ModuleFormat.h"

namespace peare {
namespace resources {

class RT_ACCELTABLE {
public:
    static QString Get(const QByteArray& data, ModuleFormat format);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
