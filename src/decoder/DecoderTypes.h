#pragma once

#include "../opener/modules/Module.h"

#include <QImage>
#include <QString>
#include <QVector>
#include <QByteArray>

namespace peare {

struct DecodedImageInfo {
    int width = 0;
    int height = 0;
    int bitsPerPixel = 0;
};

struct EmbeddedExport {
    QString fileName;
    QString extension;
    QByteArray bytes;
};

struct ResourcePreview {
    QVector<QImage> images;
    QVector<QString> imageLabels;
    // Source images used by converted export. For fonts these are the individual
    // glyph bitmaps, not the composed sample and glyph-map preview images.
    QVector<QImage> conversionImages;
    QVector<int> conversionImageCodes;
    QString text;
    QString error;
    bool rawDump = false;
    QVector<EmbeddedExport> embeddedExports;

public:
    QImage image;
};

} // namespace peare
