#include "RT_VERSION.h"

#include <QLatin1Char>
#include <QString>
#include <stdexcept>

namespace peare { namespace resources {
namespace {

// The VS_HEADER structure is simplified to include only fields common to all blocks.
struct VsHeader {
    quint16 length;
    quint16 valueLength;
};

quint16 readUInt16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size())
        throw std::runtime_error("Attempted to read past end of version resource.");

    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

VsHeader readHeader(const QByteArray& data, qsizetype& offset)
{
    VsHeader header;
    header.length = readUInt16(data, offset);
    header.valueLength = readUInt16(data, offset + 2);
    offset += 4;
    return header;
}

void align4(qsizetype& offset)
{
    offset = (offset + 3) & ~qsizetype(3);
}

QString readAsciiZ(const QByteArray& data, qsizetype& offset)
{
    const qsizetype start = offset;
    // Advance offset until a null terminator (0x00) is found or the end of data is reached.
    while (offset < data.size() && data.at(offset) != '\0')
        ++offset;

    // Decode the ASCII string from the byte array.
    const QString value = QString::fromLatin1(data.constData() + start,
                                               int(offset - start));
    // Skip the null terminator if it exists.
    if (offset < data.size() && data.at(offset) == '\0')
        ++offset;
    return value;
}

QString readUnicodeZ(const QByteArray& data, qsizetype& offset)
{
    const qsizetype start = offset;
    // Advance offset by 2 bytes at a time (for UTF-16 characters) until a double null terminator (0x00 0x00) is found or the end of data is reached.
    while (offset + 1 < data.size()) {
        if (data.at(offset) == '\0' && data.at(offset + 1) == '\0')
            break;
        offset += 2;
    }

    QString value;
    // Use UTF-16 Little Endian. Unmappable replacement characters are converted to spaces.
    value.reserve(int((offset - start) / 2));
    for (qsizetype i = start; i + 1 < offset; i += 2) {
        const quint16 ch = readUInt16(data, i);
        value.append(QChar(ch == 0xFFFD ? quint16(' ') : ch));
    }

    // Skip the double null terminator if it exists.
    if (offset + 1 < data.size() && data.at(offset) == '\0' && data.at(offset + 1) == '\0')
        offset += 2;
    return value;
}

bool isUnicode(const QByteArray& data, qsizetype offset)
{
    // Unicode representation of "VS_VERSION".
    static const char marker[] = "VS_VERSION";
    const qsizetype count = qsizetype(sizeof(marker) - 1);
    if (offset + count * 2 > data.size())
        return false;

    for (qsizetype i = 0; i < count; ++i) {
        if (readUInt16(data, offset + i * 2) != quint16(static_cast<unsigned char>(marker[i])))
            return false;
    }
    return true;
}

QString escape(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return value;
}

void parseAndDump(const QByteArray& data,
                  qsizetype& offset,
                  qsizetype parentEnd,
                  QString& output,
                  int indent,
                  bool unicode)
{
    const qsizetype safeParentEnd = qMin(parentEnd, qsizetype(data.size()));

    while (offset < safeParentEnd && offset + 4 <= data.size()) {
        const qsizetype blockStart = offset;
        const VsHeader header = readHeader(data, offset);
        const qsizetype blockEnd = blockStart + header.length;

        if (header.length == 0 || blockEnd > safeParentEnd)
            break; // Invalid block or one that exceeds the parent's boundary

        if (unicode)
            offset += 2; // Skip two bytes when unicode

        const QString key = unicode ? readUnicodeZ(data, offset)
                                    : readAsciiZ(data, offset);
        align4(offset); // Align after reading the key

        const QString indentation(indent * 2, QLatin1Char(' '));
        QString value;
        bool hasValue = false;

        // Handle the block's value.
        if (header.valueLength > 0) {
            if (indent == 3) {
                // Read the complete null-terminated string value.
                value = unicode ? readUnicodeZ(data, offset)
                                : readAsciiZ(data, offset);
                hasValue = true;
            } else if (key == QStringLiteral("Translation") &&
                       header.valueLength >= 4 && offset + 4 <= data.size()) {
                // Special case for Translation (binary value).
                const quint16 languageId = readUInt16(data, offset);
                const quint16 codePage = readUInt16(data, offset + 2);
                value = QStringLiteral("%1 %2").arg(languageId).arg(codePage);
                offset += header.valueLength;
                hasValue = true;
            }
            align4(offset); // Align offset after reading (or skipping) the value
        }

        // Print the key and the value if present.
        if (hasValue) {
            output += indentation;
            output += key;
            output += QStringLiteral(" = \"");
            output += escape(value);
            output += QStringLiteral("\"\n");
        } else {
            output += indentation;
            output += key;
            output += QLatin1Char('\n');
        }

        if (header.valueLength == 0 && offset < blockEnd) {
            output += indentation + QStringLiteral("{\n");
            // Recursively parse child blocks until the end of the current parent block.
            parseAndDump(data, offset, blockEnd, output, indent + 1, unicode);
            output += indentation + QStringLiteral("}\n");
        }

        offset = blockEnd;
        align4(offset);
    }
}

} // namespace

QString RT_VERSION::Get(const QByteArray& data)
{
    if (data.size() < 4)
        return QStringLiteral("VERSIONINFO\n{\n}");

    qsizetype offset = 0;
    QString output = QStringLiteral("VERSIONINFO\n{\n");

    const qsizetype rootStart = offset;
    const VsHeader rootHeader = readHeader(data, offset);
    const bool unicode = isUnicode(data, 6); // Offset 6 is where "VS_VERSION_INFO" should be

    if (unicode)
        offset += 2; // Unicode version has additional fields we are not interested in

    const QString rootKey = unicode ? readUnicodeZ(data, offset)
                                    : readAsciiZ(data, offset);
    Q_UNUSED(rootKey);
    align4(offset);

    if (rootHeader.valueLength > 0) {
        offset += rootHeader.valueLength;
        align4(offset);
    }

    // Start parsing the child blocks of the root.
    parseAndDump(data,
                 offset,
                 rootStart + rootHeader.length,
                 output,
                 1,
                 unicode);

    output += QStringLiteral("}\n");
    return output;
}

ResourcePreview RT_VERSION::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    try {
        preview.text = Get(entry.data);
    } catch (const std::exception& error) {
        preview.error = QString::fromUtf8(error.what());
    }
    return preview;
}

} } // namespace peare::resources
