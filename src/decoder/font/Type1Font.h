#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

struct Type1PathCommand {
    enum Kind { Move, Line, Curve, Close, Fill, Stroke } kind = Move;
    double x1 = 0;
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
    double x3 = 0;
    double y3 = 0;
};

struct Type1TextRun {
    QString family;
    QString text;
    double fontSize = 1000;
    double scaleX = 1;
    double scaleY = 1;
    double offsetX = 0;
    double offsetY = 0;
};

struct Type1Glyph {
    QString name;
    quint32 codepoint = 0;
    double advance = 0;
    QVector<Type1PathCommand> path;
    QVector<Type1TextRun> textRuns;
};

struct Type1Font {
    QString name;
    double unitsPerEm = 1000;
    double ascent = 800;
    double descent = -200;
    int paintType = 0;
    double strokeWidth = 0;
    QVector<Type1Glyph> glyphs;
};

bool hasPfbHeader(const QByteArray& data);
bool hasType1Header(const QByteArray& data);
bool parseType1Font(const QByteArray& data, Type1Font* font, QString* error = nullptr);

} // namespace font
} // namespace peare
