#include "Type1Font.h"

#include <QHash>
#include <QRegularExpression>
#include <QtMath>

#include <algorithm>

namespace peare {
namespace font {
namespace {

const int kMaxFontBytes = 64 * 1024 * 1024;
const quint32 kCipherC1 = 52845;
const quint32 kCipherC2 = 22719;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return quint32(quint8(data.at(offset))) |
           (quint32(quint8(data.at(offset + 1))) << 8) |
           (quint32(quint8(data.at(offset + 2))) << 16) |
           (quint32(quint8(data.at(offset + 3))) << 24);
}

quint16 readBe16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset)) << 8) | quint8(data.at(offset + 1));
}

quint32 readBe24(const QByteArray& data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 16) |
           (quint32(quint8(data.at(offset + 1))) << 8) |
           quint8(data.at(offset + 2));
}

quint32 readBe32(const QByteArray& data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24) |
           (quint32(quint8(data.at(offset + 1))) << 16) |
           (quint32(quint8(data.at(offset + 2))) << 8) |
           quint8(data.at(offset + 3));
}

bool rangeFits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

QByteArray decrypt(const QByteArray& encrypted, quint16 seed)
{
    QByteArray output;
    output.resize(encrypted.size());
    quint32 state = seed;
    for (int index = 0; index < encrypted.size(); ++index) {
        const quint8 cipher = quint8(encrypted.at(index));
        output[index] = char(cipher ^ (state >> 8));
        state = ((cipher + state) * kCipherC1 + kCipherC2) & 0xFFFFu;
    }
    return output;
}

void skipSpace(const QByteArray& data, int* position)
{
    while (*position < data.size()) {
        const uchar value = uchar(data.at(*position));
        if (value == '%' ) {
            while (*position < data.size() && data.at(*position) != '\n' &&
                   data.at(*position) != '\r')
                ++*position;
        } else if (value == 0 || value == 9 || value == 10 || value == 12 ||
                   value == 13 || value == 32) {
            ++*position;
        } else {
            break;
        }
    }
}

QByteArray readToken(const QByteArray& data, int* position)
{
    skipSpace(data, position);
    const int start = *position;
    if (*position < data.size() && data.at(*position) == '/') {
        ++*position;
        while (*position < data.size()) {
            const uchar value = uchar(data.at(*position));
            if (value == 0 || value == 9 || value == 10 || value == 12 ||
                value == 13 || value == 32 || value == '[' || value == ']' ||
                value == '{' || value == '}' || value == '(' || value == ')' ||
                value == '<' || value == '>' || value == '/')
                break;
            ++*position;
        }
        return data.mid(start, *position - start);
    }
    while (*position < data.size()) {
        const uchar value = uchar(data.at(*position));
        if (value == 0 || value == 9 || value == 10 || value == 12 ||
            value == 13 || value == 32 || value == '[' || value == ']' ||
            value == '{' || value == '}' || value == '(' || value == ')' ||
            value == '<' || value == '>' || value == '/')
            break;
        ++*position;
    }
    return data.mid(start, *position - start);
}

bool readInteger(const QByteArray& data, int* position, int* value)
{
    bool ok = false;
    const int parsed = readToken(data, position).toInt(&ok);
    if (!ok) return false;
    *value = parsed;
    return true;
}

bool readBinaryRecord(const QByteArray& data, int* position, QByteArray* bytes)
{
    int length = 0;
    if (!readInteger(data, position, &length) || length < 0 || length > kMaxFontBytes)
        return false;
    const QByteArray marker = readToken(data, position);
    if (marker != "RD" && marker != "-|" && marker != "|-")
        return false;
    if (*position >= data.size() ||
        (data.at(*position) != ' ' && data.at(*position) != '\t' &&
         data.at(*position) != '\r' && data.at(*position) != '\n'))
        return false;
    if (data.at(*position) == '\r' && *position + 1 < data.size() &&
        data.at(*position + 1) == '\n')
        *position += 2;
    else
        ++*position;
    if (length > data.size() - *position)
        return false;
    *bytes = data.mid(*position, length);
    *position += length;
    return true;
}

