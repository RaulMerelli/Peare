#include "RT_MENU.h"
#include "OS2_RT_MENU.h"

#include <QTextCodec>
#include <QStringList>

namespace peare {
namespace resources {
namespace {

enum MenuFlags : quint16 {
    MF_ENABLED      = 0x0000,
    MF_STRING       = 0x0000,
    MF_DISABLED     = 0x0002,
    MF_GRAYED       = 0x0001,
    MF_BITMAP       = 0x0004,
    MF_CHECKED      = 0x0008,
    MF_POPUP        = 0x0010,
    MF_MENUBREAK    = 0x0040,
    MF_MENUBARBREAK = 0x0020,
    MF_UNCHECKED    = 0x0000,
    MF_SEPARATOR    = 0x0800,
    MF_BYCOMMAND    = 0x0000,
    MF_BYPOSITION   = 0x0400,
    MF_HELP         = 0x4000,
    MF_RIGHTJUSTIFY = 0x4000,
    MF_MOUSESELECT  = 0x8000,
    MF_END          = 0x0080
};

quint16 readUInt16(const QByteArray& data, qsizetype offset)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(bytes[0]) | (quint16(bytes[1]) << 8);
}

quint32 readUInt32(const QByteArray& data, qsizetype offset)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(bytes[0]) | (quint32(bytes[1]) << 8) |
           (quint32(bytes[2]) << 16) | (quint32(bytes[3]) << 24);
}

} // namespace

QString RT_MENU::Get(const QByteArray& data, ModuleFormat format, bool isOs2, int baseId)
{
    if (data.size() < 2)
        return QStringLiteral("Insufficient data for a valid menu header.");

    if (((format == ModuleFormat::LE || format == ModuleFormat::NE) && isOs2) ||
        format == ModuleFormat::LX)
    {
        ResourceEntry entry;
        entry.data = data;
        entry.format = format;
        entry.isOs2 = isOs2;
        entry.baseId = baseId;
        return OS2_RT_MENU::Get(data, entry);
    }

    QString menuOutput;
    qsizetype offset = 0;
    const bool isUnicode = format == ModuleFormat::PE;

    if (data.size() >= 4 && readUInt32(data, 0) == 0)
        offset += 4;

    int indentLevels = 0;
    auto indent = [&indentLevels]() { return QString(indentLevels * 2, QChar(u' ')); };

    menuOutput += QStringLiteral("MENU\n");
    menuOutput += QStringLiteral("{\n");
    ++indentLevels; // Start with the main MENU block at level 1

    while (offset < data.size()) {
        if (offset + 2 > data.size()) {
            menuOutput += indent() + QStringLiteral("Truncated data, unable to read usFlags.\n");
            break;
        }

        const quint16 usFlags = readUInt16(data, offset);
        offset += 2;

        const bool isPopup = (usFlags & MF_POPUP) != 0;
        const bool isEnd = (usFlags & MF_END) != 0;
        const bool isSeparatorFlag = (usFlags & MF_SEPARATOR) != 0;

        QString menuText;
        quint16 wID = 0;

        if (isPopup) {
            const qsizetype textEndOffset = isUnicode
                ? FindNullTerminatedUnicodeStringEnd(data, offset)
                : FindNullTerminatedAnsiStringEnd(data, offset);

            if (textEndOffset == offset && offset != data.size()) {
                // Handle case where textEndOffset is returned as startIndex due to invalid input
                if ((isUnicode && offset >= data.size() - 1) || (!isUnicode && offset >= data.size())) {
                    menuOutput += indent() + QStringLiteral("Insufficient data to read the text string for POPUP.\n");
                    break;
                }
            } else if (textEndOffset < 0 || textEndOffset > data.size()) {
                menuOutput += indent() + QStringLiteral("Insufficient data to read the text string for POPUP.\n");
                break;
            }

            qsizetype stringLengthInBytes = textEndOffset - offset;
            if (stringLengthInBytes < 0)
                stringLengthInBytes = 0;

            if (isUnicode) {
                if ((stringLengthInBytes % 2) != 0)
                    --stringLengthInBytes;
                menuText = QTextCodec::codecForName("UTF-16LE")->toUnicode(
                    data.constData() + offset, int(stringLengthInBytes));
                offset = textEndOffset + 2;
            } else {
                menuText = QTextCodec::codecForLocale()->toUnicode(
                    data.constData() + offset, int(stringLengthInBytes));
                offset = textEndOffset + 1;
            }

            const QString flagString = GetMenuFlagsString(quint16(usFlags & ~MF_POPUP));
            menuOutput += indent() + QStringLiteral("POPUP \"") + menuText + QStringLiteral("\"");
            if (!flagString.isEmpty())
                menuOutput += QStringLiteral(", ") + flagString;
            menuOutput += QChar(u'\n');
            menuOutput += indent() + QStringLiteral("{\n");
            ++indentLevels;
        } else {
            if (offset + 2 > data.size()) {
                menuOutput += indent() + QStringLiteral("Truncated data, unable to read wID for NORMAL MENU ITEM.\n");
                break;
            }
            wID = readUInt16(data, offset);
            offset += 2;

            const qsizetype textEndOffset = isUnicode
                ? FindNullTerminatedUnicodeStringEnd(data, offset)
                : FindNullTerminatedAnsiStringEnd(data, offset);

            if (textEndOffset == offset && offset != data.size()) {
                // Handle case where textEndOffset is returned as startIndex due to invalid input
                if (isUnicode ? (offset >= data.size() - 1) : (offset >= data.size())) {
                    menuOutput += indent() + QStringLiteral("Insufficient data to read the text string for NORMAL MENU ITEM.\n");
                    break;
                }
            } else if (textEndOffset < 0 || textEndOffset > data.size()) {
                menuOutput += indent() + QStringLiteral("Insufficient data to read the text string for NORMAL MENU ITEM.\n");
                break;
            }

            qsizetype stringLengthInBytes = textEndOffset - offset;
            if (stringLengthInBytes < 0)
                stringLengthInBytes = 0;

            if (isUnicode) {
                if ((stringLengthInBytes % 2) != 0)
                    --stringLengthInBytes;
                menuText = QTextCodec::codecForName("UTF-16LE")->toUnicode(
                    data.constData() + offset, int(stringLengthInBytes));
                offset = textEndOffset + 2;
            } else {
                menuText = QTextCodec::codecForLocale()->toUnicode(
                    data.constData() + offset, int(stringLengthInBytes));
                offset = textEndOffset + 1;
            }

            // Corrected separator logic
            if (isSeparatorFlag || (wID == 0 && menuText.isEmpty())) {
                menuOutput += indent() + QStringLiteral("MENUITEM SEPARATOR\n");
            } else {
                const QString flagString = GetMenuFlagsString(usFlags);
                menuOutput += indent() + QStringLiteral("MENUITEM \"") + menuText +
                              QStringLiteral("\", ") + QString::number(wID);
                if (!flagString.isEmpty())
                    menuOutput += QStringLiteral(", ") + flagString;
                menuOutput += QChar(u'\n');
            }

            if (isEnd && indentLevels > 1) {
                --indentLevels;
                menuOutput += indent() + QStringLiteral("}\n");
                if (IsRemainingDataNull(data, offset))
                    break;
            }
        }
    }

    while (indentLevels > 0) {
        --indentLevels;
        menuOutput += indent() + QStringLiteral("}\n");
    }

    return menuOutput;
}

