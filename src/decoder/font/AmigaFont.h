#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "BitmapFont.h"

namespace peare {
namespace font {

struct AmigaFontStrike {
    QString fileName;
    quint16 pixelHeight = 0;
    quint8 style = 0;
    quint8 flags = 0;
    quint16 tagCount = 0;
};

struct AmigaFontContents {
    quint16 fileId = 0;
    QVector<AmigaFontStrike> strikes;
};

bool parseAmigaFontContents(const QByteArray& data, AmigaFontContents* contents,
                            QString* error = nullptr);
bool parseAmigaFontStrike(const QByteArray& data, BitmapFont* font,
                          QString* error = nullptr);

} // namespace font
} // namespace peare
