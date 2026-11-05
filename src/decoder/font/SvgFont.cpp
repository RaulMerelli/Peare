#include "SvgFont.h"

#include <QXmlStreamReader>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace peare {
namespace font {
namespace {

class PathParser {
public:
    explicit PathParser(const QString& data) : data_(data), position_(0) {}

    bool parse(QVector<SvgPathCommand>* path)
    {
        QVector<SvgPathCommand> result;
        QChar command;
        QChar previous;
        QPointF current;
        QPointF subpath;
        QPointF control;
        bool haveCurrent = false;

        while (true) {
            skipSeparators();
            if (position_ >= data_.size())
                break;
            if (data_.at(position_).isLetter())
                command = data_.at(position_++);
            else if (command.isNull())
                return false;

            const bool relative = command.isLower();
            const QChar op = command.toUpper();
            if (op == QLatin1Char('Z')) {
                if (!haveCurrent)
                    return false;
                result.append({SvgPathOp::Close, QPointF(), QPointF(), QPointF()});
                current = subpath;
                previous = command;
                command = QChar();
                continue;
            }

            bool consumed = false;
            do {
                QPointF p;
                if (op == QLatin1Char('M') || op == QLatin1Char('L')) {
                    if (!readPoint(&p))
                        break;
                    if (relative)
                        p += current;
                    if (op == QLatin1Char('M') && !consumed) {
                        result.append({SvgPathOp::MoveTo, QPointF(), QPointF(), p});
                        subpath = p;
                    } else {
                        result.append({SvgPathOp::LineTo, QPointF(), QPointF(), p});
                    }
                    current = p;
                    haveCurrent = true;
                } else if (op == QLatin1Char('H') || op == QLatin1Char('V')) {
                    double value;
                    if (!readNumber(&value))
                        break;
                    if (!haveCurrent)
                        return false;
                    p = current;
                    if (op == QLatin1Char('H'))
                        p.setX(relative ? current.x() + value : value);
                    else
                        p.setY(relative ? current.y() + value : value);
                    result.append({SvgPathOp::LineTo, QPointF(), QPointF(), p});
                    current = p;
                } else if (op == QLatin1Char('C')) {
                    QPointF c1, c2, end;
                    if (!readPoint(&c1) || !readPoint(&c2) || !readPoint(&end))
                        break;
                    if (relative) {
                        c1 += current;
                        c2 += current;
                        end += current;
                    }
                    result.append({SvgPathOp::CubicTo, c1, c2, end});
                    current = end;
                    control = c2;
                } else if (op == QLatin1Char('S')) {
                    QPointF c2, end;
                    if (!readPoint(&c2) || !readPoint(&end))
                        break;
                    const QPointF c1 = (previous.toUpper() == QLatin1Char('C') || previous.toUpper() == QLatin1Char('S'))
                        ? current * 2 - control : current;
                    if (relative) {
                        c2 += current;
                        end += current;
                    }
                    result.append({SvgPathOp::CubicTo, c1, c2, end});
                    current = end;
                    control = c2;
                } else if (op == QLatin1Char('Q')) {
                    QPointF c, end;
                    if (!readPoint(&c) || !readPoint(&end))
                        break;
                    if (relative) {
                        c += current;
                        end += current;
                    }
                    result.append({SvgPathOp::QuadTo, c, QPointF(), end});
                    current = end;
                    control = c;
                } else if (op == QLatin1Char('T')) {
                    QPointF end;
                    if (!readPoint(&end))
                        break;
                    const QPointF c = (previous.toUpper() == QLatin1Char('Q') || previous.toUpper() == QLatin1Char('T'))
                        ? current * 2 - control : current;
                    if (relative)
                        end += current;
                    result.append({SvgPathOp::QuadTo, c, QPointF(), end});
                    current = end;
                    control = c;
                } else if (op == QLatin1Char('A')) {
                    double rx, ry, rotation, largeArc, sweep;
                    QPointF end;
                    if (!readNumber(&rx) || !readNumber(&ry) || !readNumber(&rotation)
                        || !readNumber(&largeArc) || !readNumber(&sweep) || !readPoint(&end))
                        break;
                    if (!haveCurrent || (largeArc != 0 && largeArc != 1) || (sweep != 0 && sweep != 1))
                        return false;
                    if (relative)
                        end += current;
                    if (!arcTo(result, current, end, rx, ry, rotation, largeArc != 0, sweep != 0))
                        return false;
                    current = end;
                } else {
                    return false;
                }
                consumed = true;
                previous = command;
                skipSeparators();
            } while (position_ < data_.size() && !data_.at(position_).isLetter());

            if (!consumed)
                return false;
            if (op == QLatin1Char('M'))
                command = relative ? QLatin1Char('l') : QLatin1Char('L');
        }

        *path = result;
        return true;
    }

private:
    void skipSeparators()
    {
        while (position_ < data_.size()
            && (data_.at(position_).isSpace() || data_.at(position_) == QLatin1Char(',')))
            ++position_;
    }