int hexValue(uchar value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hasPfaHeader(const QByteArray& data)
{
    return data.startsWith("%!PS-") || data.startsWith("%!FontType1-");
}

bool appendPfbSegment(QByteArray* output, quint8 type, const QByteArray& bytes)
{
    if (!output || (type != 1 && type != 2) || bytes.isEmpty() ||
        bytes.size() > kMaxFontBytes - output->size() - 6)
        return false;
    const quint32 length = quint32(bytes.size());
    output->append(char(0x80));
    output->append(char(type));
    output->append(char(length));
    output->append(char(length >> 8));
    output->append(char(length >> 16));
    output->append(char(length >> 24));
    output->append(bytes);
    return true;
}

bool macPostPfb(const QByteArray& data, QByteArray* output)
{
    if (!output || data.size() < 128 || data.size() > kMaxFontBytes) return false;
    const quint32 dataOffset = readBe32(data, 0);
    const quint32 mapOffset = readBe32(data, 4);
    const quint32 dataLength = readBe32(data, 8);
    const quint32 mapLength = readBe32(data, 12);
    if (dataOffset < 16 || mapOffset < 16 || mapLength < 30 ||
        !rangeFits(dataOffset, dataLength, quint64(data.size())) ||
        !rangeFits(mapOffset, mapLength, quint64(data.size())) ||
        (quint64(dataOffset) < quint64(mapOffset) + mapLength &&
         quint64(mapOffset) < quint64(dataOffset) + dataLength))
        return false;

    const int map = int(mapOffset);
    if (data.mid(map, 16) != data.left(16) && data.mid(map, 16) != QByteArray(16, '\0'))
        return false;
    const quint16 typeListOffset = readBe16(data, map + 24);
    const quint16 nameListOffset = readBe16(data, map + 26);
    if (typeListOffset < 28 || nameListOffset < typeListOffset || nameListOffset > mapLength)
        return false;
    const quint16 countMinusOne = readBe16(data, map + typeListOffset);
    if (countMinusOne == 0xffff) return false;
    const quint32 typeCount = quint32(countMinusOne) + 1;
    const quint64 entriesStart = quint64(typeListOffset) + 2;
    const quint64 referencesStart = entriesStart + quint64(typeCount) * 8;
    if (typeCount > 4096 || referencesStart > nameListOffset) return false;

    struct PostResource { qint16 id; QByteArray bytes; };
    QVector<PostResource> resources;
    quint64 totalReferences = 0;
    for (quint32 typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
        const int typeEntry = map + int(entriesStart + quint64(typeIndex) * 8);
        const quint16 itemCountMinusOne = readBe16(data, typeEntry + 4);
        if (itemCountMinusOne == 0xffff) return false;
        const quint32 itemCount = quint32(itemCountMinusOne) + 1;
        totalReferences += itemCount;
        if (totalReferences > 65536) return false;
        if (data.mid(typeEntry, 4) != QByteArrayLiteral("POST")) continue;

        const quint16 referenceOffset = readBe16(data, typeEntry + 6);
        const quint64 referenceEnd = quint64(referenceOffset) + quint64(itemCount) * 12;
        if (referenceOffset < 2 + quint64(typeCount) * 8 ||
            referenceEnd > quint64(nameListOffset) - typeListOffset)
            return false;
        for (quint32 itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            const int reference = map + int(typeListOffset) + int(referenceOffset) +
                                  int(itemIndex * 12);
            const qint16 id = qint16(readBe16(data, reference));
            const quint32 payloadOffset = readBe24(data, reference + 5);
            if (!rangeFits(payloadOffset, 4, dataLength)) return false;
            const quint64 record = quint64(dataOffset) + payloadOffset;
            const quint32 length = readBe32(data, int(record));
            if (length > quint32(kMaxFontBytes) ||
                !rangeFits(quint64(payloadOffset) + 4, length, dataLength))
                return false;
            resources.append({id, data.mid(int(record + 4), int(length))});
        }
    }
    if (resources.isEmpty()) return false;
    std::sort(resources.begin(), resources.end(), [](const PostResource& left,
                                                     const PostResource& right) {
        return left.id < right.id;
    });

    QByteArray pfb;
    bool ended = false;
    for (const PostResource& resource : resources) {
        const QByteArray& bytes = resource.bytes;
        if (bytes.size() < 2 || bytes.at(1) != '\0') return false;
        const quint8 type = quint8(bytes.at(0));
        if (type == 0) continue;
        if (ended) return false;
        if (type == 1 || type == 2) {
            if (!appendPfbSegment(&pfb, type, bytes.mid(2))) return false;
        } else if (type == 3 || type == 5) {
            if (bytes.size() != 2) return false;
            pfb.append(char(0x80));
            pfb.append(char(3));
            ended = true;
        } else {
            return false;
        }
    }
    if (!ended || !hasPfbHeader(pfb)) return false;
    *output = pfb;
    return true;
}

QByteArray macPostPfa(const QByteArray& data)
{
    if (data.size() > 2 && uchar(data.at(0)) == 1 && data.at(1) == '\0' &&
        hasPfaHeader(data.mid(2)))
        return data.mid(2);
    return data;
}

bool extractType1(const QByteArray& data, QByteArray* clear, QByteArray* encrypted,
                  QString* error)
{
    const QByteArray normalized = macPostPfa(data);
    if (normalized.size() < 16 || normalized.size() > kMaxFontBytes)
        return fail(error, QStringLiteral("Type 1 font size is outside the supported range."));
    if (!hasPfbHeader(normalized)) {
        const QRegularExpression eexec(QStringLiteral("currentfile\\s+eexec\\b"));
        const QRegularExpressionMatch match = eexec.match(QString::fromLatin1(normalized));
        const int markerEnd = match.hasMatch() ? match.capturedEnd() : -1;
        const int trailer = normalized.indexOf("cleartomark", qMax(0, markerEnd));
        if (!hasPfaHeader(normalized) || markerEnd < 0 || trailer <= markerEnd)
            return fail(error, QStringLiteral("Invalid PFA clear-text or eexec boundary."));
        *clear = normalized.left(markerEnd);
        int highNibble = -1;
        for (int index = markerEnd; index < trailer; ++index) {
            const uchar value = uchar(normalized.at(index));
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
                value == '\f' || value == '\v')
                continue;
            const int nibble = hexValue(value);
            if (nibble < 0)
                return fail(error, QStringLiteral("PFA eexec data contains a non-hexadecimal byte."));
            if (highNibble < 0) highNibble = nibble;
            else { encrypted->append(char((highNibble << 4) | nibble)); highNibble = -1; }
        }
        if (highNibble >= 0) encrypted->append(char(highNibble << 4));
        return !encrypted->isEmpty() ||
               fail(error, QStringLiteral("PFA eexec data is empty."));
    }
    int position = 0;
    bool eexecSeen = false;
    bool endSeen = false;
    while (position < normalized.size()) {
        if (position + 2 > normalized.size() || uchar(normalized.at(position)) != 0x80)
            return fail(error, QStringLiteral("Invalid PFB marker at offset %1.").arg(position));
        const quint8 type = quint8(normalized.at(position + 1));
        if (type == 3) {
            position += 2;
            endSeen = true;
            break;
        }
        if ((type != 1 && type != 2) || position + 6 > normalized.size())
            return fail(error, QStringLiteral("Invalid PFB segment type or truncated header at offset %1.").arg(position));
        const quint32 length = readLe32(normalized, position + 2);
        position += 6;
        if (length > quint32(normalized.size() - position))
            return fail(error, QStringLiteral("PFB segment at offset %1 exceeds the file length.").arg(position - 6));
        const QByteArray segment = normalized.mid(position, int(length));
        position += int(length);
        if (!eexecSeen) {
            if (type != 1)
                return fail(error, QStringLiteral("PFB encrypted data starts before the eexec marker."));
            clear->append(segment);
            if (clear->contains("eexec")) eexecSeen = true;
        } else if (type == 2) {
            encrypted->append(segment);
        }
    }
    if (!endSeen || position != normalized.size() || !eexecSeen || encrypted->isEmpty() ||
        !(clear->startsWith("%!PS-") || clear->startsWith("%!FontType1-")))
        return fail(error, QStringLiteral("Invalid PFB stream (end=%1, trailing=%2, eexec=%3, encrypted=%4, header=%5).")
            .arg(endSeen).arg(normalized.size() - position).arg(eexecSeen)
            .arg(encrypted->size()).arg(QString::fromLatin1(clear->left(16))));
    return true;
}

