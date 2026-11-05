#include "Type3Font.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QtMath>

#include <cmath>

namespace peare {
namespace font {
namespace {

const int kMaximumBytes = 64 * 1024 * 1024;
const int kMaximumGlyphs = 65536;
const int kMaximumCommands = 100000;
const int kMaximumOperations = 2000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

void skipSpaceAndComments(const QByteArray& data, int* position)
{
    while (*position < data.size()) {
        const char value = data.at(*position);
        if (value == '%') {
            while (*position < data.size() && data.at(*position) != '\n' &&
                   data.at(*position) != '\r') ++*position;
        } else if (value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
                   value == '\f' || value == '\0') {
            ++*position;
        } else {
            break;
        }
    }
}

QByteArray readToken(const QByteArray& data, int* position)
{
    skipSpaceAndComments(data, position);
    const int start = *position;
    while (*position < data.size()) {
        const char value = data.at(*position);
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
            value == '\f' || value == '\0' || value == '%' || value == '{' ||
            value == '}' || value == '[' || value == ']' || value == '(' || value == ')')
            break;
        ++*position;
    }
    if (start == *position && *position < data.size()) ++*position;
    return data.mid(start, *position - start);
}

bool readProcedure(const QByteArray& data, int* position, QByteArray* procedure)
{
    if (*position >= data.size() || data.at(*position) != '{') return false;
    const int start = ++*position;
    int depth = 1;
    int stringDepth = 0;
    bool escaped = false;
    while (*position < data.size()) {
        const char value = data.at(*position);
        if (stringDepth) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '(') ++stringDepth;
            else if (value == ')') --stringDepth;
        } else if (value == '%') {
            while (*position < data.size() && data.at(*position) != '\n' &&
                   data.at(*position) != '\r') ++*position;
            continue;
        } else if (value == '(') {
            stringDepth = 1;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}' && --depth == 0) {
            *procedure = data.mid(start, *position - start);
            ++*position;
            return true;
        }
        ++*position;
    }
    return false;
}

struct PsValue {
    enum Kind { Number, Name, String, Procedure, Dictionary } kind = Number;
    double number = 0;
    QByteArray bytes;
    bool executable = false;
};

struct PsGraphicsState {
    double scaleX = 1;
    double scaleY = 1;
    double offsetX = 0;
    double offsetY = 0;
    double lineWidth = 1;
    int lineCap = 0;
    double fontSize = 1000;
    QByteArray fontName;
};

struct PsExecution {
    const QHash<QByteArray, QByteArray>* procedures = nullptr;
    QHash<QByteArray, PsValue> variables;
    QVector<PsValue> stack;
    QVector<PsGraphicsState> graphics;
    QVector<Type1PathCommand> currentPath;
    Type1Glyph* glyph = nullptr;
    PsGraphicsState state;
    double currentX = 0;
    double currentY = 0;
    double advance = 0;
    bool widthSet = false;
    int operations = 0;
    QString failure;
};

