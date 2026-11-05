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
    static QImage renderGlyphIndex(const ResourceEntry& entry, quint32 glyphIndex, int scale,
                                   int padding, QRgb foregroundRgba, QRgb backgroundRgba,
                                   QString* error = nullptr);
    static QVector<QImage> renderGlyphIndexes(const ResourceEntry& entry,
                                              const QVector<quint32>& glyphIndexes, int scale,
                                              int padding, QRgb foregroundRgba,
                                              QRgb backgroundRgba, QString* error = nullptr);
    static bool LooksLikeWindowsFnt(const QByteArray& resData);
    static bool LooksLikeOs2Fnt(const QByteArray& resData);
    static bool LooksLikeBitmapFont(const QByteArray& resData);
    static bool LooksLikeOutlineFont(const QByteArray& resData);
    static bool LooksLikeMetricsFont(const QByteArray& resData);
    static bool LooksLikeSvgFont(const QByteArray& resData);
    static bool LooksLikeSfnFont(const QByteArray& resData);
    static bool LooksLikeShpFont(const QByteArray& resData);
    static bool LooksLikeFont(const QByteArray& resData);
};

} // namespace resources
} // namespace peare
