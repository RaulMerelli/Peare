#include "RT_ACCELERATOR.h"
#include "../RT_ACCELTABLE/RT_ACCELTABLE.h"

#include <QLatin1Char>
#include <QStringList>

namespace peare { namespace resources {
namespace {

// ACCELTABLEENTRY flags as defined in Windows SDK (winuser.h)
const quint16 FVIRTKEY = 0x0001;   // The wAnsi member specifies a virtual-key code.
const quint16 FNOINVERT = 0x0002;  // Prevents highlighting of the menu item when the accelerator is used.
const quint16 FSHIFT = 0x0004;     // The SHIFT key must be held down.
const quint16 FCONTROL = 0x0008;   // The CTRL key must be held down.
const quint16 FALT = 0x0010;       // The ALT key must be held down.
const quint16 FLAST = 0x0080;      // Indicates the last entry in the accelerator table.

quint16 read16(const QByteArray& data, qsizetype offset)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

QString hex16(quint16 value)
{
    return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
}

} // namespace

// A mapping of common virtual key codes to their string representations for RC files.
// This helps in generating human-readable key names like VK_RETURN, VK_F5.
const QHash<quint16, QString>& RT_ACCELERATOR::virtualKeyCodeMap()
{
    static const QHash<quint16, QString> map = {
        {0x01, "VK_LBUTTON"}, {0x02, "VK_RBUTTON"}, {0x03, "VK_CANCEL"},
        {0x04, "VK_MBUTTON"}, {0x05, "VK_XBUTTON1"}, {0x06, "VK_XBUTTON2"},
        {0x08, "VK_BACK"}, {0x09, "VK_TAB"}, {0x0C, "VK_CLEAR"}, {0x0D, "VK_RETURN"},
        {0x10, "VK_SHIFT"}, {0x11, "VK_CONTROL"}, {0x12, "VK_MENU"}, {0x13, "VK_PAUSE"},
        {0x14, "VK_CAPITAL"}, {0x1B, "VK_ESCAPE"}, {0x20, "VK_SPACE"},
        {0x21, "VK_PRIOR"}, {0x22, "VK_NEXT"}, {0x23, "VK_END"}, {0x24, "VK_HOME"},
        {0x25, "VK_LEFT"}, {0x26, "VK_UP"}, {0x27, "VK_RIGHT"}, {0x28, "VK_DOWN"},
        {0x29, "VK_SELECT"}, {0x2A, "VK_PRINT"}, {0x2B, "VK_EXECUTE"}, {0x2C, "VK_SNAPSHOT"},
        {0x2D, "VK_INSERT"}, {0x2E, "VK_DELETE"}, {0x2F, "VK_HELP"},
        {0x30, "VK_0"}, {0x31, "VK_1"}, {0x32, "VK_2"}, {0x33, "VK_3"}, {0x34, "VK_4"},
        {0x35, "VK_5"}, {0x36, "VK_6"}, {0x37, "VK_7"}, {0x38, "VK_8"}, {0x39, "VK_9"},
        {0x41, "VK_A"}, {0x42, "VK_B"}, {0x43, "VK_C"}, {0x44, "VK_D"}, {0x45, "VK_E"},
        {0x46, "VK_F"}, {0x47, "VK_G"}, {0x48, "VK_H"}, {0x49, "VK_I"}, {0x4A, "VK_J"},
        {0x4B, "VK_K"}, {0x4C, "VK_L"}, {0x4D, "VK_M"}, {0x4E, "VK_N"}, {0x4F, "VK_O"},
        {0x50, "VK_P"}, {0x51, "VK_Q"}, {0x52, "VK_R"}, {0x53, "VK_S"}, {0x54, "VK_T"},
        {0x55, "VK_U"}, {0x56, "VK_V"}, {0x57, "VK_W"}, {0x58, "VK_X"}, {0x59, "VK_Y"},
        {0x5A, "VK_Z"}, {0x5B, "VK_LWIN"}, {0x5C, "VK_RWIN"}, {0x5D, "VK_APPS"},
        {0x60, "VK_NUMPAD0"}, {0x61, "VK_NUMPAD1"}, {0x62, "VK_NUMPAD2"},
        {0x63, "VK_NUMPAD3"}, {0x64, "VK_NUMPAD4"}, {0x65, "VK_NUMPAD5"},
        {0x66, "VK_NUMPAD6"}, {0x67, "VK_NUMPAD7"}, {0x68, "VK_NUMPAD8"},
        {0x69, "VK_NUMPAD9"}, {0x6A, "VK_MULTIPLY"}, {0x6B, "VK_ADD"},
        {0x6C, "VK_SEPARATOR"}, {0x6D, "VK_SUBTRACT"}, {0x6E, "VK_DECIMAL"}, {0x6F, "VK_DIVIDE"},
        {0x70, "VK_F1"}, {0x71, "VK_F2"}, {0x72, "VK_F3"}, {0x73, "VK_F4"},
        {0x74, "VK_F5"}, {0x75, "VK_F6"}, {0x76, "VK_F7"}, {0x77, "VK_F8"},
        {0x78, "VK_F9"}, {0x79, "VK_F10"}, {0x7A, "VK_F11"}, {0x7B, "VK_F12"},
        {0x7C, "VK_F13"}, {0x7D, "VK_F14"}, {0x7E, "VK_F15"}, {0x7F, "VK_F16"},
        {0x80, "VK_F17"}, {0x81, "VK_F18"}, {0x82, "VK_F19"}, {0x83, "VK_F20"},
        {0x84, "VK_F21"}, {0x85, "VK_F22"}, {0x86, "VK_F23"}, {0x87, "VK_F24"},
        {0x90, "VK_NUMLOCK"}, {0x91, "VK_SCROLL"},
        {0xA0, "VK_LSHIFT"}, {0xA1, "VK_RSHIFT"}, {0xA2, "VK_LCONTROL"},
        {0xA3, "VK_RCONTROL"}, {0xA4, "VK_LMENU"}, {0xA5, "VK_RMENU"},
        {0xF6, "VK_ATTN"}, {0xF7, "VK_CRSEL"}, {0xF8, "VK_EXSEL"},
        {0xF9, "VK_EREOF"}, {0xFA, "VK_PLAY"}, {0xFB, "VK_ZOOM"},
        {0xFC, "VK_NONAME"}, {0xFD, "VK_PA1"}, {0xFE, "VK_OEM_CLEAR"},
        // Extra IME keys if needed
        {0x18, "VK_FINAL"}, {0x19, "VK_KANJI"}
    };
    return map;
}

// Mapping for control characters to their ^X representation
const QHash<quint16, QString>& RT_ACCELERATOR::controlCharMap()
{
    static QHash<quint16, QString> map;
    if (map.isEmpty()) {
        for (quint16 i = 1; i <= 26; ++i)
            map.insert(i, QStringLiteral("^%1").arg(QChar(quint16('A') + i - 1)));
    }
    return map;
}

QString RT_ACCELERATOR::Get(const QByteArray& data, ModuleFormat format)
{
    if (data.isEmpty())
        return QStringLiteral("// No accelerator data provided or data is empty.");

    // Early exit for specific NE/LX types handled by another class.
    // In the Qt model OS/2 NE resources are dispatched to RT_ACCELTABLE before this method;
    // LX can be identified directly here.
    if (format == ModuleFormat::LX)
        return RT_ACCELTABLE::Get(data, format);

    QString result = QStringLiteral("ACCELERATORS\n{\n");
    const bool isNEFormat = format == ModuleFormat::NE;
    qsizetype offset = 0;
    bool isLastEntry = false;

    while (!isLastEntry && offset < data.size()) {
        quint16 flags; // Use quint16 for flags to be consistent for both PE and NE for bitwise operations.
                       // The actual bytes read will differ.
        quint16 rawKeyOrCharCode;
        quint16 commandID;

        if (isNEFormat) {
            // For NE (Windows 3.x), entries are 5 bytes: byte flags, ushort key, ushort command.
            if (data.size() - offset < 5) {
                result += QStringLiteral("// Warning: Incomplete accelerator entry detected at end of stream for NE format.\n");
                break;
            }
            flags = quint8(data.at(offset++)); // Read flags as a single byte, implicitly cast to quint16
            rawKeyOrCharCode = read16(data, offset); // Read key as quint16
            offset += 2;
            commandID = read16(data, offset); // Read command as quint16
            offset += 2;

            // The FLAST flag for NE is 0x80 within the single byte.
            isLastEntry = (flags & FLAST) != 0;

            // No explicit internal padding to skip for NE entries.
        } else { // Assume PE format
            // For PE, entries are 8 bytes: ushort flags, ushort key, ushort command, ushort padding.
            if (data.size() - offset < 8) { // Expect 8 bytes per entry for PE
                result += QStringLiteral("// Warning: Incomplete accelerator entry detected at end of stream for PE format.\n");
                break;
            }
            flags = read16(data, offset); // Read flags as quint16
            offset += 2;
            rawKeyOrCharCode = read16(data, offset); // Read key as quint16
            offset += 2;
            commandID = read16(data, offset); // Read command as quint16
            offset += 2;

            // For PE, FLAST is 0x0080 in the ushort flags.
            isLastEntry = (flags & FLAST) != 0;

            offset += 2; // Skip 2 bytes of padding after each PE entry
        }

        QStringList rcFlags;
        if ((flags & FCONTROL) != 0) rcFlags.append(QStringLiteral("CONTROL"));
        if ((flags & FSHIFT) != 0) rcFlags.append(QStringLiteral("SHIFT"));
        if ((flags & FALT) != 0) rcFlags.append(QStringLiteral("ALT"));
        if ((flags & FNOINVERT) != 0) rcFlags.append(QStringLiteral("NOINVERT"));

        QString keyString;
        if ((flags & FVIRTKEY) != 0) {
            rcFlags.append(QStringLiteral("VIRTKEY"));
            keyString = virtualKeyCodeMap().value(rawKeyOrCharCode, hex16(rawKeyOrCharCode));
        } else { // Not a VIRTKEY, so it's a character or control character (ASCII/ANSI)
            const QChar charCode(rawKeyOrCharCode);
            if (controlCharMap().contains(rawKeyOrCharCode)) {
                keyString = QStringLiteral("\"%1\"").arg(controlCharMap().value(rawKeyOrCharCode));
            } else if (charCode.isLetterOrNumber() || charCode.isPunct() || charCode.isSymbol() || charCode.isSpace()) {
                if (charCode == QLatin1Char('"'))
                    keyString = QStringLiteral("\"\\\"\"");
                else if (charCode == QLatin1Char('\\'))
                    keyString = QStringLiteral("\"\\\\\"");
                else
                    keyString = QStringLiteral("\"%1\"").arg(charCode);
            } else {
                // If it's an unprintable character, or something outside common ASCII, represent as hex.
                // PE accelerators can use Unicode chars, so X4 is more appropriate here.
                // For NE, it's typically just byte-sized ASCII.
                rcFlags.append(QStringLiteral("ASCII")); // Indicate it's an ASCII/ANSI character if not printable
                keyString = hex16(rawKeyOrCharCode);
            }
        }

        // Order flags for RC output: VIRTKEY, then modifiers, then others.
        QStringList orderedRcFlags;
        if (rcFlags.contains(QStringLiteral("VIRTKEY"))) orderedRcFlags.append(QStringLiteral("VIRTKEY"));
        if (rcFlags.contains(QStringLiteral("CONTROL"))) orderedRcFlags.append(QStringLiteral("CONTROL"));
        if (rcFlags.contains(QStringLiteral("SHIFT"))) orderedRcFlags.append(QStringLiteral("SHIFT"));
        if (rcFlags.contains(QStringLiteral("ALT"))) orderedRcFlags.append(QStringLiteral("ALT"));
        if (rcFlags.contains(QStringLiteral("NOINVERT"))) orderedRcFlags.append(QStringLiteral("NOINVERT"));
        if (rcFlags.contains(QStringLiteral("ASCII"))) orderedRcFlags.append(QStringLiteral("ASCII"));

        const QString flagsString = orderedRcFlags.isEmpty()
            ? QString()
            : QStringLiteral(", %1").arg(orderedRcFlags.join(QStringLiteral(", ")));

        result += QStringLiteral("\t%1, %2%3\n").arg(keyString).arg(commandID).arg(flagsString);

        if (isLastEntry) {
            // For NE, there might be residual padding after the FLAST entry
            // to align the entire resource. Consume it.
            if (isNEFormat)
                offset = data.size();
            break; // Stop processing after the last entry
        }
    }

    result += QStringLiteral("}\n");
    return result;
}

ResourcePreview RT_ACCELERATOR::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data, ModuleFormat::PE);
    return preview;
}

} } // namespace peare::resources
