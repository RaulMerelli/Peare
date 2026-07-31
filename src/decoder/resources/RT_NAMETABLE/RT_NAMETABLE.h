#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare {
namespace resources {

class RT_NAMETABLE {
public:
    static QString Get(const QByteArray& data);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
