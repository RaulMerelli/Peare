#pragma once

#include "../../decoder/DecoderTypes.h"
#include "ResourceResolver.h"

namespace peare {
namespace resources {

class ModuleResources {
public:
    static ResourcePreview preview(const ResourceEntry& entry,
                                   const IResourceResolver& resolver);
};

} // namespace resources
} // namespace peare
