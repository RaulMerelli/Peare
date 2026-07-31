#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare {
namespace resources {

class OS2_RT_FONT final
{
public:
    static bool LooksLike(const QByteArray& data);
    static ResourcePreview preview(const ResourceEntry& entry);
    static QImage renderText(const ResourceEntry& entry, const QString& text, int scale,
                             int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                             QString* error = nullptr);
};

} // namespace resources
} // namespace peare
