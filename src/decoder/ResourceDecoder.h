#pragma once

#include "DecoderTypes.h"
#include "../opener/OpenerSession.h"
#include "resources/ResourceResolver.h"

namespace peare {

class ResourceDecoder final {
public:
    static ResourcePreview preview(const OpenedResource& resource) noexcept;
    static ResourcePreview preview(const OpenedResource& resource,
                                   const resources::IResourceResolver& resolver) noexcept;
    static QVector<DecodedImageInfo> imageInfo(const OpenedResource& resource) noexcept;
    static bool fontCharacterRange(const OpenedResource& resource, int* firstCharacter,
                                   int* lastCharacter, int* pixelHeight = nullptr) noexcept;
    static QImage renderFont(const OpenedResource& resource, const QString& text, int scale,
                             int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                             QString* error = nullptr) noexcept;
    static QImage renderFontGlyph(const OpenedResource& resource, quint32 glyphIndex, int scale,
                                  int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                                  QString* error = nullptr) noexcept;
    static QVector<QImage> renderFontGlyphs(const OpenedResource& resource,
                                            const QVector<quint32>& glyphIndexes, int scale,
                                            int padding, QRgb foregroundRgba,
                                            QRgb backgroundRgba,
                                            QString* error = nullptr) noexcept;
};

} // namespace peare