ResourcePreview RT_MENU::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    result.text = Get(entry.data, entry.format, entry.isOs2, entry.baseId);
    const QString lower = result.text.trimmed().toLower();
    if (lower.startsWith(QStringLiteral("// error:")) ||
        lower.startsWith(QStringLiteral("insufficient data")) ||
        lower.contains(QStringLiteral("data too short")))
    {
        result.error = result.text.trimmed();
        result.text.clear();
    }
    return result;
}

// Helper method to check if remaining data is all nulls
bool RT_MENU::IsRemainingDataNull(const QByteArray& data, qsizetype startIndex)
{
    for (qsizetype i = startIndex; i < data.size(); ++i) {
        if (data.at(i) != '\0')
            return false;
    }
    return true;
}

QString RT_MENU::GetMenuFlagsString(quint16 flags)
{
    QStringList flagNames;

    if ((flags & MF_DISABLED) != 0) flagNames.append(QStringLiteral("DISABLED"));
    else if ((flags & MF_GRAYED) != 0) flagNames.append(QStringLiteral("GRAYED"));

    if ((flags & MF_BITMAP) != 0) flagNames.append(QStringLiteral("BITMAP"));
    if ((flags & MF_CHECKED) != 0) flagNames.append(QStringLiteral("CHECKED"));
    if ((flags & MF_MENUBREAK) != 0) flagNames.append(QStringLiteral("MENUBREAK"));
    if ((flags & MF_MENUBARBREAK) != 0) flagNames.append(QStringLiteral("MENUBARBREAK"));

    if (((flags & MF_HELP) != 0) && ((flags & MF_RIGHTJUSTIFY) != 0))
        flagNames.append(QStringLiteral("HELP | RIGHTJUSTIFY"));
    else if ((flags & MF_HELP) != 0)
        flagNames.append(QStringLiteral("HELP"));
    else if ((flags & MF_RIGHTJUSTIFY) != 0)
        flagNames.append(QStringLiteral("RIGHTJUSTIFY"));

    if ((flags & MF_MOUSESELECT) != 0) flagNames.append(QStringLiteral("MOUSESELECT"));

    return flagNames.join(QStringLiteral(", "));
}

qsizetype RT_MENU::FindNullTerminatedUnicodeStringEnd(const QByteArray& data, qsizetype startIndex)
{
    if (startIndex < 0 || startIndex >= data.size() - 1)
        return startIndex;

    for (qsizetype i = startIndex; i < data.size() - 1; i += 2) {
        if (data.at(i) == '\0' && data.at(i + 1) == '\0')
            return i;
    }
    return data.size() - (data.size() % 2);
}

qsizetype RT_MENU::FindNullTerminatedAnsiStringEnd(const QByteArray& data, qsizetype startIndex)
{
    if (startIndex < 0 || startIndex >= data.size())
        return startIndex;

    for (qsizetype i = startIndex; i < data.size(); ++i) {
        if (data.at(i) == '\0')
            return i;
    }
    return data.size();
}

} // namespace resources
} // namespace peare
