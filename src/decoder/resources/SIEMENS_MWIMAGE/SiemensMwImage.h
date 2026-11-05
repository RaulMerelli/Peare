#pragma once

#include "../../../opener/modules/Module.h"
#include "../../DecoderTypes.h"

namespace peare { namespace resources {

class SiemensMwImage final {
public:
    static ResourcePreview preview(const ResourceEntry& entry);
};

} } // namespace peare::resources
