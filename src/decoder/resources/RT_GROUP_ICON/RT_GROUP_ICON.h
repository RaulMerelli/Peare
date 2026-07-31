#pragma once
#include "../../../decoder/DecoderTypes.h"
#include "../ResourceResolver.h"
namespace peare { namespace resources {
class RT_GROUP_ICON {
public:
    static ResourcePreview preview(const ResourceEntry& entry,const IResourceResolver& resolver);
};
} }
