#include "RT_MESSAGE.h"

#include "../RT_STRING/RT_STRING.h"

#include <QStringList>
#include <QTextCodec>
#include <stdexcept>

namespace peare {
namespace resources {

// --- Structures for Windows Message Table ---
// These are essential and MUST accurately reflect the WinAPI definitions.
// In Qt we read the fields explicitly in little-endian order instead of relying on
// LayoutKind.Sequential and Marshal.PtrToStructure.

// MESSAGE_RESOURCE_HEADER is part of MESSAGE_RESOURCE_DATA.
// It contains NumberOfBlocks and is followed by MESSAGE_RESOURCE_BLOCKs.

// MESSAGE_RESOURCE_BLOCK defines a range of message IDs and their offset:
// LowId, HighId and OffsetToEntries.

// MESSAGE_RESOURCE_ENTRY is not explicitly a separate struct in the file,
// but its components are always a short Length, short Flags, and then the text.
// The Length field includes the 4-byte header (Length + Flags), the string content
// and its null terminator.
namespace {

quint16 readLe16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size())
        throw std::runtime_error("Attempted to read past end of message data.");

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(bytes[0]) | (quint16(bytes[1]) << 8);
}

quint32 readLe32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size())
        throw std::runtime_error("Attempted to read past end of message data.");

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(bytes[0])
        | (quint32(bytes[1]) << 8)
        | (quint32(bytes[2]) << 16)
        | (quint32(bytes[3]) << 24);
}

QString cleanMessage(QString message)
{
    // Clean up the message text: remove carriage returns and line feeds.
    message.replace(QStringLiteral("\r\n"), QString());
    return message;
}

QString decodeAnsiEntry(const QByteArray& data, qsizetype offset, qsizetype byteCount)
{
    if (byteCount < 0 || offset < 0 || offset + byteCount > data.size())
        throw std::runtime_error("Invalid ANSI message entry length.");

    QByteArray bytes = data.mid(offset, byteCount);
    const qsizetype nullIndex = bytes.indexOf('\0');
    if (nullIndex >= 0)
        bytes.truncate(nullIndex);
    return QString::fromLocal8Bit(bytes);
}

QString decodeUnicodeEntry(const QByteArray& data, qsizetype offset, qsizetype byteCount)
{
    if (byteCount < 0 || offset < 0 || offset + byteCount > data.size())
        throw std::runtime_error("Invalid Unicode message entry length.");

    byteCount -= byteCount % 2;
    QString value;
    value.reserve(int(byteCount / 2));

    for (qsizetype i = 0; i + 1 < byteCount; i += 2) {
        const quint16 ch = readLe16(data, offset + i);
        if (ch == 0)
            break;
        value.append(QChar(ch));
    }
    return value;
}

} // namespace

QString RT_MESSAGE::Get(const QByteArray& data,
                        ModuleFormat headerType,
                        bool isOs2)
{
    quint16 codePage = 20127; // Default ASCII
    qsizetype offset = 0; // Current read position in the data array

    if (((headerType == ModuleFormat::LE || headerType == ModuleFormat::NE) && isOs2)
        || headerType == ModuleFormat::LX)
    {
        // OS/2 1.x resources may omit the codepage prefix.
        codePage = 850;
        if (data.size() >= 2) {
            const quint16 candidate = quint16(uchar(data.at(0))) | (quint16(uchar(data.at(1))) << 8);
            const QByteArray codecName = QByteArray("IBM ") + QByteArray::number(candidate);
            if (candidate != 0 && QTextCodec::codecForName(codecName) != nullptr) {
                codePage = candidate;
                offset = 2;
            }
        }
    }

    QStringList output;
    output << QStringLiteral("MESSAGETABLE") << QStringLiteral("{");

    if (headerType == ModuleFormat::PE) {
        // --- Windows PE (RT_MESSAGETABLE) Parsing Logic ---
        if (data.size() < 4)
            throw std::runtime_error("Invalid MESSAGE_RESOURCE_DATA header.");

        // The first thing in MESSAGE_RESOURCE_DATA is MESSAGE_RESOURCE_HEADER.NumberOfBlocks.
        const quint32 numberOfBlocks = readLe32(data, 0);
        const quint64 blockTableEnd = 4ULL + quint64(numberOfBlocks) * 12ULL;
        if (blockTableEnd > quint64(data.size()))
            throw std::runtime_error("Incomplete MESSAGE_RESOURCE_BLOCK table.");

        // The blocks array starts immediately after NumberOfBlocks.
        qsizetype blockOffset = 4;
        for (quint32 blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            const quint32 lowId = readLe32(data, blockOffset);
            const quint32 highId = readLe32(data, blockOffset + 4);
            const quint32 entriesOffset = readLe32(data, blockOffset + 8);
            blockOffset += 12;

            if (highId < lowId)
                throw std::runtime_error("Invalid message ID range.");

            // OffsetToEntries is relative to the start of MESSAGE_RESOURCE_DATA.
            qsizetype entryOffset = qsizetype(entriesOffset);
            for (quint32 id = lowId; id <= highId; ++id) {
                if (entryOffset < 0 || entryOffset + 4 > data.size())
                    throw std::runtime_error("Incomplete MESSAGE_RESOURCE_ENTRY header.");

                // Read MESSAGE_RESOURCE_ENTRY fields.
                const quint16 entryLength = readLe16(data, entryOffset);
                const quint16 flags = readLe16(data, entryOffset + 2);
                if (entryLength < 4 || entryOffset + entryLength > data.size())
                    throw std::runtime_error("Invalid MESSAGE_RESOURCE_ENTRY length.");

                // The actual string data starts 4 bytes after the beginning of the entry.
                const qsizetype textOffset = entryOffset + 4;
                const qsizetype textLength = entryLength - 4;
                QString message;

                // Determine encoding based on flags.
                if (flags == 0) { // ANSI
                    message = decodeAnsiEntry(data, textOffset, textLength);
                } else if (flags == 1) { // Unicode (UTF-16LE)
                    message = decodeUnicodeEntry(data, textOffset, textLength);
                } else {
                    message = QStringLiteral("UNKNOWN_FLAGS_%1_FOR_ID_%2")
                                  .arg(flags)
                                  .arg(id);
                }

                output << QStringLiteral("\t0x%1, \"%2\"")
                              .arg(id, 4, 16, QLatin1Char('0'))
                              .arg(cleanMessage(message));
                // Advance to the next MESSAGE_RESOURCE_ENTRY using its total length.
                entryOffset += entryLength;

                if (id == 0xFFFFFFFFu)
                    break;
            }
        }
    } else { // Existing non-PE logic
        while (offset + 1 < data.size()) {
            const quint8 messageId = RT_STRING::ReadByte(data, offset);
            const QString message = RT_STRING::ReadNullTerminatedString(data, offset, codePage);
            output << QStringLiteral("\t0x%1, \"%2\"")
                          .arg(messageId, 4, 16, QLatin1Char('0'))
                          .arg(cleanMessage(message));
        }
    }

    output << QStringLiteral("}");
    return output.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

ResourcePreview RT_MESSAGE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    try {
        preview.text = Get(entry.data, entry.format, entry.isOs2);
    } catch (const std::exception& error) {
        preview.error = QString::fromUtf8(error.what());
    }
    return preview;
}

} // namespace resources
} // namespace peare