bool readPsValue(const QByteArray& data, int* position, PsValue* value)
{
    skipSpaceAndComments(data, position);
    if (*position >= data.size()) return false;
    const char first = data.at(*position);
    value->bytes.clear();
    value->executable = false;
    if (first == '{') {
        value->kind = PsValue::Procedure;
        return readProcedure(data, position, &value->bytes);
    }
    if (first == '(') {
        value->kind = PsValue::String;
        ++*position;
        int depth = 1;
        while (*position < data.size() && depth) {
            const uchar ch = uchar(data.at((*position)++));
            if (ch == '\\' && *position < data.size()) {
                const uchar escaped = uchar(data.at((*position)++));
                if (escaped >= '0' && escaped <= '7') {
                    int octal = escaped - '0';
                    for (int i = 0; i < 2 && *position < data.size() &&
                         data.at(*position) >= '0' && data.at(*position) <= '7'; ++i)
                        octal = octal * 8 + data.at((*position)++) - '0';
                    value->bytes.append(char(octal & 0xff));
                } else if (escaped == 'n') value->bytes.append('\n');
                else if (escaped == 'r') value->bytes.append('\r');
                else if (escaped == 't') value->bytes.append('\t');
                else if (escaped == '\n') { }
                else value->bytes.append(char(escaped));
            } else if (ch == '(') {
                ++depth; value->bytes.append('(');
            } else if (ch == ')') {
                if (--depth) value->bytes.append(')');
            } else value->bytes.append(char(ch));
        }
        return depth == 0;
    }
    const QByteArray token = readToken(data, position);
    if (token.isEmpty()) return false;
    bool ok = false;
    const double number = token.toDouble(&ok);
    if (ok) {
        value->kind = PsValue::Number;
        value->number = number;
        return qIsFinite(number) && qAbs(number) <= 10000000.0;
    }
    value->kind = PsValue::Name;
    value->executable = !token.startsWith('/');
    value->bytes = token.startsWith('/') ? token.mid(1) : token;
    return true;
}

bool popValues(PsExecution* state, int count, QVector<PsValue>* values)
{
    if (state->stack.size() < count) return false;
    values->clear();
    for (int i = state->stack.size() - count; i < state->stack.size(); ++i)
        values->push_back(state->stack.at(i));
    state->stack.resize(state->stack.size() - count);
    return true;
}

bool popNumbers(PsExecution* state, int count, QVector<double>* values)
{
    QVector<PsValue> objects;
    if (!popValues(state, count, &objects)) return false;
    values->clear();
    for (const PsValue& object : objects) {
        if (object.kind != PsValue::Number) return false;
        values->push_back(object.number);
    }
    return true;
}

void appendMapped(PsExecution* state, Type1PathCommand command)
{
    command.x1 = command.x1 * state->state.scaleX + state->state.offsetX;
    command.y1 = command.y1 * state->state.scaleY + state->state.offsetY;
    if (command.kind == Type1PathCommand::Curve) {
        command.x2 = command.x2 * state->state.scaleX + state->state.offsetX;
        command.y2 = command.y2 * state->state.scaleY + state->state.offsetY;
        command.x3 = command.x3 * state->state.scaleX + state->state.offsetX;
        command.y3 = command.y3 * state->state.scaleY + state->state.offsetY;
    }
    state->currentPath.push_back(command);
}

bool executeProcedure(const QByteArray& data, PsExecution* state, int depth);

