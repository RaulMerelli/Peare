#include "RT_FONTDIR.h"

#include <QStringList>
#include <QTextCodec>
#include <stdexcept>

namespace peare { namespace resources {
namespace {

quint8 readU8(const QByteArray& data, qsizetype& off)
{
    if (off < 0 || off >= data.size()) throw std::runtime_error("Unexpected end of FONTDIR data.");
    return quint8(static_cast<unsigned char>(data.at(off++)));
}

quint16 readU16(const QByteArray& data, qsizetype& off)
{
    if (off < 0 || off + 2 > data.size()) throw std::runtime_error("Unexpected end of FONTDIR data.");
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    off += 2;
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

qint16 readS16(const QByteArray& data, qsizetype& off) { return qint16(readU16(data, off)); }

quint32 readU32(const QByteArray& data, qsizetype& off)
{
    if (off < 0 || off + 4 > data.size()) throw std::runtime_error("Unexpected end of FONTDIR data.");
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + off);
    off += 4;
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

qint32 readS32(const QByteArray& data, qsizetype& off) { return qint32(readU32(data, off)); }

QString readAsciiZ(const QByteArray& data, qsizetype& off)
{
    const qsizetype start = off;
    while (off < data.size() && data.at(off) != '\0') ++off;
    const QString value = QString::fromLatin1(data.constData() + start, int(off - start));
    if (off < data.size()) ++off;
    return value;
}

QString readCp850Fixed(const QByteArray& data, qsizetype& off, int length)
{
    if (off < 0 || off + length > data.size()) throw std::runtime_error("Unexpected end of FONTDIR string.");
    QByteArray bytes = data.mid(off, length);
    off += length;
    const int nul = bytes.indexOf('\0');
    if (nul >= 0) bytes.truncate(nul);
    QTextCodec* codec = QTextCodec::codecForName("IBM 850");
    if (!codec) throw std::runtime_error("CP850 codec is unavailable.");
    return codec->toUnicode(bytes).trimmed();
}


QString readCopyright(const QByteArray& data, qsizetype& off)
{
    if (off < 0 || off + 60 > data.size())
        throw std::runtime_error("Unexpected end of FONTDIRENTRY copyright.");

    QByteArray bytes = data.mid(off, 60);
    off += 60;

    // Convert copyright from byte[] to ASCII string.
    while (!bytes.isEmpty() && bytes.endsWith('\0'))
        bytes.chop(1);
    bytes.replace('\0', '\n');
    return QString::fromLatin1(bytes);
}

QString hex8(quint32 value)
{
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

bool looksLikeOs2(const QByteArray& data)
{
    if (data.size() < 6) return false;
    qsizetype off = 0;
    const qint16 type = readS16(data, off);
    const qint16 count = readS16(data, off);
    const qint16 blockSize = readS16(data, off); // Always 182
    Q_UNUSED(type);
    return count >= 0 && blockSize >= 166 && 6 + qsizetype(count) * blockSize <= data.size();
}

QString windowsFontDir(const QByteArray& data)
{
    if (data.size() < 2) return QString();
    qsizetype off = 0;
    const quint16 count = readU16(data, off);
    QStringList out;
    out << QStringLiteral("Font count: %1").arg(count);

    for (quint16 i = 0; i < count; ++i) {
        if (off + 2 > data.size()) { out << QStringLiteral("Unexpected end of data reading ordinal."); break; }
        const quint16 ordinal = readU16(data, off);
        const qsizetype start = off;
        if (start + 0x71 > data.size()) { out << QStringLiteral("Unexpected end of data reading FONTDIRENTRY struct."); break; }

        const quint16 version = readU16(data, off);
        const quint32 size = readU32(data, off);
        const QString copyright = readCopyright(data, off);

        const quint16 type = readU16(data, off);
        const quint16 points = readU16(data, off);
        const quint16 vertRes = readU16(data, off);
        const quint16 horizRes = readU16(data, off);
        const quint16 ascent = readU16(data, off);
        const quint16 internalLeading = readU16(data, off);
        const quint16 externalLeading = readU16(data, off);
        const quint8 italic = readU8(data, off);
        const quint8 underline = readU8(data, off);
        const quint8 strikeOut = readU8(data, off);
        const quint16 weight = readU16(data, off);
        const quint8 charSet = readU8(data, off);
        const quint16 pixWidth = readU16(data, off);
        const quint16 pixHeight = readU16(data, off);
        const quint8 pitchAndFamily = readU8(data, off);
        const quint16 avgWidth = readU16(data, off);
        const quint16 maxWidth = readU16(data, off);
        const quint8 firstChar = readU8(data, off);
        const quint8 lastChar = readU8(data, off);
        const quint8 defaultChar = readU8(data, off);
        const quint8 breakChar = readU8(data, off);
        const quint16 widthBytes = readU16(data, off);
        const quint32 device = readU32(data, off);
        const quint32 face = readU32(data, off);
        const quint32 reserved = readU32(data, off);

        off = start + 0x71;

        // Now read szDeviceName (null-terminated string).
        const QString deviceName = readAsciiZ(data, off);
        // Then szFaceName (null-terminated string), right after szDeviceName.
        const QString faceName = readAsciiZ(data, off);

        out << QStringLiteral("RT_FONT #%1:").arg(ordinal) << QStringLiteral("{")
            << QStringLiteral("\tVersion: %1").arg(version)
            << QStringLiteral("\tSize: %1").arg(size)
            << QStringLiteral("\tCopyright: %1").arg(copyright)
            << QStringLiteral("\tType: %1").arg(type)
            << QStringLiteral("\tPoints: %1").arg(points)
            << QStringLiteral("\tVertRes: %1").arg(vertRes)
            << QStringLiteral("\tHorizRes: %1").arg(horizRes)
            << QStringLiteral("\tAscent: %1").arg(ascent)
            << QStringLiteral("\tInternalLeading: %1").arg(internalLeading)
            << QStringLiteral("\tExternalLeading: %1").arg(externalLeading)
            << QStringLiteral("\tItalic: %1").arg(italic)
            << QStringLiteral("\tUnderline: %1").arg(underline)
            << QStringLiteral("\tStrikeOut: %1").arg(strikeOut)
            << QStringLiteral("\tWeight: %1").arg(weight)
            << QStringLiteral("\tCharSet: %1").arg(charSet)
            << QStringLiteral("\tPixWidth: %1").arg(pixWidth)
            << QStringLiteral("\tPixHeight: %1").arg(pixHeight)
            << QStringLiteral("\tPitchAndFamily: %1").arg(pitchAndFamily)
            << QStringLiteral("\tAvgWidth: %1").arg(avgWidth)
            << QStringLiteral("\tMaxWidth: %1").arg(maxWidth)
            << QStringLiteral("\tFirstChar: %1").arg(firstChar)
            << QStringLiteral("\tLastChar: %1").arg(lastChar)
            << QStringLiteral("\tDefaultChar: %1").arg(defaultChar)
            << QStringLiteral("\tBreakChar: %1").arg(breakChar)
            << QStringLiteral("\tWidthBytes: %1").arg(widthBytes)
            << QStringLiteral("\tDevice: 0x%1").arg(hex8(device))
            << QStringLiteral("\tFace: 0x%1").arg(hex8(face))
            << QStringLiteral("\tReserved: 0x%1").arg(hex8(reserved))
            << QStringLiteral("\tDeviceName: %1").arg(deviceName)
            << QStringLiteral("\tFaceName: %1").arg(faceName)
            << QStringLiteral("}") << QString();
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString os2FontDir(const QByteArray& data)
{
    qsizetype off = 0;
    const qint16 resourceType = readS16(data, off); // Even when the font is #1000, here it is always 6 (RT_FONT)
    const qint16 count = readS16(data, off);
    const qint16 blockSize = readS16(data, off); // Always 182
    Q_UNUSED(resourceType);
    if (count < 0 || blockSize < 166) throw std::runtime_error("Invalid OS/2 FONTDIR dimensions.");

    static const char* names[] = {
        "usRegistryId","usCodePage","yEmHeight","yXHeight","yMaxAscender","yMaxDescender",
        "yLowerCaseAscent","yLowerCaseDescent","yInternalLeading","yExternalLeading",
        "xAveCharWidth","xMaxCharInc","xEmInc","yMaxBaselineExt","sCharSlope","sInlineDir",
        "sCharRot","usWeightClass","usWidthClass","xDeviceRes","yDeviceRes","usFirstChar",
        "usLastChar","usDefaultChar","usBreakChar","usNominalPointSize","usMinimumPointSize",
        "usMaximumPointSize","usTypeFlags","fsDefn","fsSelectionFlags","fsCapabilities",
        "ySubscriptXSize","ySubscriptYSize","ySubscriptXOffset","ySubscriptYOffset",
        "ySuperscriptXSize","ySuperscriptYSize","ySuperscriptXOffset","ySuperscriptYOffset",
        "yUnderscoreSize","yUnderscorePosition","yStrikeoutSize","yStrikeoutPosition",
        "usKerningPairs","sFamilyClass"
    };

    QStringList out;
    out << QStringLiteral("RT_FONTDIR") << QStringLiteral("{");
    for (int i = 0; i < count; ++i) {
        const qsizetype start = 6 + qsizetype(i) * blockSize;
        if (start + blockSize > data.size()) throw std::runtime_error("Incomplete OS/2 FONTDIR block.");
        QByteArray block = data.mid(start, blockSize);
        qsizetype p = 0;
        // First two bytes are the resource id.
        const qint16 resourceId = readS16(block, p);
        // From offset 2 the structure is FOCAMETRICS.
        const qint32 identity = readS32(block, p);
        const qint32 size = readS32(block, p);
        const QString family = readCp850Fixed(block, p, 32);
        const QString face = readCp850Fixed(block, p, 32);

        out << QStringLiteral("    RT_FONT #%1").arg(resourceId)
            << QStringLiteral("    {")
            << QStringLiteral("        ulIdentity = %1").arg(identity)
            << QStringLiteral("        ulSize = %1").arg(size)
            << QStringLiteral("        szFamilyname = \"%1\"").arg(family)
            << QStringLiteral("        szFacename = \"%1\"").arg(face);

        for (const char* rawName : names) {
            const QString name = QString::fromLatin1(rawName);
            const bool isUnsigned = name == QStringLiteral("usCodePage") ||
                                    name == QStringLiteral("usWeightClass") ||
                                    name == QStringLiteral("usWidthClass");
            if (isUnsigned)
                out << QStringLiteral("        %1 = %2").arg(name).arg(readU16(block, p));
            else
                out << QStringLiteral("        %1 = %2").arg(name).arg(readS16(block, p));
        }
        out << QStringLiteral("    }");
    }
    out << QStringLiteral("}");
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace

QString RT_FONTDIR::Get(const QByteArray& data, ModuleFormat format, bool isOs2)
{
    if (format == ModuleFormat::LX || isOs2)
        return os2FontDir(data);
    return windowsFontDir(data);
}

ResourcePreview RT_FONTDIR::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    try {
        // ResourceEntry currently does not carry the module header/version properties.
        // Use the data signature only in the preview adapter; Get() keeps the C# routing semantics.
        result.text = looksLikeOs2(entry.data)
            ? Get(entry.data, ModuleFormat::Unknown, true)
            : Get(entry.data, ModuleFormat::PE, false);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} } // namespace peare::resources