double parseMetric(const QByteArray& data, const QByteArray& pattern, double fallback)
{
    const QRegularExpression expression(QString::fromLatin1(pattern));
    const QRegularExpressionMatch match = expression.match(QString::fromLatin1(data));
    if (!match.hasMatch()) return fallback;
    bool ok = false;
    const double value = match.captured(1).toDouble(&ok);
    return ok && qIsFinite(value) ? value : fallback;
}

quint32 standardCodepoint(const QString& name)
{
    if (name.size() == 1 && name.at(0).unicode() >= 33 && name.at(0).unicode() <= 126)
        return name.at(0).unicode();
    static const QHash<QString, quint32> names = {
        {QStringLiteral("space"), 32}, {QStringLiteral("exclam"), 33},
        {QStringLiteral("quotedbl"), 34}, {QStringLiteral("numbersign"), 35},
        {QStringLiteral("dollar"), 36}, {QStringLiteral("percent"), 37},
        {QStringLiteral("ampersand"), 38}, {QStringLiteral("quoteright"), 39},
        {QStringLiteral("parenleft"), 40}, {QStringLiteral("parenright"), 41},
        {QStringLiteral("asterisk"), 42}, {QStringLiteral("plus"), 43},
        {QStringLiteral("comma"), 44}, {QStringLiteral("hyphen"), 45},
        {QStringLiteral("period"), 46}, {QStringLiteral("slash"), 47},
        {QStringLiteral("colon"), 58}, {QStringLiteral("semicolon"), 59},
        {QStringLiteral("less"), 60}, {QStringLiteral("equal"), 61},
        {QStringLiteral("greater"), 62}, {QStringLiteral("question"), 63},
        {QStringLiteral("at"), 64}, {QStringLiteral("bracketleft"), 91},
        {QStringLiteral("backslash"), 92}, {QStringLiteral("bracketright"), 93},
        {QStringLiteral("asciicircum"), 94}, {QStringLiteral("underscore"), 95},
        {QStringLiteral("quoteleft"), 96}, {QStringLiteral("braceleft"), 123},
        {QStringLiteral("bar"), 124}, {QStringLiteral("braceright"), 125},
        {QStringLiteral("asciitilde"), 126}, {QStringLiteral("zero"), 48},
        {QStringLiteral("one"), 49}, {QStringLiteral("two"), 50},
        {QStringLiteral("three"), 51}, {QStringLiteral("four"), 52},
        {QStringLiteral("five"), 53}, {QStringLiteral("six"), 54},
        {QStringLiteral("seven"), 55}, {QStringLiteral("eight"), 56},
        {QStringLiteral("nine"), 57}
    };
    return names.value(name, 0);
}