bool executeOperator(const QByteArray& op, PsExecution* state, int depth)
{
    QVector<PsValue> values;
    QVector<double> numbers;
    if (op == "exch") {
        if (!popValues(state, 2, &values)) return false;
        state->stack.push_back(values.at(1)); state->stack.push_back(values.at(0));
    } else if (op == "dup") {
        if (state->stack.isEmpty()) return false;
        state->stack.push_back(state->stack.last());
    } else if (op == "pop") {
        if (state->stack.isEmpty()) return false;
        state->stack.removeLast();
    } else if (op == "dict") {
        if (!popNumbers(state, 1, &numbers)) return false;
        PsValue value; value.kind = PsValue::Dictionary; state->stack.push_back(value);
    } else if (op == "begin") {
        if (state->stack.isEmpty() || state->stack.last().kind != PsValue::Dictionary) return false;
        state->stack.removeLast();
    } else if (op == "end" || op == "bind" || op == "definefont" ||
               op == "true" || op == "false") {
    } else if (op == "def") {
        if (!popValues(state, 2, &values) || values.at(0).kind != PsValue::Name ||
            values.at(0).executable) return false;
        state->variables.insert(values.at(0).bytes, values.at(1));
    } else if (op == "exec") {
        if (state->stack.isEmpty() || state->stack.last().kind != PsValue::Procedure) return false;
        const QByteArray procedure = state->stack.takeLast().bytes;
        if (!executeProcedure(procedure, state, depth + 1)) return false;
    } else if (op == "save" || op == "gsave") {
        PsValue value; value.kind = PsValue::Dictionary; state->stack.push_back(value);
        state->graphics.push_back(state->state);
    } else if (op == "restore" || op == "grestore") {
        if (state->graphics.isEmpty() || state->stack.isEmpty()) return false;
        state->state = state->graphics.takeLast(); state->stack.removeLast();
    } else if (op == "scale" || op == "translate") {
        if (!popNumbers(state, 2, &numbers)) return false;
        if (op == "scale") {
            state->state.scaleX *= numbers.at(0); state->state.scaleY *= numbers.at(1);
        } else {
            state->state.offsetX += numbers.at(0) * state->state.scaleX;
            state->state.offsetY += numbers.at(1) * state->state.scaleY;
        }
    } else if (op == "moveto" || op == "lineto" || op == "mt" || op == "lt") {
        if (!popNumbers(state, 2, &numbers)) return false;
        state->currentX = numbers.at(0); state->currentY = numbers.at(1);
        Type1PathCommand command;
        command.kind = (op == "moveto" || op == "mt") ? Type1PathCommand::Move
                                                        : Type1PathCommand::Line;
        command.x1 = state->currentX; command.y1 = state->currentY;
        appendMapped(state, command);
    } else if (op == "curveto") {
        if (!popNumbers(state, 6, &numbers)) return false;
        Type1PathCommand command; command.kind = Type1PathCommand::Curve;
        command.x1 = numbers.at(0); command.y1 = numbers.at(1);
        command.x2 = numbers.at(2); command.y2 = numbers.at(3);
        command.x3 = numbers.at(4); command.y3 = numbers.at(5);
        state->currentX = command.x3; state->currentY = command.y3;
        appendMapped(state, command);
    } else if (op == "closepath" || op == "cp") {
        Type1PathCommand command; command.kind = Type1PathCommand::Close;
        state->currentPath.push_back(command);
    } else if (op == "newpath") {
        state->currentPath.clear();
    } else if (op == "arc" || op == "arcn" || op == "a" || op == "an") {
        if (!popNumbers(state, 5, &numbers)) return false;
        const double cx = numbers.at(0), cy = numbers.at(1), radius = numbers.at(2);
        double start = numbers.at(3), finish = numbers.at(4);
        const bool clockwise = op == "arcn" || op == "an";
        if (clockwise) while (finish > start) finish -= 360;
        else while (finish < start) finish += 360;
        const int segments = qBound(1, int(qCeil(qAbs(finish - start) / 10.0)), 360);
        for (int i = 0; i <= segments; ++i) {
            const double angle = (start + (finish - start) * i / segments) *
                3.14159265358979323846 / 180.0;
            Type1PathCommand command;
            command.kind = i == 0 && state->currentPath.isEmpty()
                ? Type1PathCommand::Move : Type1PathCommand::Line;
            command.x1 = cx + radius * qCos(angle); command.y1 = cy + radius * qSin(angle);
            state->currentX = command.x1; state->currentY = command.y1;
            appendMapped(state, command);
        }
    } else if (op == "fill" || op == "eofill" || op == "stroke") {
        if (!state->currentPath.isEmpty()) {
            for (const Type1PathCommand& command : state->currentPath)
                state->glyph->path.push_back(command);
            Type1PathCommand marker;
            marker.kind = op == "stroke" ? Type1PathCommand::Stroke : Type1PathCommand::Fill;
            marker.x1 = state->state.lineWidth * qSqrt(qAbs(
                state->state.scaleX * state->state.scaleY));
            marker.x2 = state->state.lineCap;
            state->glyph->path.push_back(marker);
            state->currentPath.clear();
        }
    } else if (op == "setlinewidth") {
        if (!popNumbers(state, 1, &numbers) || numbers.at(0) < 0) return false;
        state->state.lineWidth = numbers.at(0);
    } else if (op == "setlinecap") {
        if (!popNumbers(state, 1, &numbers) || numbers.at(0) < 0 || numbers.at(0) > 2)
            return false;
        state->state.lineCap = int(numbers.at(0));
    } else if (op == "setgray") {
        if (!popNumbers(state, 1, &numbers)) return false;
    } else if (op == "setrgbcolor") {
        if (!popNumbers(state, 3, &numbers)) return false;
    } else if (op == "setlinejoin") {
        if (!popNumbers(state, 1, &numbers) || numbers.at(0) < 0 || numbers.at(0) > 2)
            return false;
    } else if (op == "setcachedevice" || op == "setcharwidth" ||
               op == "d0" || op == "d1") {
        if (state->widthSet) return false;
        const int count = op == "setcachedevice" ? 6
            : op == "d1" ? 6 : op == "d0" || op == "setcharwidth" ? 2 : 0;
        if (!popNumbers(state, count, &numbers)) return false;
        state->advance = numbers.at(0);
        state->widthSet = true;
    } else if (op == "findfont") {
        if (state->stack.isEmpty() || state->stack.last().kind != PsValue::Name) return false;
        state->state.fontName = state->stack.takeLast().bytes;
        PsValue value; value.kind = PsValue::Dictionary; state->stack.push_back(value);
    } else if (op == "scalefont") {
        if (!popNumbers(state, 1, &numbers) || state->stack.isEmpty() ||
            state->stack.last().kind != PsValue::Dictionary) return false;
        state->stack.removeLast(); state->state.fontSize = numbers.at(0);
        PsValue value; value.kind = PsValue::Dictionary; state->stack.push_back(value);
    } else if (op == "setfont") {
        if (state->stack.isEmpty() || state->stack.last().kind != PsValue::Dictionary) return false;
        state->stack.removeLast();
    } else if (op == "show") {
        if (state->stack.isEmpty() || state->stack.last().kind != PsValue::String) return false;
        const QByteArray bytes = state->stack.takeLast().bytes;
        Type1TextRun run;
        run.family = QString::fromLatin1(state->state.fontName);
        run.text = QString::fromLatin1(bytes);
        run.fontSize = state->state.fontSize;
        run.scaleX = state->state.scaleX; run.scaleY = state->state.scaleY;
        run.offsetX = state->currentX * state->state.scaleX + state->state.offsetX;
        run.offsetY = state->currentY * state->state.scaleY + state->state.offsetY;
        state->glyph->textRuns.push_back(run);
        state->currentX += bytes.size() * run.fontSize * 0.6;
    } else if (op == "for") {
        if (!popValues(state, 4, &values) || values.at(0).kind != PsValue::Number ||
            values.at(1).kind != PsValue::Number || values.at(2).kind != PsValue::Number ||
            values.at(3).kind != PsValue::Procedure || values.at(1).number == 0)
            return false;
        const double first = values.at(0).number, step = values.at(1).number;
        const double limit = values.at(2).number;
        for (double i = first; step > 0 ? i <= limit : i >= limit; i += step) {
            if (++state->operations > kMaximumOperations || !qIsFinite(i)) return false;
            PsValue item; item.kind = PsValue::Number; item.number = i;
            state->stack.push_back(item);
            if (!executeProcedure(values.at(3).bytes, state, depth + 1)) return false;
        }
    } else {
        return false;
    }
    return state->stack.size() <= 256 &&
        state->glyph->path.size() <= kMaximumCommands &&
        state->currentPath.size() <= kMaximumCommands &&
        state->glyph->textRuns.size() <= kMaximumGlyphs &&
        qIsFinite(state->state.scaleX) && qIsFinite(state->state.scaleY) &&
        qIsFinite(state->state.offsetX) && qIsFinite(state->state.offsetY) &&
        qAbs(state->state.scaleX) <= 10000000.0 &&
        qAbs(state->state.scaleY) <= 10000000.0 &&
        qAbs(state->state.offsetX) <= 10000000.0 &&
        qAbs(state->state.offsetY) <= 10000000.0;
}

