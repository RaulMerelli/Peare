#include "ShpFont.h"

#include <QTextCodec>

#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace peare {
namespace font {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool parseNumber(const QString& token, int* value)
{
    if (!value || token.isEmpty())
        return false;
    bool ok = false;
    int parsed = 0;
    if (token.size() == 3 && token.at(0) == QLatin1Char('0'))
        parsed = token.mid(1).toInt(&ok, 16);
    else
        parsed = token.toInt(&ok, 10);
    if (!ok || parsed < -127 || parsed > 255)
        return false;
    *value = parsed;
    return true;
}

bool parseUnicodeNumber(const QString& token, int* value)
{
    if (!value || token.isEmpty() || token.size() > 5)
        return false;
    bool ok = false;
    const uint parsed = token.toUInt(&ok, 16);
    if (!ok || parsed > 0xFFFFu)
        return false;
    *value = int(parsed);
    return true;
}

QTextCodec* codecForCodePage(quint32 codePage)
{
    const QByteArray number = QByteArray::number(codePage);
    QList<QByteArray> names = {
        QByteArrayLiteral("Windows-") + number,
        QByteArrayLiteral("CP") + number,
        QByteArrayLiteral("IBM ") + number
    };
    if (codePage == 932) names << QByteArrayLiteral("Shift_JIS") << QByteArrayLiteral("SJIS");
    if (codePage == 936) names << QByteArrayLiteral("GBK");
    if (codePage == 949) names << QByteArrayLiteral("EUC-KR");
    if (codePage == 950) names << QByteArrayLiteral("Big5");
    for (const QByteArray& name : names) {
        if (QTextCodec* codec = QTextCodec::codecForName(name)) return codec;
    }
    return nullptr;
}

bool decodePackedCode(QTextCodec* codec, quint16 code, quint32* codepoint)
{
    if (!codec || !codepoint) return false;
    const QByteArray bytes = code <= 0xFF
        ? QByteArray(1, char(code))
        : QByteArray(2, char(code >> 8)) + char(code & 0xFF);
    QTextCodec::ConverterState state;
    const QString decoded = codec->toUnicode(bytes.constData(), bytes.size(), &state);
    const QVector<uint> scalars = decoded.toUcs4();
    if (state.invalidChars != 0 || state.remainingChars != 0 || scalars.size() != 1)
        return false;
    *codepoint = scalars.first();
    return true;
}

bool parseBigFontNumber(const QString& token, int* value)
{
    if (!value || token.isEmpty() || token.size() > 5)
        return false;
    bool ok = false;
    const uint parsed = token.toUInt(&ok, 16);
    if (!ok || parsed > 0xFFFFu)
        return false;
    *value = int(parsed);
    return true;
}

bool validateGlyph(const QVector<quint16>& codes, QVector<quint16>* subshapeReferences,
                   QString* error, bool bigFont = false);

quint16 readLe16(const QByteArray& data, int offset)
{
    return quint16(quint8(data.at(offset))) |
           (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return quint32(readLe16(data, offset)) |
           (quint32(readLe16(data, offset + 2)) << 16);
}

bool parseCompiledInstructions(const QByteArray& bytes, bool unicode, bool bigFont,
                               QVector<quint16>* instructions);

bool parseCompiledFontHeader(const QByteArray& bytes, bool unicode, QString* name,
                             QVector<quint16>* metrics)
{
    if (!name || !metrics) return false;
    const int nameEnd = bytes.indexOf('\0');
    const int expected = unicode ? 6 : 4;
    if (nameEnd < 0 || bytes.size() - nameEnd - 1 != expected) return false;
    *name = QString::fromLatin1(bytes.constData(), nameEnd);
    metrics->clear();
    for (int index = nameEnd + 1; index < bytes.size(); ++index)
        metrics->append(quint8(bytes.at(index)));
    return metrics->last() == 0;
}

bool parseCompiledBigFontHeader(const QByteArray& bytes, QString* name,
                                QVector<quint16>* metrics)
{
    if (!name || !metrics) return false;
    const int nameEnd = bytes.indexOf('\0');
    if (nameEnd < 0) return false;
    *name = QString::fromLatin1(bytes.constData(), nameEnd);
    QByteArray tail = bytes.mid(nameEnd + 1);
    while (tail.size() > 3 && tail.at(0) == '\0') tail.remove(0, 1);
    metrics->clear();
    if (tail.size() == 5 && tail.at(1) == '\0') {
        metrics->append(quint8(tail.at(0)));
        metrics->append(0);
        metrics->append(quint8(tail.at(2)));
    } else if (tail.size() == 4) {
        metrics->append(quint8(tail.at(0)));
        metrics->append(quint8(tail.at(1)));
        metrics->append(quint8(tail.at(2)));
    } else if (tail.size() == 3) {
        metrics->append(quint8(tail.at(0)));
        metrics->append(0);
        metrics->append(quint8(tail.at(1)));
    } else {
        return false;
    }
    return tail.endsWith('\0') && metrics->first() != 0;
}

bool splitCompiledFontGlyph(const QByteArray& bytes, QString* name, QByteArray* program)
{
    if (!name || !program || bytes.isEmpty()) return false;
    name->clear();
    if (bytes.at(0) == '\0') {
        *program = bytes.mid(1);
        return !program->isEmpty();
    }
    const int nameEnd = bytes.indexOf('\0');
    if (nameEnd > 0) {
        const QByteArray candidate = bytes.left(nameEnd);
        const bool isName = std::all_of(candidate.cbegin(), candidate.cend(), [](char value) {
            const quint8 byte = quint8(value);
            return byte >= 0x20 && byte <= 0x7e;
        });
        if (isName) *name = QString::fromLatin1(candidate);
        *program = bytes.mid(nameEnd + 1);
        return !program->isEmpty();
    }
    *program = bytes;
    return true;
}

bool parseCompiledUnifont(const QByteArray& data, ShpFont* font, QString* error)
{
    const quint32 count = readLe32(data, 25);
    const quint16 infoLength = readLe16(data, 29);
    if (count < 2 || count > 65536 || infoLength == 0 ||
        quint64(31) + infoLength > quint64(data.size())) {
        if (error) *error = QStringLiteral("Invalid SHX Unicode font header");
        return false;
    }
    const QByteArray info = data.mid(31, infoLength);
    ShpFont parsed;
    parsed.compiled = true;
    parsed.unicode = true;
    QVector<quint16> headerMetrics;
    if (!parseCompiledFontHeader(info, true, &parsed.name, &headerMetrics)) {
        if (error) *error = QStringLiteral("Invalid SHX Unicode font metrics");
        return false;
    }
    parsed.above = quint8(headerMetrics.at(0));
    parsed.below = quint8(headerMetrics.at(1));
    parsed.modes = quint8(headerMetrics.at(2));
    parsed.encoding = quint8(headerMetrics.at(3));
    parsed.embeddingType = quint8(headerMetrics.at(4));
    if (parsed.above == 0 || parsed.above > 127 || parsed.below > 127 ||
        (parsed.modes != 0 && parsed.modes != 2) || parsed.encoding > 1 ||
        parsed.embeddingType > 2) return false;

    int offset = 31 + infoLength;
    quint64 totalProgramBytes = 0;
    QSet<int> codesSeen;
    QVector<quint16> references;
    for (quint32 index = 1; index < count; ++index) {
        if (offset > data.size() - 4) return false;
        const quint16 code = readLe16(data, offset);
        const quint16 size = readLe16(data, offset + 2);
        offset += 4;
        if (size == 0 || offset > data.size() - size) return false;
        const QByteArray bytes = data.mid(offset, size);
        QString name;
        QByteArray program;
        QVector<quint16> instructions;
        if (!splitCompiledFontGlyph(bytes, &name, &program))
            return fail(error, QStringLiteral("Invalid SHX Unicode glyph name at record %1")
                .arg(index));
        if (!parseCompiledInstructions(program, true, false, &instructions))
            return fail(error, QStringLiteral("Invalid SHX Unicode bytecode at U+%1")
                .arg(code, 4, 16, QLatin1Char('0')));
        if (codesSeen.contains(code))
            return fail(error, QStringLiteral("Duplicate SHX Unicode code U+%1")
                .arg(code, 4, 16, QLatin1Char('0')));
        if (!validateGlyph(instructions, &references, error)) return false;
        totalProgramBytes += quint64(program.size());
        if (totalProgramBytes > 4 * 1024 * 1024) return false;
        codesSeen.insert(code);
        ShpGlyph glyph;
        glyph.code = code;
        glyph.name = name;
        glyph.instructions = instructions;
        parsed.glyphs.append(glyph);
        offset += size;
    }
    const bool hasTrailingMarker = data.endsWith(QByteArray("\r\n\x1a", 3)) ||
                                  data.endsWith(QByteArrayLiteral("EOF"));
    if (offset != data.size() - (hasTrailingMarker ? 3 : 0) || parsed.glyphs.isEmpty())
        return false;
    for (quint16 code : references)
        if (!codesSeen.contains(code)) return false;
    *font = parsed;
    return true;
}

bool shxKind(const QByteArray& data, bool* unicode, bool* bigFont = nullptr)
{
    if (data.size() < 30 || !unicode) return false;
    const int marker = data.indexOf("\r\n\x1a");
    if (marker < 0 || marker >= 28) return false;
    const QByteArray header = data.left(marker);
    if (bigFont) *bigFont = false;
    if (header == QByteArrayLiteral("AutoCAD-86 bigfont 1.0")) {
        *unicode = false;
        if (bigFont) *bigFont = true;
        return true;
    }
    if (header == QByteArrayLiteral("AutoCAD-86 shapes 1.0") ||
        header == QByteArrayLiteral("AutoCAD-86 shapes 1.1")) {
        *unicode = false;
        return true;
    }
    if (header == QByteArrayLiteral("AutoCAD-86 unifont 1.0")) {
        *unicode = true;
        return true;
    }
    return false;
}

bool parseCompiledInstructions(const QByteArray& bytes, bool unicode, bool bigFont,
                               QVector<quint16>* instructions)
{
    if (!instructions || bytes.isEmpty() || bytes.size() > 16384 || bytes.back() != 0)
        return false;
    QVector<quint16> parsed;
    int offset = 0;
    while (offset < bytes.size()) {
        const quint8 code = quint8(bytes.at(offset++));
        parsed.append(code);
        if (code == 0) {
            if (offset == bytes.size()) {
                *instructions = parsed;
                return true;
            }
            if (!bigFont) return false;
            continue;
        }
        if (code >= 0x10) continue;
        int operands = 0;
        switch (code) {
        case 1: case 2: case 5: case 6: break;
        case 3: case 4: operands = 1; break;
        case 14: break;
        case 7:
            if (bigFont && offset < bytes.size() && quint8(bytes.at(offset)) == 0) {
                if (offset + 7 > bytes.size()) return false;
                parsed.append(0);
                parsed.append(quint16(quint8(bytes.at(offset + 1))) |
                              (quint16(quint8(bytes.at(offset + 2))) << 8));
                for (int index = 3; index < 7; ++index)
                    parsed.append(quint8(bytes.at(offset + index)));
                offset += 7;
                continue;
            }
            operands = unicode ? 2 : 1;
            if (offset + operands > bytes.size()) return false;
            if (unicode) {
                parsed.append((quint16(quint8(bytes.at(offset))) << 8) |
                              quint8(bytes.at(offset + 1)));
                offset += 2;
                continue;
            }
            break;
        case 8: operands = 2; break;
        case 9: {
            bool terminated = false;
            while (offset + 1 < bytes.size()) {
                const quint8 dx = quint8(bytes.at(offset++));
                const quint8 dy = quint8(bytes.at(offset++));
                parsed.append(dx);
                parsed.append(dy);
                if (dx == 0 && dy == 0) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) return false;
            continue;
        }
        case 10: operands = 2; break;
        case 11: operands = 5; break;
        case 12: operands = 3; break;
        case 13: {
            bool terminated = false;
            while (offset + 1 < bytes.size()) {
                const quint8 dx = quint8(bytes.at(offset++));
                const quint8 dy = quint8(bytes.at(offset++));
                parsed.append(dx);
                parsed.append(dy);
                if (dx == 0 && dy == 0) {
                    terminated = true;
                    break;
                }
                if (offset >= bytes.size()) return false;
                parsed.append(quint8(bytes.at(offset++)));
            }
            if (!terminated) return false;
            continue;
        }
        default: return false;
        }
        if (offset + operands > bytes.size()) return false;
        for (int index = 0; index < operands; ++index)
            parsed.append(quint8(bytes.at(offset++)));
    }
    return false;
}

bool parseCompiledShx(const QByteArray& data, ShpFont* font, QString* error)
{
    bool unicode = false;
    bool bigFont = false;
    if (!font || data.size() < 32 || data.size() > 16 * 1024 * 1024 ||
        !shxKind(data, &unicode, &bigFont) || bigFont)
        return false;
    if (unicode) return parseCompiledUnifont(data, font, error);
    const bool hasTrailingMarker = data.endsWith(QByteArray("\r\n\x1a", 3)) ||
                                  data.endsWith(QByteArrayLiteral("EOF"));
    const int trailer = data.size() - (hasTrailingMarker ? 3 : 0);
    const int signatureEnd = data.indexOf("\r\n\x1a") + 3;
    const bool shapes = data.startsWith(QByteArrayLiteral("AutoCAD-86 shapes 1."));
    int directoryOffset = signatureEnd;
    quint16 count = 0;
    if (shapes) {
        if (signatureEnd > trailer - 6) return false;
        const quint16 firstShape = readLe16(data, signatureEnd);
        const quint16 lastShape = readLe16(data, signatureEnd + 2);
        count = readLe16(data, signatureEnd + 4);
        if (firstShape > lastShape || count == 0 || count > lastShape - firstShape + 1) {
            if (error) *error = QStringLiteral("Invalid SHX Shapes 1.0 index header");
            return false;
        }
        directoryOffset += 6;
    } else {
        count = readLe16(data, signatureEnd);
        directoryOffset += 2;
    }
    if (count == 0) {
        if (error) *error = QStringLiteral("Invalid SHX shape count");
        return false;
    }
    struct ShapeRecord {
        quint16 code;
        QString name;
        QByteArray bytes;
        QVector<quint16> instructions;
    };
    QVector<ShapeRecord> records;
    records.reserve(count);
    int offset = directoryOffset;
    quint64 totalProgramBytes = 0;
    QVector<QPair<quint16, quint16>> directory;
    if (!unicode) {
        const int directoryEnd = directoryOffset + int(count) * 4;
        if (directoryEnd > trailer) return false;
        directory.reserve(count);
        for (quint16 index = 0; index < count; ++index) {
            directory.append(qMakePair(readLe16(data, offset), readLe16(data, offset + 2)));
            offset += 4;
        }
    }
    for (quint16 index = 0; index < count; ++index) {
        quint16 code = 0;
        quint16 size = 0;
        code = directory.at(index).first;
        size = directory.at(index).second;
        if (size < 2 || offset > trailer - size) return false;
        const int end = offset + size;
        const QByteArray bytes = data.mid(offset, size);
        totalProgramBytes += quint64(bytes.size());
        if (totalProgramBytes > 4 * 1024 * 1024) return false;
        ShapeRecord record;
        record.code = code;
        record.bytes = bytes;
        records.append(record);
        offset = end;
    }
    if (offset != trailer) return false;
    int headerIndex = -1;
    for (int index = 0; index < records.size(); ++index) {
        if (records.at(index).code == 0) {
            if (headerIndex >= 0) return false;
            headerIndex = index;
        }
    }
    if (headerIndex < 0) {
        if (error) *error = QStringLiteral("SHX file is a shape library, not a font");
        return false;
    }
    const ShapeRecord header = records.takeAt(headerIndex);
    ShpFont parsed;
    QVector<quint16> headerMetrics;
    if (!parseCompiledFontHeader(header.bytes, false, &parsed.name, &headerMetrics)) {
        if (error) *error = QStringLiteral("Invalid SHX font header shape");
        return false;
    }
    parsed.compiled = true;
    parsed.above = quint8(headerMetrics.at(0));
    parsed.below = quint8(headerMetrics.at(1));
    parsed.modes = quint8(headerMetrics.at(2));
    if (parsed.above == 0 || parsed.above > 127 || parsed.below > 127 ||
        (parsed.modes != 0 && parsed.modes != 2)) return false;
    QSet<int> codesSeen;
    QVector<quint16> subshapeReferences;
    for (ShapeRecord& record : records) {
        QByteArray program;
        if (!splitCompiledFontGlyph(record.bytes, &record.name, &program) ||
            !parseCompiledInstructions(program, false, false, &record.instructions)) {
            if (error) *error = QStringLiteral("Invalid SHX shape instruction stream");
            return false;
        }
        if (codesSeen.contains(record.code) ||
            !validateGlyph(record.instructions, &subshapeReferences, error)) return false;
        codesSeen.insert(record.code);
        ShpGlyph glyph;
        glyph.code = record.code;
        glyph.name = record.name;
        glyph.instructions = record.instructions;
        parsed.glyphs.append(glyph);
    }
    for (quint16 code : subshapeReferences) {
        if (!codesSeen.contains(code)) return false;
    }
    if (parsed.glyphs.isEmpty()) return false;
    *font = parsed;
    return true;
}

bool parseCompiledBigFont(const QByteArray& data, ShpFont* font, QString* error)
{
    bool unicode = false;
    bool bigFont = false;
    if (!font || data.size() > 16 * 1024 * 1024 ||
        !shxKind(data, &unicode, &bigFont) || !bigFont)
        return false;
    const int signatureEnd = data.indexOf("\r\n\x1a") + 3;
    if (signatureEnd < 3 || signatureEnd > data.size() - 6)
        return false;
    const quint16 entrySize = readLe16(data, signatureEnd);
    const quint16 recordCount = readLe16(data, signatureEnd + 2);
    const quint16 rangeCount = readLe16(data, signatureEnd + 4);
    if ((entrySize != 0 && entrySize != 8) || recordCount == 0 || rangeCount > 16)
        return false;
    const quint64 rangesEnd = quint64(signatureEnd) + 6 + quint64(rangeCount) * 4;
    const quint64 indexEnd = rangesEnd + quint64(recordCount) * 8;
    if (indexEnd > quint64(data.size()))
        return false;
    int previousRangeEnd = -1;
    for (quint16 index = 0; index < rangeCount; ++index) {
        const int offset = signatureEnd + 6 + index * 4;
        const quint16 begin = readLe16(data, offset);
        const quint16 end = readLe16(data, offset + 2);
        if (begin > end || begin <= previousRangeEnd) {
            if (error) *error = QStringLiteral("Invalid compiled SHX BigFont ranges");
            return false;
        }
        previousRangeEnd = end;
    }
    struct Record {
        quint16 code;
        QByteArray bytes;
        QString name;
        QVector<quint16> instructions;
    };
    QVector<Record> records;
    QVector<QPair<quint32, quint32>> spans;
    QSet<quint16> codesSeen;
    records.reserve(recordCount);
    spans.reserve(recordCount);
    for (quint16 index = 0; index < recordCount; ++index) {
        const int entry = int(rangesEnd) + index * 8;
        const quint16 code = readLe16(data, entry);
        const quint16 byteCount = readLe16(data, entry + 2);
        const quint32 bodyOffset = quint32(quint8(data.at(entry + 4))) |
            (quint32(quint8(data.at(entry + 5))) << 8) |
            (quint32(quint8(data.at(entry + 6))) << 16) |
            (quint32(quint8(data.at(entry + 7))) << 24);
        if (code == 0 && byteCount == 0 && bodyOffset == 0)
            continue;
        const quint64 bodyEnd = quint64(bodyOffset) + byteCount;
        if (codesSeen.contains(code) || byteCount < 2 || bodyOffset < indexEnd ||
            bodyEnd > quint64(data.size())) {
            if (error) *error = QStringLiteral("Invalid compiled SHX BigFont index entry");
            return false;
        }
        records.append({code, data.mid(int(bodyOffset), byteCount), {}, {}});
        spans.append(qMakePair(bodyOffset, quint32(bodyEnd)));
        codesSeen.insert(code);
    }
    std::sort(spans.begin(), spans.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    for (int index = 1; index < spans.size(); ++index) {
        if (spans.at(index).first < spans.at(index - 1).second) {
            if (error) *error = QStringLiteral("Overlapping compiled SHX BigFont records");
            return false;
        }
    }
    int headerIndex = -1;
    for (int index = 0; index < records.size(); ++index) {
        if (records.at(index).code == 0) {
            if (headerIndex >= 0) return false;
            headerIndex = index;
        }
    }
    if (headerIndex < 0) {
        if (error) *error = QStringLiteral("Invalid compiled SHX BigFont font header");
        return false;
    }
    const Record header = records.takeAt(headerIndex);
    ShpFont parsed;
    parsed.bigFont = true;
    parsed.compiled = true;
    QVector<quint16> headerMetrics;
    if (!parseCompiledBigFontHeader(header.bytes, &parsed.name, &headerMetrics)) {
        if (error) *error = QStringLiteral("Invalid compiled SHX BigFont font metrics");
        return false;
    }
    parsed.above = quint8(headerMetrics.at(0));
    parsed.below = quint8(headerMetrics.at(1));
    parsed.modes = quint8(headerMetrics.at(2));
    if (parsed.above == 0)
        return false;
    QVector<quint16> references;
    for (Record& record : records) {
        QByteArray program;
        if (!splitCompiledFontGlyph(record.bytes, &record.name, &program) ||
            !parseCompiledInstructions(program, false, true, &record.instructions) ||
            !validateGlyph(record.instructions, &references, error, true))
            return false;
        ShpGlyph glyph;
        glyph.code = record.code;
        glyph.name = record.name;
        glyph.instructions = record.instructions;
        parsed.glyphs.append(glyph);
    }
    if (parsed.glyphs.isEmpty()) return false;
    *font = parsed;
    return true;
}

QStringList splitValues(const QString& line)
{
    QStringList values;
    QString token;
    for (QChar character : line) {
        if (character == QLatin1Char(',') || character == QLatin1Char('(') ||
            character == QLatin1Char(')') || character.isSpace()) {
            if (!token.isEmpty()) {
                values.append(token);
                token.clear();
            }
        } else {
            token.append(character);
        }
    }
    if (!token.isEmpty())
        values.append(token);
    return values;
}

bool validateGlyph(const QVector<quint16>& codes, QVector<quint16>* subshapeReferences,
                   QString* error, bool bigFont)
{
    if (codes.isEmpty() || codes.last() != 0) {
        if (error) *error = QStringLiteral("SHP shape has no end code");
        return false;
    }
    int stackDepth = 0;
    for (int i = 0; i < codes.size();) {
        const quint16 code = codes.at(i++);
        if (code == 0) {
            if (i == codes.size()) return true;
            if (!bigFont) {
                if (error) *error = QStringLiteral("SHP shape has trailing data");
                return false;
            }
            continue;
        }
        if (code >= 0x10)
            continue;
        int operands = 0;
        switch (code) {
        case 1: case 2: break;
        case 3: case 4: operands = 1; break;
        case 5:
            if (++stackDepth > 4) {
                if (error) *error = QStringLiteral("SHP position stack exceeds four entries");
                return false;
            }
            break;
        case 6:
            if (stackDepth > 0) --stackDepth;
            break;
        case 7:
            if (bigFont && i < codes.size() && codes.at(i) == 0) {
                if (i + 6 > codes.size() || codes.at(i + 4) == 0 ||
                    codes.at(i + 5) == 0 || codes.at(i + 4) > 255 ||
                    codes.at(i + 5) > 255) {
                    if (error) *error = QStringLiteral("Invalid extended SHP BigFont call");
                    return false;
                }
                if (subshapeReferences) subshapeReferences->append(codes.at(i + 1));
                i += 6;
                continue;
            }
            operands = 1;
            if (i < codes.size() && subshapeReferences)
                subshapeReferences->append(codes.at(i));
            break;
        case 8: operands = 2; break;
        case 9: {
            bool terminated = false;
            while (i + 1 < codes.size()) {
                const qint8 dx = qint8(codes.at(i++));
                const qint8 dy = qint8(codes.at(i++));
                if (dx == 0 && dy == 0) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                if (error) *error = QStringLiteral("Unterminated SHP displacement list");
                return false;
            }
            continue;
        }
        case 10: operands = 2; break;
        case 11: operands = 5; break;
        case 12: operands = 3; break;
        case 13: {
            bool terminated = false;
            while (i + 1 < codes.size()) {
                const qint8 dx = qint8(codes.at(i++));
                const qint8 dy = qint8(codes.at(i++));
                if (dx == 0 && dy == 0) {
                    terminated = true;
                    break;
                }
                if (dx == -128 || dy == -128 || i >= codes.size() ||
                    qint8(codes.at(i)) == -128) {
                    if (error) *error = QStringLiteral("Truncated SHP polyarc segment");
                    return false;
                }
                ++i;
            }
            if (!terminated) {
                if (error) *error = QStringLiteral("Unterminated SHP polyarc");
                return false;
            }
            continue;
        }
        case 14:
            if (i >= codes.size() || codes.at(i) == 0) {
                if (error) *error = QStringLiteral("Truncated SHP orientation code");
                return false;
            }
            continue;
        default:
            if (error) *error = QStringLiteral("Unsupported SHP shape instruction");
            return false;
        }
        if (i + operands > codes.size()) {
            if (error) *error = QStringLiteral("Truncated SHP shape instruction");
            return false;
        }
        if ((code == 3 || code == 4) && (codes.at(i) == 0 || codes.at(i) > 127)) {
            if (error) *error = QStringLiteral("Invalid SHP scale factor");
            return false;
        }
        if (code == 10 || code == 11) {
            const int specIndex = i + (code == 10 ? 1 : 4);
            const int spec = qint8(codes.at(specIndex));
            const int packed = qAbs(spec);
            if (packed > 127 || codes.at(i + (code == 10 ? 0 : 3)) == 0 ||
                (code == 11 && quint8(codes.at(i + 2)) * 256u +
                                   quint8(codes.at(i + 3)) == 0)) {
                if (error) *error = QStringLiteral("Invalid SHP arc parameters");
                return false;
            }
        } else if (code == 12) {
            if ((codes.at(i) == 0 && codes.at(i + 1) == 0) ||
                qint8(codes.at(i)) == -128 || qint8(codes.at(i + 1)) == -128 ||
                qint8(codes.at(i + 2)) == -128) {
                if (error) *error = QStringLiteral("Invalid SHP bulge arc");
                return false;
            }
        }
        i += operands;
    }
    if (error) *error = QStringLiteral("SHP shape has no end code");
    return false;
}

bool parseDefinitions(const QByteArray& data, ShpFont* font, QString* error)
{
    if (!font || data.isEmpty() || data.size() > 16 * 1024 * 1024)
        return false;
    const QString source = QString::fromLatin1(data.constData(), data.size());
    const QStringList lines = source.split(QLatin1Char('\n'));
    ShpFont parsed;
    QSet<int> codesSeen;
    QVector<quint16> subshapeReferences;
    ShpGlyph current;
    int expected = -1;
    int encodedBytes = 0;
    quint64 totalInstructions = 0;
    bool active = false;
    bool currentIsHeader = false;
    bool hasHeader = false;
    bool expectsUnicodeSubshape = false;
    int unicodeOperandsRemaining = 0;
    int unicodeListMode = 0;
    int unicodeListState = 0;
    int unicodeListDx = 0;
    int unicodeListDy = 0;
    int bigFontCharacterCount = -1;
    int bigFontOperandState = 0;
    int bigFontTransformFields = 0;
    auto finish = [&]() -> bool {
        if (!active)
            return true;
        if (encodedBytes != expected) {
            if (error) *error = QStringLiteral("SHP definition byte count mismatch");
            return false;
        }
        totalInstructions += quint64(encodedBytes);
        if (totalInstructions > 4 * 1024 * 1024 || codesSeen.size() >= 65536) {
            if (error) *error = QStringLiteral("SHP font exceeds parser limits");
            return false;
        }
        if (currentIsHeader) {
            const int expectedHeaderBytes = parsed.unicode ? 6 : parsed.bigFont ? 5 : 4;
            if (hasHeader || current.instructions.size() != expectedHeaderBytes ||
                current.instructions.last() != 0) {
                if (error) *error = QStringLiteral("Invalid SHP font header shape");
                return false;
            }
            parsed.above = quint8(current.instructions.at(0));
            parsed.below = quint8(current.instructions.at(1));
            parsed.modes = quint8(current.instructions.at(2));
            if (parsed.above == 0 || parsed.above > 127 || parsed.below > 127 ||
                (parsed.modes != 0 && parsed.modes != 2)) {
                if (error) *error = QStringLiteral("Invalid SHP font metrics");
                return false;
            }
            if (parsed.unicode) {
                parsed.encoding = quint8(current.instructions.at(3));
                parsed.embeddingType = quint8(current.instructions.at(4));
                if (parsed.encoding > 1 || parsed.embeddingType > 2) {
                    if (error) *error = QStringLiteral("Unsupported SHP Unicode font encoding");
                    return false;
                }
            }
            parsed.name = current.name;
            hasHeader = true;
        } else {
            if (codesSeen.contains(current.code) ||
                !validateGlyph(current.instructions, &subshapeReferences, error,
                               parsed.bigFont))
                return false;
            codesSeen.insert(current.code);
            parsed.glyphs.append(current);
        }
        active = false;
        return true;
    };

    for (QString line : lines) {
        const int comment = line.indexOf(QLatin1Char(';'));
        if (comment >= 0)
            line.truncate(comment);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('*'))) {
            if (!finish())
                return false;
            if (line.startsWith(QStringLiteral("*BIGFONT"), Qt::CaseInsensitive)) {
                if (parsed.bigFont || parsed.unicode || hasHeader || !codesSeen.isEmpty()) {
                    if (error) *error = QStringLiteral("BigFont header is not at the start of the file");
                    return false;
                }
                const QStringList fields = splitValues(line.mid(8).trimmed());
                bool countOk = false;
                bool rangesOk = false;
                const int characterCount = fields.size() >= 2 ? fields.at(0).toInt(&countOk) : 0;
                const int rangeCount = fields.size() >= 2 ? fields.at(1).toInt(&rangesOk) : 0;
                if (!countOk || !rangesOk || characterCount < 1 || rangeCount < 0 ||
                    rangeCount > 16 || fields.size() != 2 + rangeCount * 2) {
                    if (error) *error = QStringLiteral("Invalid SHP BigFont escape ranges");
                    return false;
                }
                bigFontCharacterCount = characterCount;
                int previousEnd = -1;
                for (int index = 0; index < rangeCount; ++index) {
                    int begin = 0, end = 0;
                    if (!parseNumber(fields.at(2 + index * 2), &begin) ||
                        !parseNumber(fields.at(3 + index * 2), &end) ||
                        begin > end || begin <= previousEnd) {
                        if (error) *error = QStringLiteral("Invalid SHP BigFont escape range");
                        return false;
                    }
                    previousEnd = end;
                }
                parsed.bigFont = true;
                continue;
            }
            const QStringList header = line.mid(1).split(QLatin1Char(','));
            int shapeCode = -1;
            bool byteCountOk = false;
            const int byteCount = header.size() >= 2
                ? header.at(1).trimmed().toInt(&byteCountOk, 10) : -1;
            const bool unicodeHeader = !header.isEmpty() &&
                header.at(0).trimmed().compare(QStringLiteral("UNIFONT"),
                                                Qt::CaseInsensitive) == 0;
            const bool parsedShape = header.size() >= 2 &&
                (unicodeHeader || (parsed.unicode
                    ? (parsed.encoding == 1
                        ? parseBigFontNumber(header.at(0).trimmed(), &shapeCode)
                        : parseUnicodeNumber(header.at(0).trimmed(), &shapeCode))
                    : parsed.bigFont
                    ? parseBigFontNumber(header.at(0).trimmed(), &shapeCode)
                    : parseNumber(header.at(0).trimmed(), &shapeCode))) &&
                byteCountOk;
            if (!parsedShape || (!unicodeHeader && (shapeCode < 0 || shapeCode > 65535)) ||
                byteCount < 1 || byteCount > 16384 ||
                (unicodeHeader && (hasHeader || !codesSeen.isEmpty()))) {
                if (error) *error = QStringLiteral("Invalid SHP definition header");
                return false;
            }
            if (unicodeHeader) {
                if (parsed.bigFont) {
                    if (error) *error = QStringLiteral("SHP BigFont cannot use a Unicode header");
                    return false;
                }
                parsed.unicode = true;
                shapeCode = 0;
            }
            current = ShpGlyph();
            current.code = quint16(shapeCode);
            current.name = header.size() > 2 ? header.at(2).trimmed() : QString();
            expected = byteCount;
            encodedBytes = 0;
            expectsUnicodeSubshape = false;
            unicodeOperandsRemaining = 0;
            unicodeListMode = 0;
            unicodeListState = 0;
            bigFontOperandState = 0;
            bigFontTransformFields = 0;
            currentIsHeader = unicodeHeader || (!parsed.unicode && shapeCode == 0);
            active = true;
            continue;
        }
        if (!active) {
            if (error) *error = QStringLiteral("SHP data appears outside a definition");
            return false;
        }
        for (const QString& token : splitValues(line)) {
            int value = 0;
            const bool unicodeSubshapeOperand = parsed.unicode && expectsUnicodeSubshape;
            bool twoByteValue = parsed.unicode && expectsUnicodeSubshape;
            if (parsed.bigFont)
                twoByteValue = bigFontOperandState == 2 ||
                    (bigFontOperandState == 1 && token != QStringLiteral("0"));
            const bool validValue = twoByteValue
                ? (parsed.bigFont || parsed.encoding == 1
                    ? parseBigFontNumber(token, &value)
                    : parseUnicodeNumber(token, &value))
                : parseNumber(token, &value);
            const int valueBytes = twoByteValue ? 2 : 1;
            if (!validValue || encodedBytes + valueBytes > expected) {
                if (error) *error = QStringLiteral("Invalid SHP instruction data");
                return false;
            }
            current.instructions.append(quint16(value));
            encodedBytes += valueBytes;
            if (parsed.unicode && !parsed.bigFont) {
                if (unicodeSubshapeOperand) {
                    expectsUnicodeSubshape = false;
                } else if (unicodeOperandsRemaining > 0) {
                    --unicodeOperandsRemaining;
                } else if (unicodeListMode != 0) {
                    if (unicodeListMode == 13 && unicodeListState == 2) {
                        unicodeListState = 0;
                    } else if (unicodeListState == 0) {
                        unicodeListDx = value;
                        unicodeListState = 1;
                    } else {
                        unicodeListDy = value;
                        if (unicodeListDx == 0 && unicodeListDy == 0) {
                            unicodeListMode = 0;
                            unicodeListState = 0;
                        } else if (unicodeListMode == 13) {
                            unicodeListState = 2;
                        } else {
                            unicodeListState = 0;
                        }
                    }
                } else if (value == 7) {
                    expectsUnicodeSubshape = true;
                } else if (value == 3 || value == 4) {
                    unicodeOperandsRemaining = 1;
                } else if (value == 8 || value == 10) {
                    unicodeOperandsRemaining = 2;
                } else if (value == 11) {
                    unicodeOperandsRemaining = 5;
                } else if (value == 12) {
                    unicodeOperandsRemaining = 3;
                } else if (value == 9 || value == 13) {
                    unicodeListMode = value;
                    unicodeListState = 0;
                }
            }
            if (parsed.bigFont) {
                if (bigFontOperandState == 1) {
                    bigFontOperandState = value == 0 ? 2 : 0;
                } else if (bigFontOperandState == 2) {
                    bigFontOperandState = 3;
                    bigFontTransformFields = 0;
                } else if (bigFontOperandState == 3) {
                    if (++bigFontTransformFields == 4) bigFontOperandState = 0;
                } else if (value == 7) {
                    bigFontOperandState = 1;
                }
            }
        }
    }
    if (!finish() || !hasHeader)
        return false;
    if (parsed.glyphs.isEmpty()) {
        if (error) *error = QStringLiteral("SHP font contains no glyphs");
        return false;
    }
    if (parsed.bigFont && parsed.glyphs.size() != bigFontCharacterCount) {
        if (error) *error = QStringLiteral("SHP BigFont character count mismatch");
        return false;
    }
    for (quint16 code : subshapeReferences) {
        if (!codesSeen.contains(code)) {
            if (error) *error = QStringLiteral("SHP font references a missing subshape");
            return false;
        }
    }
    *font = parsed;
    return true;
}

} // namespace