struct CharState {
    double x = 0;
    double y = 0;
    double advance = 0;
    QVector<double> stack;
    QVector<double> otherSubrResults;
    QVector<Type1PathCommand> path;
    bool widthSet = false;
    int lastOpcode = -1;
    int lastEscape = -1;
    QString failure;
};

bool executeCharString(const QByteArray& bytes, const QVector<QByteArray>& subrs,
                       CharState* state, int depth, bool* ended)
{
    if (depth > 16 || !state || !ended) return false;
    for (int position = 0; position < bytes.size();) {
        const quint8 code = quint8(bytes.at(position++));
        state->lastOpcode = code;
        state->lastEscape = -1;
        if (code >= 32) {
            double value = 0;
            if (code <= 246) {
                value = int(code) - 139;
            } else if (code <= 250) {
                if (position >= bytes.size()) return false;
                value = (int(code) - 247) * 256 + quint8(bytes.at(position++)) + 108;
            } else if (code <= 254) {
                if (position >= bytes.size()) return false;
                value = -((int(code) - 251) * 256 + quint8(bytes.at(position++)) + 108);
            } else {
                if (position + 4 > bytes.size()) return false;
                const quint32 raw = (quint32(quint8(bytes.at(position))) << 24) |
                    (quint32(quint8(bytes.at(position + 1))) << 16) |
                    (quint32(quint8(bytes.at(position + 2))) << 8) |
                    quint32(quint8(bytes.at(position + 3)));
                position += 4;
                value = qint32(raw) / 65536.0;
            }
            if (!qIsFinite(value) || state->stack.size() >= 48) return false;
            state->stack.append(value);
            continue;
        }
        auto pop = [&](double* value) {
            if (state->stack.isEmpty()) return false;
            *value = state->stack.takeLast();
            return true;
        };
        auto move = [&](double dx, double dy) {
            state->x += dx; state->y += dy;
            Type1PathCommand command; command.kind = Type1PathCommand::Move;
            command.x1 = state->x; command.y1 = state->y; state->path.append(command);
        };
        auto line = [&](double dx, double dy) {
            state->x += dx; state->y += dy;
            Type1PathCommand command; command.kind = Type1PathCommand::Line;
            command.x1 = state->x; command.y1 = state->y; state->path.append(command);
        };
        if (code == 1 || code == 2 || code == 3) {
            state->stack.clear();
        } else if (code == 4 || code == 22) {
            double dy = 0;
            if (!pop(&dy) || state->stack.size() != 0) return false;
            move(code == 22 ? dy : 0, code == 4 ? dy : 0);
        } else if (code == 5) {
            if (state->stack.isEmpty() || (state->stack.size() & 1)) return false;
            const QVector<double> values = state->stack;
            state->stack.clear();
            for (int index = 0; index < values.size(); index += 2)
                line(values.at(index), values.at(index + 1));
        } else if (code == 6 || code == 7) {
            if (state->stack.isEmpty()) return false;
            const QVector<double> values = state->stack;
            state->stack.clear();
            bool horizontal = code == 6;
            for (double delta : values) {
                line(horizontal ? delta : 0, horizontal ? 0 : delta);
                horizontal = !horizontal;
            }
        } else if (code == 8) {
            if (state->stack.isEmpty() || state->stack.size() % 6) return false;
            while (!state->stack.isEmpty()) {
                double values[6];
                for (int index = 5; index >= 0; --index)
                    if (!pop(&values[index])) return false;
                Type1PathCommand command; command.kind = Type1PathCommand::Curve;
                command.x1 = state->x + values[0]; command.y1 = state->y + values[1];
                command.x2 = command.x1 + values[2]; command.y2 = command.y1 + values[3];
                command.x3 = command.x2 + values[4]; command.y3 = command.y2 + values[5];
                state->x = command.x3; state->y = command.y3; state->path.append(command);
            }
        } else if (code == 9) {
            if (!state->stack.isEmpty()) return false;
            Type1PathCommand command; command.kind = Type1PathCommand::Close;
            state->path.append(command);
        } else if (code == 10) {
            double index = 0;
            if (!pop(&index) || index < 0 || index >= subrs.size() || index != qFloor(index)) {
                state->failure = QStringLiteral("invalid subroutine index");
                return false;
            }
            bool returned = false;
            if (!executeCharString(subrs.at(int(index)), subrs, state, depth + 1, &returned) ||
                !returned) {
                state->failure = QStringLiteral("subroutine %1 did not return").arg(int(index));
                return false;
            }
        } else if (code == 11) {
            *ended = true;
            return true;
        } else if (code == 13) {
            double width = 0, bearing = 0;
            if (state->widthSet || !pop(&width) || !pop(&bearing) || !state->stack.isEmpty())
                return false;
            state->x = bearing; state->y = 0; state->advance = width; state->widthSet = true;
        } else if (code == 14) {
            if (!state->stack.isEmpty() || !state->widthSet) {
                state->failure = QStringLiteral("invalid endchar state");
                return false;
            }
            *ended = true;
            return true;
        } else if (code == 21) {
            double dy = 0, dx = 0;
            if (!pop(&dy) || !pop(&dx) || !state->stack.isEmpty()) return false;
            move(dx, dy);
        } else if (code == 30 || code == 31) {
            if (state->stack.size() != 4 && state->stack.size() != 5) return false;
            double v[5] = {};
            const int count = state->stack.size();
            for (int index = count - 1; index >= 0; --index) pop(&v[index]);
            Type1PathCommand command; command.kind = Type1PathCommand::Curve;
            if (code == 31) {
                command.x1 = state->x + v[0]; command.y1 = state->y;
                command.x2 = command.x1 + v[1]; command.y2 = command.y1 + v[2];
                command.x3 = command.x2 + (count == 5 ? v[4] : 0);
                command.y3 = command.y2 + v[3];
            } else {
                command.x1 = state->x; command.y1 = state->y + v[0];
                command.x2 = command.x1 + v[1]; command.y2 = command.y1 + v[2];
                command.x3 = command.x2 + v[3];
                command.y3 = command.y2 + (count == 5 ? v[4] : 0);
            }
            state->x = command.x3; state->y = command.y3; state->path.append(command);
        } else if (code == 12) {
            if (position >= bytes.size()) return false;
            const quint8 escape = quint8(bytes.at(position++));
            state->lastEscape = escape;
            if (escape == 0 || escape == 1 || escape == 2) {
                state->stack.clear();
            } else if (escape == 12) {
                double denominator = 0, numerator = 0;
                if (!pop(&denominator) || !pop(&numerator) || denominator == 0) return false;
                state->stack.append(numerator / denominator);
            } else if (escape == 6) {
                double accent = 0, base = 0, dy = 0, dx = 0, sideBearing = 0;
                if (!pop(&accent) || !pop(&base) || !pop(&dy) || !pop(&dx) ||
                    !pop(&sideBearing) || !state->stack.isEmpty() ||
                    accent < 0 || accent > 255 || base < 0 || base > 255)
                    return false;
                *ended = true;
                return true;
            } else if (escape == 16) {
                double subr = 0, count = 0;
                if (!pop(&subr) || !pop(&count) || count < 0 || count > 48 ||
                    subr < 0 || subr > 65535 || subr != qFloor(subr) ||
                    count != qFloor(count) || state->stack.size() < int(count))
                    return false;
                QVector<double> arguments;
                arguments.resize(int(count));
                for (int index = int(count) - 1; index >= 0; --index)
                    if (!pop(&arguments[index])) return false;
                if (int(subr) == 0 && arguments.size() >= 3) {
                    state->otherSubrResults.append(arguments.at(arguments.size() - 2));
                    state->otherSubrResults.append(arguments.last());
                } else if (int(subr) == 3 && !arguments.isEmpty()) {
                    state->otherSubrResults.append(arguments.first());
                } else if (int(subr) > 3) {
                    for (int index = arguments.size() - 1; index >= 0; --index)
                        state->otherSubrResults.append(arguments.at(index));
                }
            } else if (escape == 17) {
                if (state->otherSubrResults.isEmpty()) return false;
                state->stack.append(state->otherSubrResults.takeLast());
            } else if (escape == 7) {
                double wy = 0, wx = 0, sy = 0, sx = 0;
                if (state->widthSet || !pop(&wy) || !pop(&wx) || !pop(&sy) || !pop(&sx) ||
                    !state->stack.isEmpty()) return false;
                state->x = sx; state->y = sy; state->advance = wx; state->widthSet = true;
            } else if (escape == 33) {
                double y = 0, x = 0;
                if (!pop(&y) || !pop(&x) || !state->stack.isEmpty()) return false;
                state->x = x; state->y = y;
            } else {
                state->failure = QStringLiteral("unsupported escape operator %1").arg(escape);
                return false;
            }
        } else {
            state->failure = QStringLiteral("unsupported operator %1").arg(code);
            return false;
        }
        if (qAbs(state->x) > 1.0e7 || qAbs(state->y) > 1.0e7 ||
            qAbs(state->advance) > 1.0e7 || state->path.size() > 65536)
            return false;
    }
    if (depth == 0 && state->widthSet && state->stack.isEmpty()) {
        *ended = true;
        return true;
    }
    state->failure = QStringLiteral("charstring ended without return or endchar");
    return false;
}