bool executeProcedure(const QByteArray& data, PsExecution* state, int depth)
{
    if (depth > 32) return false;
    int position = 0;
    while (position < data.size()) {
        skipSpaceAndComments(data, &position);
        if (position >= data.size()) break;
        PsValue value;
        if (!readPsValue(data, &position, &value)) {
            state->failure = QStringLiteral("Invalid token at byte %1").arg(position);
            return false;
        }
        if (++state->operations > kMaximumOperations) {
            state->failure = QStringLiteral("PostScript operation limit exceeded");
            return false;
        }
        if (value.kind == PsValue::Name && value.executable) {
            if (state->procedures->contains(value.bytes)) {
                if (!executeProcedure(state->procedures->value(value.bytes), state, depth + 1))
                    return false;
            } else if (state->variables.contains(value.bytes)) {
                const PsValue resolved = state->variables.value(value.bytes);
                if (resolved.kind == PsValue::Procedure) {
                    if (!executeProcedure(resolved.bytes, state, depth + 1)) return false;
                } else {
                    state->stack.push_back(resolved);
                }
            } else if (!executeOperator(value.bytes, state, depth)) {
                state->failure = QStringLiteral("Unsupported PostScript operator: %1")
                    .arg(QString::fromLatin1(value.bytes));
                return false;
            }
        } else {
            if (state->stack.size() >= 256) {
                state->failure = QStringLiteral("PostScript operand stack limit exceeded");
                return false;
            }
            state->stack.push_back(value);
        }
    }
    return true;
}

