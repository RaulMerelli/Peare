#pragma once

#include "../../DecoderTypes.h"

namespace peare { namespace resources {

class SiemensAdonis final {
public:
    static bool recognizes(const QByteArray& data) noexcept;
    static ResourcePreview preview(const ResourceEntry& entry);
};

} } // namespace peare::resources