bool extractSubrs(const QByteArray& plain, int lenIV, QVector<QByteArray>* subrs,
                  QString* error)
{
    const int start = plain.indexOf("/Subrs");
    const int end = plain.indexOf("/CharStrings", start < 0 ? 0 : start);
    if (start < 0 || end < 0) return true;
    int arraySize = 0;
    int position = start + 6;
    if (!readInteger(plain, &position, &arraySize) || arraySize < 0 || arraySize > 65536)
        return fail(error, QStringLiteral("Invalid Type 1 Subrs declaration."));
    subrs->resize(arraySize);
    while (position < end) {
        const QByteArray token = readToken(plain, &position);
        if (token.isEmpty()) break;
        if (token == "ND" || token == "def" || token == "/OtherSubrs") break;
        if (token != "dup") continue;
        bool indexOk = false;
        const int index = readToken(plain, &position).toInt(&indexOk);
        if (!indexOk) break;
        QByteArray encrypted;
        if (index < 0 || index >= arraySize)
            return fail(error, QStringLiteral("Invalid Type 1 Subr index."));
        if (!readBinaryRecord(plain, &position, &encrypted) ||
            (lenIV >= 0 && encrypted.size() < lenIV))
            return fail(error, QStringLiteral("Invalid Type 1 Subr binary record at index %1.").arg(index));
        (*subrs)[index] = lenIV < 0 ? encrypted : decrypt(encrypted, 4330).mid(lenIV);
    }
    return true;
}