bool hasShpFontHeader(const QByteArray& data)
{
    bool unicode = false;
    bool bigFont = false;
    if (shxKind(data, &unicode, &bigFont)) {
        ShpFont parsed;
        QString error;
        return bigFont ? parseCompiledBigFont(data, &parsed, &error)
                       : parseCompiledShx(data, &parsed, &error);
    }
    ShpFont font;
    return parseDefinitions(data, &font, nullptr);
}

bool parseShpFont(const QByteArray& data, ShpFont* font, QString* error)
{
    bool unicode = false;
    bool bigFont = false;
    if (shxKind(data, &unicode, &bigFont)) {
        if (bigFont) {
            if (parseCompiledBigFont(data, font, error)) return true;
            if (error && error->isEmpty())
                *error = QStringLiteral("Invalid or unsupported compiled SHX BigFont");
            return false;
        }
        if (parseCompiledShx(data, font, error)) return true;
        if (error && error->isEmpty())
            *error = QStringLiteral("Invalid or unsupported compiled SHX font");
        return false;
    }
    if (!parseDefinitions(data, font, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Invalid or unsupported SHP font");
        return false;
    }
    return true;
}

bool mapPackedShpUnicode(ShpFont* font, quint32 codePage, QString* error)
{
    if (!font || !font->unicode || font->encoding != 1 || codePage == 0) {
        if (error) *error = QStringLiteral("Packed SHP Unicode mapping needs a code page.");
        return false;
    }
    QTextCodec* codec = codecForCodePage(codePage);
    if (!codec) {
        if (error) *error = QStringLiteral("Unsupported packed SHP code page.");
        return false;
    }
    QHash<quint32, quint16> mapping;
    for (const ShpGlyph& glyph : font->glyphs) {
        quint32 codepoint = 0;
        if (!decodePackedCode(codec, glyph.code, &codepoint)) {
            if (error) *error = QStringLiteral("Invalid packed SHP character for code page %1.")
                                     .arg(codePage);
            return false;
        }
        if (mapping.contains(codepoint) && mapping.value(codepoint) != glyph.code) {
            if (error) *error = QStringLiteral("Packed SHP codes map to a duplicate Unicode character.");
            return false;
        }
        mapping.insert(codepoint, glyph.code);
    }
    font->glyphByUnicode = mapping;
    if (error) error->clear();
    return true;
}

} // namespace font
} // namespace peare
