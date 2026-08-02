#include "RT_HELPSUBTABLE.h"

namespace peare { namespace resources {
namespace {

quint16 readU16(const QByteArray& data, int offset)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

} // namespace

QString RT_HELPSUBTABLE::Get(const QByteArray& data)
{
    if (data.size() < 2)
        return QString(); // Or throw an exception, depending on desired error handling

    const quint16 size = readU16(data, 0);
    if (size == 0) // Handle cases where size might be 0, though not explicitly mentioned for HELPSUBTABLE.
        return QString();

    QString text = QStringLiteral("RT_HELPSUBTABLE\r\n{\r\n");

    qint64 offset = 2; // Start after the 'size' field

    // Each subitem has 'size' integers, and we know wnd and help are the first two
    const qint64 subItemSizeInBytes = qint64(size) * 2; // Each integer is 16 bits (2 bytes)

    while (offset + subItemSizeInBytes <= data.size()) {
        const quint16 wnd = readU16(data, int(offset));
        const quint16 help = readU16(data, int(offset + 2));

        text += QStringLiteral("  %1, %2").arg(wnd).arg(help);

        // If size is more than 2, append the remaining integers
        for (quint16 i = 2; i < size; ++i) {
            if (offset + (qint64(i) * 2) + 2 <= data.size()) { // Ensure we don't go out of bounds
                const quint16 additionalValue = readU16(data, int(offset + qint64(i) * 2));
                text += QStringLiteral(", %1").arg(additionalValue);
            } else {
                // Data unexpectedly ends early for an additional value
                // This might indicate corrupted data or an unexpected format.
                // For now, we'll break and process what we have.
                break;
            }
        }
        text += QStringLiteral("\r\n");
        offset += subItemSizeInBytes;
    }

    text += QStringLiteral("}\r\n");
    return text;
}

ResourcePreview RT_HELPSUBTABLE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data);
    return preview;
}

} } // namespace peare::resources
