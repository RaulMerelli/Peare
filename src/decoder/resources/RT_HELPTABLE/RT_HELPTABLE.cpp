#include "RT_HELPTABLE.h"

namespace peare { namespace resources {
namespace {

quint16 readU16(const QByteArray& data, int offset)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

} // namespace

// Special thanks to EDM2.com for providing how this resource is structured.
// The format is described at "when rt = 18" (search for it in the page)
// https://www.edm2.com/index.php/Resources_and_Decompiling_Them
QString RT_HELPTABLE::Get(const QByteArray& data)
{
    QString text;
    text += QStringLiteral("HELPTABLE\n");
    text += QStringLiteral("{\n");

    // Check for null or insufficient data.
    // Each help item requires 8 bytes (wnd, sub, separator, ext, each 16 bits).
    if (data.size() < 8) {
        text += QStringLiteral("}\n");
        return text;
    }

    // Iterate through the byte array, processing 8 bytes at a time for each help item.
    // The loop condition 'i + 7 < data.size()' ensures that we only attempt to read
    // a full 8-byte block, preventing an out-of-range access for incomplete items
    // at the end of the data array.
    for (int i = 0; i + 7 < data.size(); i += 8) {
        const quint16 wnd = readU16(data, i);     // application window ID
        const quint16 sub = readU16(data, i + 2); // help subtable ID
        const quint16 ext = readU16(data, i + 6); // extended help panel ID
        text += QStringLiteral("    %1, %2, %3\n").arg(wnd).arg(sub).arg(ext);
    }

    text += QStringLiteral("}\n");
    return text;
}

ResourcePreview RT_HELPTABLE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data);
    return preview;
}

} } // namespace peare::resources
