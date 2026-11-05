#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

struct AfmGlyph {
    int code = -1;
    double advance = 0;
    QString name;
};

struct AfmFont {
    QString fontName;
    QString familyName;
    QString fullName;
    QString weight;
    double italicAngle = 0;
    double ascender = 0;
    double descender = 0;
    double bboxWidth = 0;
    double bboxHeight = 0;
    int declaredGlyphs = -1;
    QVector<AfmGlyph> glyphs;
};

bool hasAfmHeader(const QByteArray& data);
bool parseAfm(const QByteArray& data, AfmFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
