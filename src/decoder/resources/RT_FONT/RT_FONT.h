#pragma once

#include "../../../decoder/DecoderTypes.h"

namespace peare {
namespace resources {

class RT_FONT final
{
public:
    static ResourcePreview preview(const ResourceEntry& entry);
    static QImage renderText(const ResourceEntry& entry, const QString& text, int scale,
                             int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                             QString* error = nullptr);
    static bool LooksLikeWindowsFnt(const QByteArray& resData);
    static bool LooksLikeOs2Fnt(const QByteArray& resData);
};

} // namespace resources
} // namespace peare
