#include "RT_STRING.h"

#include <QTextCodec>

#include <memory>
#include <stdexcept>

namespace peare {
namespace resources {
namespace {

QTextCodec* tryCodecForCodePage(int codepage)
{
    if (codepage == 0)
        codepage = 850;
    if (codepage == 1200)
        return QTextCodec::codecForName("UTF-16LE");
    if (codepage == 1201)
        return QTextCodec::codecForName("UTF-16BE");
    if (codepage == 65001)
        return QTextCodec::codecForName("UTF-8");
    if (codepage == 20127)
        return QTextCodec::codecForName("US-ASCII");
    if (codepage == 850)
        return QTextCodec::codecForName("IBM 850");

    const QList<QByteArray> names = {
        QByteArrayLiteral("Windows-") + QByteArray::number(codepage),
        QByteArrayLiteral("IBM ") + QByteArray::number(codepage),
        QByteArrayLiteral("IBM") + QByteArray::number(codepage),
        QByteArrayLiteral("CP") + QByteArray::number(codepage)
    };

    for (const QByteArray& name : names) {
        if (QTextCodec* codec = QTextCodec::codecForName(name))
            return codec;
    }

    return nullptr;
}


QString decodeCp437(const char* bytes, int length)
{
    static const ushort table[256] = {
        0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
        0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
        0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
        0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
        0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
        0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
        0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
        0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
        0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
        0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
        0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
        0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
        0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
        0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
        0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
        0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
        0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
        0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
        0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
        0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
    };

    QString result;
    result.reserve(length);
    for (int i = 0; i < length; ++i)
        result.append(QChar(table[quint8(bytes[i])]));
    return result;
}

QTextCodec* codecForCodePage(int codepage)
{
    if (QTextCodec* codec = tryCodecForCodePage(codepage))
        return codec;
    throw std::runtime_error("Unsupported code page");
}

} // namespace

quint8 RT_STRING::ReadByte(const QByteArray& data, qsizetype& offset)
{
    if (offset + 1 > data.size())
        throw std::runtime_error("Attempted to read past end of data.");

    return static_cast<quint8>(data.at(offset++));
}

quint16 RT_STRING::ReadUInt16(const QByteArray& data, qsizetype& offset)
{
    if (offset + 2 > data.size())
        throw std::runtime_error("Attempted to read past end of data for ushort.");

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    const quint16 value = quint16(bytes[0]) | (quint16(bytes[1]) << 8);
    offset += 2;
    return value;
}

QString RT_STRING::ReadLenString(const QByteArray& data,
                                 qsizetype& offset,
                                 int codepage,
                                 int len)
{
    if (len < 0)
        throw std::runtime_error("Invalid character count.");

    if (codepage == 437) {
        if (offset + len > data.size())
            throw std::runtime_error("Not enough data to decode the requested number of characters.");
        QString result = decodeCp437(data.constData() + offset, len);
        offset += len;
        return result.replace(QChar(u'\0'), QChar(u' '));
    }

    QTextCodec* encoding = codecForCodePage(codepage);

    // All historical Windows/OS2 resource code pages used here are
    // single-byte, while PE string tables are UTF-16LE. Handle those two
    // cases directly: this is equivalent to Decoder.Convert in the C# code
    // and avoids QTextDecoder buffering one input byte at a time.
    if (codepage == 1200 || codepage == 1201) {
        const qsizetype byteCount = qsizetype(len) * 2;
        if (offset + byteCount > data.size())
            throw std::runtime_error("Not enough data to decode the requested number of characters.");
        const QString result = encoding->toUnicode(data.constData() + offset, int(byteCount));
        offset += byteCount;
        if (result.size() != len)
            throw std::runtime_error("Not enough data to decode the requested number of characters.");
        QString copy = result;
        return copy.replace(QChar(u'\0'), QChar(u' '));
    }

    if (codepage != 65001) {
        if (offset + len > data.size())
            throw std::runtime_error("Not enough data to decode the requested number of characters.");
        QString result = encoding->toUnicode(data.constData() + offset, len);
        offset += len;
        if (result.size() != len)
            throw std::runtime_error("Not enough data to decode the requested number of characters.");
        return result.replace(QChar(u'\0'), QChar(u' '));
    }

    std::unique_ptr<QTextDecoder> decoder(encoding->makeDecoder());
    QString chars;
    chars.reserve(len);
    while (offset < data.size() && chars.size() < len) {
        chars += decoder->toUnicode(data.constData() + offset, 1);
        ++offset;
    }
    if (chars.size() != len)
        throw std::runtime_error("Not enough data to decode the requested number of characters.");
    return chars.replace(QChar(u'\0'), QChar(u' '));
}

QString RT_STRING::ReadNullTerminatedString(const QByteArray& data,
                                            qsizetype& offset,
                                            int codepage)
{
    if (codepage == 437) {
        const qsizetype start = offset;
        while (offset < data.size() && data.at(offset) != '\0')
            ++offset;
        if (offset >= data.size())
            throw std::runtime_error("Null-terminated string not found in the provided data.");
        const QString result = decodeCp437(data.constData() + start, int(offset - start));
        ++offset; // consume the terminator
        return result;
    }

    QTextCodec* encoding = codecForCodePage(codepage);

    // Determine the null terminator for the given encoding.
    // For most single-byte encodings, this is 0x00.
    // For UTF-16, it's 0x00 0x00.
    // This is a simplification; a robust solution might need a more complex way to get the null terminator.
    const QByteArray nullTerminatorBytes =
        (codepage == 1200 || codepage == 1201) ? QByteArray(2, '\0') : QByteArray(1, '\0');

    QByteArray stringBytes;
    bool foundNull = false;

    while (offset < data.size()) {
        // Check if the current byte(s) match the null terminator sequence
        bool possibleNull = true;
        if (offset + nullTerminatorBytes.size() <= data.size()) {
            for (qsizetype i = 0; i < nullTerminatorBytes.size(); ++i) {
                if (data.at(offset + i) != nullTerminatorBytes.at(i)) {
                    possibleNull = false;
                    break;
                }
            }
        } else {
            possibleNull = false; // Not enough bytes left to be the null terminator
        }

        if (possibleNull) {
            foundNull = true;
            offset += nullTerminatorBytes.size(); // Consume the null terminator
            break;
        }

        // If not null, write the current byte to the byte array
        stringBytes.append(data.at(offset));
        ++offset;
    }

    if (!foundNull) {
        // If we reached the end of the data without finding a null terminator,
        // you might want to throw an exception or handle it differently
        // depending on your expected behavior (e.g., return the string read so far).
        throw std::runtime_error("Null-terminated string not found in the provided data.");
    }

    // Convert the bytes read (excluding the null terminator) into a string
    return encoding->toUnicode(stringBytes);
}

QString RT_STRING::Get(const QByteArray& data,
                       ModuleFormat headerType,
                       bool isOs2,
                       int baseId)
{
    if (data.isEmpty())
        return {};

    struct ParseResult {
        QString text;
        QString error;
        int strings = 0;
        qsizetype consumed = 0;
        bool complete = false;
    };

    const auto parseTable = [&](qsizetype initialOffset, int codePage) -> ParseResult {
        ParseResult parsed;
        QString body;
        qsizetype offset = initialOffset;
        int currentId = -1;

        while (offset < data.size()) {
            try {
                const int length = headerType == ModuleFormat::PE
                    ? int(ReadUInt16(data, offset))
                    : int(ReadByte(data, offset));
                if (length == 0)
                    continue;

                // Match the explicit C# preflight. For the historical
                // single-byte tables this also prevents malformed lengths from
                // consuming the following entry.
                const qsizetype minimumBytes = headerType == ModuleFormat::PE
                    ? qsizetype(length) * 2 : qsizetype(length);
                if (offset + minimumBytes > data.size()) {
                    parsed.error = QStringLiteral("incomplete string data");
                    break;
                }

                QString value = ReadLenString(data, offset, codePage, length);
                while (value.endsWith(QChar(u'\0')))
                    value.chop(1);

                if (currentId == -1)
                    currentId = baseId;
                if (!value.isEmpty()) {
                    body += QStringLiteral("\t%1, \"%2\"\n")
                                .arg(currentId)
                                .arg(Escape(value));
                    ++currentId;
                    ++parsed.strings;
                }
            } catch (const std::exception& exception) {
                parsed.error = QString::fromUtf8(exception.what());
                break;
            }
        }

        parsed.consumed = offset;
        parsed.complete = parsed.error.isEmpty() && offset == data.size();
        if (parsed.strings > 0 || parsed.complete) {
            parsed.text = QStringLiteral("STRINGTABLE\n{\n") + body;
            if (!parsed.error.isEmpty())
                parsed.text += QStringLiteral("  // ERROR: %1\n").arg(parsed.error);
            parsed.text += QStringLiteral("}\n");
        }
        return parsed;
    };

    if (headerType == ModuleFormat::PE) {
        const ParseResult parsed = parseTable(0, 1200);
        if (parsed.text.isEmpty())
            throw std::runtime_error(parsed.error.toUtf8().constData());
        return parsed.text;
    }

    const bool os2Resource = (((headerType == ModuleFormat::LE || headerType == ModuleFormat::NE) && isOs2)
                              || headerType == ModuleFormat::LX);
    if (!os2Resource) {
        // Win1/2 and later NE tables are byte-counted single-byte strings.
        // ASCII is the C# default; CP1252 is only a recovery for bytes above
        // 0x7F when the ASCII codec rejects the producer's ANSI data.
        ParseResult best = parseTable(0, 20127);
        if (best.text.isEmpty())
            best = parseTable(0, 1252);
        if (best.text.isEmpty())
            throw std::runtime_error(best.error.toUtf8().constData());
        return best.text;
    }

    // Real files exist both with and without the two-byte code-page prefix.
    // Evaluate all layouts and keep the structurally strongest parse instead
    // of committing to the first two bytes, which may simply be the first
    // string length and character.
    QVector<ParseResult> candidates;
    if (data.size() >= 2) {
        const quint16 declared = quint16(uchar(data.at(0))) |
                                 (quint16(uchar(data.at(1))) << 8);
        if (tryCodecForCodePage(declared))
            candidates.push_back(parseTable(2, declared));
        // Old resources occasionally contain an unknown/obsolete code-page
        // number but otherwise use the standard two-byte-prefixed layout.
        candidates.push_back(parseTable(2, 850));
        candidates.push_back(parseTable(2, 437));
    }
    candidates.push_back(parseTable(0, 850));
    candidates.push_back(parseTable(0, 437));

    const ParseResult* best = nullptr;
    for (const ParseResult& candidate : candidates) {
        if (candidate.text.isEmpty())
            continue;
        if (!best ||
            candidate.complete > best->complete ||
            (candidate.complete == best->complete && candidate.strings > best->strings) ||
            (candidate.complete == best->complete && candidate.strings == best->strings &&
             candidate.consumed > best->consumed))
            best = &candidate;
    }
    if (best)
        return best->text;

    QString error = QStringLiteral("incomplete string data");
    for (const ParseResult& candidate : candidates)
        if (!candidate.error.isEmpty()) { error = candidate.error; break; }
    throw std::runtime_error(error.toUtf8().constData());
}

ResourcePreview RT_STRING::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;

    try {
        preview.text = Get(entry.data, entry.format, entry.isOs2, entry.baseId);
    } catch (const std::exception& error) {
        preview.error = QString::fromUtf8(error.what());
    }

    return preview;
}

QString RT_STRING::Escape(const QString& value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return escaped;
}

} // namespace resources
} // namespace peare
