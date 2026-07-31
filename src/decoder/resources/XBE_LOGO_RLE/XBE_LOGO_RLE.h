#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/Module.h"

namespace peare {
namespace resources {

class XBE_LOGO_RLE final {
public:
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
