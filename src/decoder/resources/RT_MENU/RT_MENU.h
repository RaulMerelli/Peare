#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare {
namespace resources {

class RT_MENU {
public:
    static QString Get(const QByteArray& data, ModuleFormat format, bool isOs2 = false, int baseId = 100);
    static ResourcePreview preview(const ResourceEntry& entry);

private:
    static QString GetMenuFlagsString(quint16 flags);
    static qsizetype FindNullTerminatedUnicodeStringEnd(const QByteArray& data, qsizetype startIndex);
    static qsizetype FindNullTerminatedAnsiStringEnd(const QByteArray& data, qsizetype startIndex);
    static bool IsRemainingDataNull(const QByteArray& data, qsizetype startIndex);
};

} // namespace resources
} // namespace peare
