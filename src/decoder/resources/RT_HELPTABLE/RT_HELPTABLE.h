#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare { namespace resources {

class RT_HELPTABLE {
public:
    static QString Get(const QByteArray& data);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} } // namespace peare::resources
