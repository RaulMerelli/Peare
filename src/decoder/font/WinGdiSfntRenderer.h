#pragma once

#include "SfntFont.h"
#include <QImage>
#include <QVector>

namespace peare {
namespace font {

QImage renderSfntTextWithGdi(const SfntFont& font, const QString& text, int scale,
                             int padding, QRgb foreground, QRgb background,
                             QString* error);
QImage renderSfntGlyphWithGdi(const SfntFont& font, quint32 glyphIndex, int scale,
                              int padding, QRgb foreground, QRgb background,
                              QString* error);
QVector<QImage> renderSfntGlyphsWithGdi(const SfntFont& font,
                                        const QVector<quint32>& glyphIndexes,
                                        int scale, int padding, QRgb foreground,
                                        QRgb background, QString* error);

} // namespace font
} // namespace peare