QHash<QByteArray, QByteArray> collectProcedures(const QByteArray& data)
{
    QHash<QByteArray, QByteArray> procedures;
    int position = 0;
    while (position < data.size()) {
        skipSpaceAndComments(data, &position);
        if (position >= data.size()) break;
        if (data.at(position) == '(') {
            PsValue ignored;
            if (!readPsValue(data, &position, &ignored)) break;
        } else if (data.at(position) == '{') {
            QByteArray ignored;
            if (!readProcedure(data, &position, &ignored)) break;
        } else {
            const QByteArray token = readToken(data, &position);
            if (!token.startsWith('/') || token.size() < 2) continue;
            const QByteArray name = token.mid(1);
            skipSpaceAndComments(data, &position);
            if (position < data.size() && data.at(position) == '{') {
                QByteArray procedure;
                if (!readProcedure(data, &position, &procedure)) break;
                procedures.insert(name, procedure);
                skipSpaceAndComments(data, &position);
                if (data.mid(position, 4) == "bind") {
                    position += 4;
                    skipSpaceAndComments(data, &position);
                }
                if (data.mid(position, 3) == "def") position += 3;
            }
        }
    }
    return procedures;
}

QVector<QByteArray> readEncoding(const QByteArray& data)
{
    QVector<QByteArray> encoding;
    const int start = data.indexOf("/Encoding");
    const int array = start < 0 ? -1 : data.indexOf('[', start + 9);
    if (array < 0) return encoding;
    int position = array + 1;
    while (position < data.size()) {
        const QByteArray token = readToken(data, &position);
        if (token == "]") break;
        if (token.startsWith('/') && token.size() > 1) encoding.push_back(token.mid(1));
        if (encoding.size() > 256) return {};
    }
    return encoding;
}