bool parseCharStrings(const QByteArray& plain, int lenIV, Type1Font* font, QString* error)
{
    const int start = plain.indexOf("/CharStrings");
    if (start < 0)
        return fail(error, QStringLiteral("Type 1 font has no CharStrings dictionary."));
    int position = start + 12;
    int declaredCount = 0;
    if (!readInteger(plain, &position, &declaredCount) || declaredCount < 1 ||
        declaredCount > 65536)
        return fail(error, QStringLiteral("Invalid Type 1 CharStrings declaration."));
    QVector<QByteArray> subrs;
    if (!extractSubrs(plain, lenIV, &subrs, error)) return false;
    while (position < plain.size()) {
        const QByteArray nameToken = readToken(plain, &position);
        if (nameToken == "end") break;
        if (!nameToken.startsWith('/')) continue;
        QByteArray encrypted;
        if (!readBinaryRecord(plain, &position, &encrypted) ||
            (lenIV >= 0 && encrypted.size() < lenIV))
            return fail(error, QStringLiteral("Invalid Type 1 CharString record for %1.")
                .arg(QString::fromLatin1(nameToken.mid(1))));
        const QByteArray decoded = lenIV < 0 ? encrypted : decrypt(encrypted, 4330).mid(lenIV);
        CharState state;
        bool ended = false;
        const bool executionValid = executeCharString(decoded, subrs, &state, 0, &ended);
        if (!executionValid || !ended || !state.widthSet) {
            if (error)
                *error = QStringLiteral("Invalid Type 1 CharString: %1")
                    .arg(QStringLiteral("%1 (operator %2, escape %3, executed=%4, ended=%5)")
                        .arg(QString::fromLatin1(nameToken.mid(1)))
                        .arg(state.lastOpcode).arg(state.lastEscape)
                        .arg(executionValid).arg(ended)
                        + QStringLiteral(" stack=%1 width=%2")
                            .arg(state.stack.size()).arg(state.widthSet)
                        + QStringLiteral(" reason=%1").arg(state.failure));
            return false;
        }
        Type1Glyph glyph;
        glyph.name = QString::fromLatin1(nameToken.mid(1));
        glyph.codepoint = standardCodepoint(glyph.name);
        glyph.advance = state.advance;
        glyph.path = state.path;
        font->glyphs.append(glyph);
        if (font->glyphs.size() > declaredCount)
            return fail(error, QStringLiteral("Type 1 CharStrings count exceeds its declaration."));
    }
    if (font->glyphs.isEmpty() || font->glyphs.size() > declaredCount) {
        if (error) *error = QStringLiteral("Invalid Type 1 CharStrings count.");
        return false;
    }
    return true;
}

} // namespace