    bool readNumber(double* value)
    {
        skipSeparators();
        if (position_ >= data_.size())
            return false;
        const int start = position_;
        if (data_.at(position_) == QLatin1Char('+') || data_.at(position_) == QLatin1Char('-'))
            ++position_;
        bool digits = false;
        while (position_ < data_.size() && data_.at(position_).isDigit()) {
            ++position_;
            digits = true;
        }
        if (position_ < data_.size() && data_.at(position_) == QLatin1Char('.')) {
            ++position_;
            while (position_ < data_.size() && data_.at(position_).isDigit()) {
                ++position_;
                digits = true;
            }
        }
        if (!digits)
            return false;
        if (position_ < data_.size() && (data_.at(position_) == QLatin1Char('e') || data_.at(position_) == QLatin1Char('E'))) {
            ++position_;
            if (position_ < data_.size() && (data_.at(position_) == QLatin1Char('+') || data_.at(position_) == QLatin1Char('-')))
                ++position_;
            const int exponent = position_;
            while (position_ < data_.size() && data_.at(position_).isDigit())
                ++position_;
            if (position_ == exponent)
                return false;
        }
        bool ok = false;
        *value = data_.mid(start, position_ - start).toDouble(&ok);
        return ok && qIsFinite(*value) && qAbs(*value) <= 1.0e9;
    }

    bool readPoint(QPointF* point)
    {
        double x, y;
        if (!readNumber(&x) || !readNumber(&y))
            return false;
        *point = QPointF(x, y);
        return true;
    }

    static double vectorAngle(double ux, double uy, double vx, double vy)
    {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    }

    static bool arcTo(QVector<SvgPathCommand>& path, const QPointF& from, const QPointF& to,
        double rx, double ry, double degrees, bool largeArc, bool sweep)
    {
        rx = qAbs(rx);
        ry = qAbs(ry);
        if (from == to)
            return true;
        if (rx == 0 || ry == 0) {
            path.append({SvgPathOp::LineTo, QPointF(), QPointF(), to});
            return true;
        }
        const double phi = qDegreesToRadians(degrees);
        const double cs = std::cos(phi);
        const double sn = std::sin(phi);
        const double dx = (from.x() - to.x()) / 2;
        const double dy = (from.y() - to.y()) / 2;
        const double xp = cs * dx + sn * dy;
        const double yp = -sn * dx + cs * dy;
        const double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
        if (lambda > 1) {
            const double scale = std::sqrt(lambda);
            rx *= scale;
            ry *= scale;
        }
        const double numerator = qMax(0.0, rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp);
        const double denominator = rx * rx * yp * yp + ry * ry * xp * xp;
        if (denominator == 0)
            return false;
        const double sign = (largeArc == sweep) ? -1 : 1;
        const double factor = sign * std::sqrt(numerator / denominator);
        const double cxp = factor * rx * yp / ry;
        const double cyp = factor * -ry * xp / rx;
        const double cx = cs * cxp - sn * cyp + (from.x() + to.x()) / 2;
        const double cy = sn * cxp + cs * cyp + (from.y() + to.y()) / 2;
        const double ux = (xp - cxp) / rx;
        const double uy = (yp - cyp) / ry;
        const double vx = (-xp - cxp) / rx;
        const double vy = (-yp - cyp) / ry;
        const double start = std::atan2(uy, ux);
        const double pi = qAcos(-1.0);
        double delta = vectorAngle(ux, uy, vx, vy);
        if (!sweep && delta > 0)
            delta -= 2 * M_PI;
        else if (sweep && delta < 0)
            delta += 2 * M_PI;
        if (largeArc && qAbs(delta) < pi)
            delta += sweep ? 2 * pi : -2 * pi;
        if (!largeArc && qAbs(delta) > pi)
            delta += sweep ? -2 * pi : 2 * pi;
        const int count = qMax(1, static_cast<int>(std::ceil(qAbs(delta) / (pi / 2))));
        const double step = delta / count;
        const auto map = [=](double angle) {
            return QPointF(cx + cs * rx * std::cos(angle) - sn * ry * std::sin(angle),
                cy + sn * rx * std::cos(angle) + cs * ry * std::sin(angle));
        };
        const auto derivative = [=](double angle) {
            return QPointF(-cs * rx * std::sin(angle) - sn * ry * std::cos(angle),
                -sn * rx * std::sin(angle) + cs * ry * std::cos(angle));
        };
        for (int i = 0; i < count; ++i) {
            const double a0 = start + i * step;
            const double a1 = a0 + step;
            const double alpha = 4.0 / 3.0 * std::tan((a1 - a0) / 4.0);
            const QPointF p0 = map(a0);
            const QPointF p1 = p0 + derivative(a0) * alpha;
            const QPointF p3 = map(a1);
            const QPointF p2 = p3 - derivative(a1) * alpha;
            path.append({SvgPathOp::CubicTo, p1, p2, p3});
        }
        return true;
    }