bool parseCharacterDefs(const QByteArray& data, Type1Font* font, QString* error)
{
    const int marker = data.indexOf("CharacterDefs begin");
    if (marker < 0) return false;
    const QHash<QByteArray, QByteArray> procedures = collectProcedures(data);
    const QVector<QByteArray> encoding = readEncoding(data);
    if (encoding.isEmpty()) return false;
    int position = marker + int(sizeof("CharacterDefs begin")) - 1;
    QHash<QByteArray, QByteArray> definitions;
    while (position < data.size()) {
        const QByteArray token = readToken(data, &position);
        if (token == "end") break;
        if (token.isEmpty()) continue;
        if (!token.startsWith('/') || token.size() < 2) return false;
        skipSpaceAndComments(data, &position);
        QByteArray body;
        if (!readProcedure(data, &position, &body)) return false;
        definitions.insert(token.mid(1), body);
        skipSpaceAndComments(data, &position);
        readToken(data, &position);
    }
    for (int code = 0; code < encoding.size(); ++code) {
        const QByteArray name = encoding.at(code);
        if (!definitions.contains(name)) continue;
        Type1Glyph glyph;
        glyph.name = QString::fromLatin1(name);
        glyph.codepoint = quint32(code);
        glyph.advance = 600;
        PsExecution state;
        state.procedures = &procedures;
        state.glyph = &glyph;
        state.advance = glyph.advance;
        state.widthSet = true;
        if (!executeProcedure(definitions.value(name), &state, 0)) {
            if (error) *error = QStringLiteral("Unsupported Type 3 CharacterDefs procedure: %1")
                .arg(glyph.name) + (state.failure.isEmpty() ? QString()
                    : QStringLiteral(" (%1)").arg(state.failure));
            return false;
        }
        font->glyphs.push_back(glyph);
        if (font->glyphs.size() > kMaximumGlyphs) return false;
    }
    return !font->glyphs.isEmpty();
}

bool parseCharProc(const QByteArray& data, const QHash<QByteArray, QByteArray>& procedures,
                   Type1Glyph* glyph)
{
    PsExecution state;
    state.procedures = &procedures;
    state.glyph = glyph;
    if (!executeProcedure(data, &state, 0) || !state.widthSet) return false;
    glyph->advance = state.advance;
    return true;
}

quint32 codepointForName(const QString& name)
{
    if (name.size() == 1 && name.at(0).unicode() >= 32 && name.at(0).unicode() <= 126)
        return name.at(0).unicode();
    if (name == QLatin1String("space")) return 32;
    static const char* const digits[] = {"zero", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine"};
    for (quint32 i = 0; i < 10; ++i)
        if (name == QLatin1String(digits[i])) return 48 + i;
    return 0xffffffffu;
}

} // namespace

bool hasType3Header(const QByteArray& data)
{
    if (data.isEmpty()) return false;
    return QRegularExpression(QStringLiteral("/FontType\\s+3\\b"))
        .match(QString::fromLatin1(data.left(qMin(data.size(), 1024 * 1024)))).hasMatch();
}

