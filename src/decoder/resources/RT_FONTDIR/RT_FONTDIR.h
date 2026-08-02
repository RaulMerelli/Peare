#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/ModuleFormat.h"

namespace peare { namespace resources {

class RT_FONTDIR {
public:
    static QString Get(const QByteArray& data,
                       ModuleFormat format = ModuleFormat::PE,
                       bool isOs2 = false);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} } // namespace peare::resources