bool hasPfbHeader(const QByteArray& data)
{
    if (data.size() < 12 || uchar(data.at(0)) != 0x80 || uchar(data.at(1)) != 1)
        return false;
    const quint32 length = readLe32(data, 2);
    return length >= 2 && data.mid(6, 2) == "%!";
}

bool hasType1Header(const QByteArray& data)
{
    if (hasPfbHeader(data)) return true;
    QByteArray postPfb;
    if (macPostPfb(data, &postPfb)) return true;
    const QByteArray normalized = macPostPfa(data);
    if (!hasPfaHeader(normalized)) return false;
    return QRegularExpression(QStringLiteral("currentfile\\s+eexec\\b"))
        .match(QString::fromLatin1(normalized.left(qMin(normalized.size(), 64 * 1024)))).hasMatch();
}

bool parseType1Font(const QByteArray& data, Type1Font* font, QString* error)
{
    if (!font) return fail(error, QStringLiteral("Missing Type 1 font output."));
    *font = {};
    QByteArray postPfb;
    const QByteArray input = macPostPfb(data, &postPfb) ? postPfb : data;
    QByteArray clear;
    QByteArray encrypted;
    if (!extractType1(input, &clear, &encrypted, error))
        return fail(error, error && !error->isEmpty() ? *error
            : QStringLiteral("Invalid Type 1 segment stream."));
    const QByteArray namePattern = "/FontName\\s*/([^\\s]+)";
    QRegularExpressionMatch nameMatch = QRegularExpression(QString::fromLatin1(namePattern))
        .match(QString::fromLatin1(clear));
    if (!nameMatch.hasMatch())
        return fail(error, QStringLiteral("Type 1 font has no clear-text FontName."));
    Type1Font parsed;
    parsed.name = nameMatch.captured(1);
    parsed.paintType = int(parseMetric(clear, "/PaintType\\s+([0-9]+)", 0));
    parsed.strokeWidth = parseMetric(clear, "/StrokeWidth\\s+([0-9.]+)", 0);
    if (parsed.paintType < 0 || parsed.paintType > 1 ||
        (parsed.paintType == 1 && parsed.strokeWidth <= 0))
        return fail(error, QStringLiteral("Unsupported Type 1 paint type or stroke width."));
    parsed.ascent = parseMetric(clear, "/FontBBox\\s*\\[\\s*(-?[0-9.]+)", 800);
    const QRegularExpression bbox(QStringLiteral("/FontBBox\\s*\\[\\s*(-?[0-9.]+)\\s+(-?[0-9.]+)\\s+(-?[0-9.]+)\\s+(-?[0-9.]+)"));
    const QRegularExpressionMatch bboxMatch = bbox.match(QString::fromLatin1(clear));
    if (bboxMatch.hasMatch()) {
        parsed.descent = bboxMatch.captured(2).toDouble();
        parsed.ascent = bboxMatch.captured(4).toDouble();
    }
    const QRegularExpression matrix(QStringLiteral("/FontMatrix\\s*\\[\\s*([0-9.eE+-]+)"));
    const QRegularExpressionMatch matrixMatch = matrix.match(QString::fromLatin1(clear));
    if (matrixMatch.hasMatch()) {
        const double scale = matrixMatch.captured(1).toDouble();
        if (qIsFinite(scale) && qAbs(scale) > 1.0e-9)
            parsed.unitsPerEm = 1.0 / qAbs(scale);
    }
    const QByteArray decoded = decrypt(encrypted, 55665);
    if (decoded.size() < 5)
        return fail(error, QStringLiteral("Truncated Type 1 eexec data."));
    const QByteArray privateData = decoded.mid(4);
    int lenIV = 4;
    const QRegularExpression lenIvExpression(QStringLiteral("/lenIV\\s+(-?[0-9]+)"));
    const QRegularExpressionMatch lenIvMatch = lenIvExpression.match(QString::fromLatin1(privateData));
    if (lenIvMatch.hasMatch()) {
        bool ok = false;
        lenIV = lenIvMatch.captured(1).toInt(&ok);
        if (!ok || lenIV < -1 || lenIV > 32)
            return fail(error, QStringLiteral("Invalid Type 1 lenIV value."));
    }
    if (!parseCharStrings(privateData, lenIV, &parsed, error))
        return fail(error, error && !error->isEmpty() ? *error
            : QStringLiteral("PFB CharStrings are invalid or unsupported."));
    *font = parsed;
    return true;
}

} // namespace font
} // namespace peare