    QString data_;
    int position_;
};

double attributeNumber(const QXmlStreamAttributes& attributes, const QString& name, double fallback, bool* ok = nullptr)
{
    const QString value = attributes.value(name).toString();
    if (value.isEmpty()) {
        if (ok)
            *ok = true;
        return fallback;
    }
    bool valid = false;
    const double number = value.toDouble(&valid);
    if (ok)
        *ok = valid && qIsFinite(number);
    return number;
}

bool parsePathAttribute(const QXmlStreamAttributes& attributes, QVector<SvgPathCommand>* path)
{
    const QString data = attributes.value(QLatin1String("d")).toString();
    if (data.isEmpty()) {
        path->clear();
        return true;
    }
    return PathParser(data).parse(path);
}

} // namespace

bool hasSvgFontHeader(const QByteArray& data)
{
    QXmlStreamReader reader(data.left(1024 * 1024));
    bool svgRoot = false;
    int depth = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (depth == 0) {
                svgRoot = reader.name() == QLatin1String("svg");
                if (!svgRoot)
                    return false;
            }
            ++depth;
            if (svgRoot && reader.name() == QLatin1String("font") && depth > 1)
                return true;
        } else if (reader.isEndElement()) {
            --depth;
        }
    }
    return false;
}

bool parseSvgFont(const QByteArray& data, SvgFont* font, QString* error)
{
    if (!font || data.isEmpty() || data.size() > 64 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("Invalid SVG font input size");
        return false;
    }
    SvgFont parsed;
    QXmlStreamReader reader(data);
    reader.setEntityExpansionLimit(1024);
    bool svgRoot = false;
    bool inFont = false;
    bool inGlyph = false;
    bool inMissingGlyph = false;
    bool sawFont = false;
    bool sawFace = false;
    bool valid = true;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            const QXmlStreamAttributes attrs = reader.attributes();
            if (!svgRoot) {
                if (name != QLatin1String("svg")) {
                    valid = false;
                    break;
                }
                svgRoot = true;
            } else if (!sawFont && name == QLatin1String("font")) {
                sawFont = true;
                inFont = true;
                bool ok = false;
                parsed.advance = attributeNumber(attrs, QStringLiteral("horiz-adv-x"), 1000, &ok);
                if (!ok || parsed.advance <= 0) {
                    valid = false;
                    break;
                }
            } else if (inFont && name == QLatin1String("font-face")) {
                bool emOk = false, ascentOk = false, descentOk = false;
                parsed.familyName = attrs.value(QLatin1String("font-family")).toString();
                parsed.styleName = attrs.value(QLatin1String("font-style")).toString();
                parsed.unitsPerEm = attributeNumber(attrs, QStringLiteral("units-per-em"), 1000, &emOk);
                parsed.ascent = attributeNumber(attrs, QStringLiteral("ascent"), 800, &ascentOk);
                const double descent = attributeNumber(attrs, QStringLiteral("descent"), 200, &descentOk);
                parsed.descent = -qAbs(descent);
                if (!emOk || !ascentOk || !descentOk || parsed.unitsPerEm <= 0
                    || parsed.unitsPerEm > 1.0e7 || qAbs(parsed.ascent) > 1.0e7
                    || qAbs(parsed.descent) > 1.0e7 || parsed.ascent <= 0
                    || parsed.ascent <= parsed.descent) {
                    valid = false;
                    break;
                }
                sawFace = true;
            } else if (inFont && (name == QLatin1String("glyph") || name == QLatin1String("missing-glyph"))) {
                SvgGlyph glyph;
                glyph.unicode = attrs.value(QLatin1String("unicode")).toString();
                glyph.name = attrs.value(QLatin1String("glyph-name")).toString();
                bool advanceOk = false;
                glyph.advance = attributeNumber(attrs, QStringLiteral("horiz-adv-x"), parsed.advance, &advanceOk);
                if (!advanceOk || glyph.advance < 0 || glyph.advance > 1.0e7
                    || !parsePathAttribute(attrs, &glyph.path)) {
                    valid = false;
                    break;
                }
                if (name == QLatin1String("missing-glyph")) {
                    parsed.missingGlyph = glyph.path;
                    parsed.missingAdvance = glyph.advance;
                    inMissingGlyph = true;
                } else {
                    parsed.glyphs.append(glyph);
                    inGlyph = true;
                }
            } else if (inFont && (inGlyph || inMissingGlyph) && name == QLatin1String("path")) {
                QVector<SvgPathCommand> path;
                if (!parsePathAttribute(attrs, &path)) {
                    valid = false;
                    break;
                }
                if (inMissingGlyph)
                    parsed.missingGlyph += path;
                else
                    parsed.glyphs.last().path += path;
            }
        } else if (reader.isEndElement()) {
            if (reader.name() == QLatin1String("glyph"))
                inGlyph = false;
            else if (reader.name() == QLatin1String("missing-glyph"))
                inMissingGlyph = false;
            else if (reader.name() == QLatin1String("font") && inFont)
                inFont = false;
        }
        if (!valid)
            break;
    }
    if (!valid || reader.hasError() || !svgRoot || !sawFont || !sawFace || parsed.glyphs.isEmpty()) {
        if (error)
            *error = reader.hasError() ? reader.errorString() : QStringLiteral("Unsupported or malformed SVG font");
        return false;
    }
    *font = parsed;
    return true;
}

} // namespace font
} // namespace peare
