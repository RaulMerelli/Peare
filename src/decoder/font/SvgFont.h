#pragma once

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

enum class SvgPathOp { MoveTo, LineTo, CubicTo, QuadTo, Close };

struct SvgPathCommand {
    SvgPathCommand() = default;
    SvgPathCommand(SvgPathOp operation, const QPointF& first, const QPointF& second,
                   const QPointF& endpoint)
        : op(operation), control1(first), control2(second), end(endpoint) {}

    SvgPathOp op = SvgPathOp::MoveTo;
    QPointF control1;
    QPointF control2;
    QPointF end;
};

struct SvgGlyph {
    QString unicode;
    QString name;
    double advance = 0;
    QVector<SvgPathCommand> path;
};

struct SvgFont {
    QString familyName;
    QString styleName;
    double unitsPerEm = 1000;
    double ascent = 800;
    double descent = -200;
    double advance = 1000;
    QVector<SvgGlyph> glyphs;
    QVector<SvgPathCommand> missingGlyph;
    double missingAdvance = 1000;
};

bool hasSvgFontHeader(const QByteArray& data);
bool parseSvgFont(const QByteArray& data, SvgFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
