#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/ModuleFormat.h"

namespace peare {
namespace resources {

class RT_MESSAGE {
public:
    static QString Get(const QByteArray& data,
                       ModuleFormat headerType,
                       bool isOs2 = false);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
