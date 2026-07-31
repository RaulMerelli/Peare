#pragma once
#include "../../decoder/DecoderTypes.h"
namespace peare {
namespace resources {
class RawDetect final {
public:
    static ResourcePreview Get(const ResourceEntry& entry);
    static QString DumpRaw(const QByteArray& data, bool showAddressAndAscii = true);
};
} // namespace resources
} // namespace peare
