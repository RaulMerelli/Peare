#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare {
namespace resources {

class RT_DIALOG {
public:
    static QString Get(const QByteArray& data, ModuleFormat format, bool isOs2 = false);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