bool parseType3Font(const QByteArray& data, Type1Font* font, QString* error)
{
    if (!font || data.isEmpty() || data.size() > kMaximumBytes || data.contains('\0'))
        return fail(error, QStringLiteral("Invalid Type 3 font input size or encoding"));
    *font = {};
    const QString source = QString::fromLatin1(data);
    const QRegularExpression fontType(QStringLiteral("/FontType\\s+3\\b"));
    if (!fontType.match(source).hasMatch())
        return fail(error, QStringLiteral("Not a Type 3 PostScript font"));
    const QRegularExpression namePattern(QStringLiteral("/FontName\\s+/([^\\s/]+)"));
    const QRegularExpressionMatch nameMatch = namePattern.match(source);
    if (!nameMatch.hasMatch()) return fail(error, QStringLiteral("Type 3 font has no FontName"));

    Type1Font parsed;
    parsed.name = nameMatch.captured(1);
    const QRegularExpression matrix(QStringLiteral("/FontMatrix\\s*\\[\\s*([0-9.eE+-]+)"));
    const QRegularExpressionMatch matrixMatch = matrix.match(source);
    if (matrixMatch.hasMatch()) {
        bool ok = false;
        const double value = matrixMatch.captured(1).toDouble(&ok);
        if (!ok || !qIsFinite(value) || qAbs(value) < 1.0e-12)
            return fail(error, QStringLiteral("Invalid Type 3 FontMatrix"));
        parsed.unitsPerEm = 1.0 / qAbs(value);
    }
    const QRegularExpression bbox(QStringLiteral("/FontBBox\\s*\\[\\s*(-?[0-9.]+)\\s+(-?[0-9.]+)\\s+(-?[0-9.]+)\\s+(-?[0-9.]+)"));
    const QRegularExpressionMatch bboxMatch = bbox.match(source);
    if (bboxMatch.hasMatch()) {
        bool ok = false;
        parsed.descent = bboxMatch.captured(2).toDouble(&ok);
        if (!ok) return fail(error, QStringLiteral("Invalid Type 3 FontBBox"));
        parsed.ascent = bboxMatch.captured(4).toDouble(&ok);
        if (!ok || !qIsFinite(parsed.ascent) || !qIsFinite(parsed.descent) ||
            qAbs(parsed.ascent) > 10000000.0 || qAbs(parsed.descent) > 10000000.0 ||
            parsed.ascent <= parsed.descent)
            return fail(error, QStringLiteral("Invalid Type 3 FontBBox"));
    }

    const int charProcs = data.indexOf("/CharProcs");
    if (charProcs < 0) {
        if (!parseCharacterDefs(data, &parsed, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Type 3 font has no supported glyph dictionary");
            return false;
        }
        *font = parsed;
        if (error) error->clear();
        return true;
    }
    const int begin = charProcs < 0 ? -1 : data.indexOf("begin", charProcs + 10);
    if (begin < 0) return fail(error, QStringLiteral("Type 3 font has no CharProcs dictionary"));
    int position = begin + 5;
    bool foundNotdef = false;
    quint64 totalCommands = 0;
    QSet<QString> glyphNames;
    const QHash<QByteArray, QByteArray> procedures = collectProcedures(data);
    while (position < data.size()) {
        const QByteArray token = readToken(data, &position);
        if (token.isEmpty()) continue;
        if (token == "end") break;
        if (!token.startsWith('/') || token.size() == 1)
            return fail(error, QStringLiteral("Invalid Type 3 CharProcs entry"));
        skipSpaceAndComments(data, &position);
        QByteArray procedure;
        if (!readProcedure(data, &position, &procedure))
            return fail(error, QStringLiteral("Invalid Type 3 CharProc procedure"));
        Type1Glyph glyph;
        glyph.name = QString::fromLatin1(token.mid(1));
        if (glyphNames.contains(glyph.name))
            return fail(error, QStringLiteral("Duplicate Type 3 CharProc name"));
        glyphNames.insert(glyph.name);
        glyph.codepoint = codepointForName(glyph.name);
        if (!parseCharProc(procedure, procedures, &glyph))
            return fail(error, QStringLiteral("Type 3 CharProc uses unsupported operators"));
        if (glyph.name == QLatin1String(".notdef")) foundNotdef = true;
        totalCommands += quint64(glyph.path.size());
        if (totalCommands > 1000000u)
            return fail(error, QStringLiteral("Type 3 font has too many path commands"));
        parsed.glyphs.push_back(glyph);
        if (parsed.glyphs.size() > kMaximumGlyphs)
            return fail(error, QStringLiteral("Type 3 font has too many glyphs"));
        skipSpaceAndComments(data, &position);
        if (data.mid(position, 4) == "bind") position += 4;
        const QByteArray terminator = readToken(data, &position);
        if (terminator != "def" && terminator != "put")
            return fail(error, QStringLiteral("Type 3 CharProc has no dictionary terminator"));
    }
    if (!foundNotdef || parsed.glyphs.isEmpty())
        return fail(error, QStringLiteral("Type 3 font requires .notdef and at least one glyph"));
    *font = parsed;
    if (error) error->clear();
    return true;
}

} // namespace font
} // namespace peare
