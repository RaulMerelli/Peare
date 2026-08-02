#include "RT_ACCELTABLE.h"
#include "../RT_ACCELERATOR/RT_ACCELERATOR.h"
#include "../RT_STRING/RT_STRING.h"

#include <QLatin1Char>
#include <stdexcept>

namespace peare { namespace resources {
namespace {

// Define AccelTypeFlags as const quint16 directly within the translation unit.
// These flags come from pmwin.h.
const quint16 KC_NONE = 0x0000;     /* Reserved */
const quint16 KC_CHAR = 0x0001;
const quint16 KC_VIRTUALKEY = 0x0002;
const quint16 KC_SCANCODE = 0x0004;
const quint16 KC_SHIFT = 0x0008;
const quint16 KC_CTRL = 0x0010;
const quint16 KC_ALT = 0x0020;
const quint16 KC_KEYUP = 0x0040;
const quint16 KC_PREVDOWN = 0x0080;
const quint16 KC_LONEKEY = 0x0100;
const quint16 KC_DEADKEY = 0x0200;
const quint16 KC_COMPOSITE = 0x0400;
const quint16 KC_INVALIDCOMP = 0x0800;

QString hex16(quint16 value)
{
    return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
}

} // namespace

QString RT_ACCELTABLE::Get(const QByteArray& data, ModuleFormat)
{
    QString result;
    qsizetype offset = 0;

    if (data.size() < 4)
        return QStringLiteral("Error: Invalid data or data too short. Expected at least 4 bytes.");

    try {
        // Read count and codepage (these are typically outside the ACCELERATORS block in RC)
        const quint16 count = RT_STRING::ReadUInt16(data, offset);
        const quint16 cp = RT_STRING::ReadUInt16(data, offset);
        Q_UNUSED(cp);

        // Add RC file header for accelerators
        //result += QStringLiteral("// Generated from RT_ACCELTABLE resource\n");
        //result += QStringLiteral("// Number of entries: %1, Codepage: %2\n").arg(count).arg(cp);
        result += QStringLiteral("ACCELERATORS\n");
        result += QStringLiteral("{\n");

        const qsizetype expected = 4 + qsizetype(count) * 6;
        if (data.size() < expected) {
            return QStringLiteral("Error: Data too short for %1 entries. Expected at least %2 bytes, but got %3. Aborting RC generation.")
                .arg(count).arg(expected).arg(data.size());
        }

        for (quint16 i = 0; i < count; ++i) {
            if (offset + 6 > data.size()) {
                result += QStringLiteral("    // WARNING: Not enough data for entry %1. Remaining bytes: %2. Truncated RC output.\n")
                    .arg(i + 1).arg(data.size() - offset);
                break;
            }

            const quint16 type = RT_STRING::ReadUInt16(data, offset);
            const quint16 key = RT_STRING::ReadUInt16(data, offset);
            const quint16 cmd = RT_STRING::ReadUInt16(data, offset);

            QString rcFlags;
            QString keyLiteral;

            // Determine if it's a CHAR or VIRTKEY based on AF_CHAR flag and key value
            const bool isCharKey = (type & KC_CHAR) != 0;

            if (isCharKey) {
                rcFlags += QStringLiteral("ASCII");
                keyLiteral = QStringLiteral("'%1'").arg(QChar(key)); // e.g., 'A'
            } else { // Assume VIRTKEY if AF_CHAR is not set
                rcFlags += QStringLiteral("VIRTKEY");
                keyLiteral = RT_ACCELERATOR::virtualKeyCodeMap().value(key, hex16(key));
            }

            // Append modifier flags
            if ((type & KC_SHIFT) != 0) rcFlags += QStringLiteral(", SHIFT");
            if ((type & KC_CTRL) != 0) rcFlags += QStringLiteral(", CONTROL");
            if ((type & KC_ALT) != 0) rcFlags += QStringLiteral(", ALT");
            if ((type & KC_KEYUP) != 0) rcFlags += QStringLiteral(", KEYUP");
            if ((type & KC_PREVDOWN) != 0) rcFlags += QStringLiteral(", PREVDOWN");
            if ((type & KC_LONEKEY) != 0) rcFlags += QStringLiteral(", LONEKEY");
            if ((type & KC_DEADKEY) != 0) rcFlags += QStringLiteral(", DEADKEY");
            if ((type & KC_COMPOSITE) != 0) rcFlags += QStringLiteral(", COMPOSITE");
            if ((type & KC_INVALIDCOMP) != 0) rcFlags += QStringLiteral(", INVALIDCOMP");

            // RC compilers only understand specific flags. Unrecognized bits are ignored for RC syntax.
            // For debugging, you could uncomment the following to see "unknown" bits in comments:
            const quint16 knownFlagsMask = quint16(KC_CHAR | KC_VIRTUALKEY | KC_SCANCODE |
                KC_SHIFT | KC_CTRL | KC_ALT | KC_KEYUP | KC_PREVDOWN | KC_LONEKEY |
                KC_DEADKEY | KC_COMPOSITE | KC_INVALIDCOMP);
            const quint16 unrecognizedFlags = quint16(type & ~knownFlagsMask);
            if (unrecognizedFlags != 0) {
                result += QStringLiteral("    // WARNING: Original type %1 had unrecognized bits: %2\n")
                    .arg(hex16(type), hex16(unrecognizedFlags));
            }

            result += QStringLiteral("    %1, %2, %3\n").arg(keyLiteral, hex16(cmd), rcFlags);
        }

        result += QStringLiteral("}\n");
    } catch (const std::exception& error) {
        return QStringLiteral("An unexpected error occurred during parsing: %1")
            .arg(QString::fromUtf8(error.what()));
    }

    return result;
}

ResourcePreview RT_ACCELTABLE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data, ModuleFormat::LX);
    return preview;
}

} } // namespace peare::resources
