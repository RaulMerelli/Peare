#include "RT_DISPLAYINFO.h"

namespace peare { namespace resources {
namespace {

quint16 readU16(const QByteArray& data, int offset)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

} // namespace

// Special thanks to bitsavers.trailing-edge.com for providing some IBM docs containig useful info
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/os2/OS2_2.x/IBM_OS2_2.0_Technical_Library_1992/S10G-6267-00_OS2_2.0_Presentation_Driver_Reference_199203.pdf
// page 244
QString RT_DISPLAYINFO::Get(const QByteArray& data)
{
    if (data.size() < 26)
        return QStringLiteral("Invalid data (must be at least 26 bytes).");

    const quint16 cb = readU16(data, 0x00);
    const quint16 cxIcon = readU16(data, 0x02);
    const quint16 cyIcon = readU16(data, 0x04);
    const quint16 cxPointer = readU16(data, 0x06);
    const quint16 cyPointer = readU16(data, 0x08);
    const quint16 cxBorder = readU16(data, 0x0A);
    const quint16 cyBorder = readU16(data, 0x0C);
    const quint16 cxHSlider = readU16(data, 0x0E);
    const quint16 cyVSlider = readU16(data, 0x10);
    const quint16 cxSizeBorder = readU16(data, 0x12);
    const quint16 cySizeBorder = readU16(data, 0x14);
    const quint16 cxDeviceAlign = readU16(data, 0x16);
    const quint16 cyDeviceAlign = readU16(data, 0x18);

    QString text;
    text += QStringLiteral("RT_DISPLAYINFO\n");
    text += QStringLiteral("{\n");
    text += QStringLiteral("\tSize:              %1 bytes\n").arg(cb);
    text += QStringLiteral("\tIcon Size:         %1 x %2 px\n").arg(cxIcon).arg(cyIcon);
    text += QStringLiteral("\tPointer Size:      %1 x %2 px\n").arg(cxPointer).arg(cyPointer);
    text += QStringLiteral("\tBorder Size:       %1 x %2 px\n").arg(cxBorder).arg(cyBorder);
    text += QStringLiteral("\tSlider Size:       %1 (H) x %2 (V) px\n").arg(cxHSlider).arg(cyVSlider);
    text += QStringLiteral("\tSize Border:       %1 x %2 px\n").arg(cxSizeBorder).arg(cySizeBorder);
    text += QStringLiteral("\tDevice Alignment:  %1 x %2 px\n").arg(cxDeviceAlign).arg(cyDeviceAlign);
    text += QStringLiteral("}\n");

    return text;
}

ResourcePreview RT_DISPLAYINFO::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;
    preview.text = Get(entry.data);
    return preview;
}

} } // namespace peare::resources
