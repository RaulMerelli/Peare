#include "ResourceExport.h"

#include <QDir>
#include <QFileInfo>
#include <QBuffer>
#include <QRegularExpression>
#include <QTextCodec>
#include <QtEndian>

namespace peare {
namespace {

ResourceFileFormat format(const char* extension, const char* description, const char* mimeType)
{
    QString ext = QString::fromLatin1(extension);
    if (ext.isEmpty()) ext = QStringLiteral(".bin");
    if (!ext.startsWith(QLatin1Char('.'))) ext.prepend(QLatin1Char('.'));
    return {ext.toLower(), QString::fromLatin1(description), QString::fromLatin1(mimeType)};
}

bool startsWith(const QByteArray& data, std::initializer_list<unsigned char> bytes)
{
    if (data.size() < int(bytes.size())) return false;
    int i = 0;
    for (unsigned char value : bytes)
        if (quint8(data.at(i++)) != value) return false;
    return true;
}

bool asciiEquals(const QByteArray& data, int offset, const char* value, int explicitLength = -1)
{
    const int length = explicitLength >= 0 ? explicitLength : int(qstrlen(value));
    return offset >= 0 && data.size() >= offset + length &&
           QByteArray::fromRawData(data.constData() + offset, length) == QByteArray(value, length);
}

QString asciiWindow(const QByteArray& data, int offset, int count)
{
    if (offset < 0 || count < 0 || data.size() < offset + count) return QString();
    return QString::fromLatin1(data.constData() + offset, count);
}

quint16 read16(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 read32(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

bool isBitmapBitCount(quint16 bits)
{
    return bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
}

bool looksLikeDib(const QByteArray& data, int offset)
{
    if (offset < 0 || data.size() < offset + 12) return false;
    const quint32 size = read32(data, offset);
    if (size == 12) {
        return read16(data, offset + 4) > 0 && read16(data, offset + 6) > 0 &&
               read16(data, offset + 8) == 1 && isBitmapBitCount(read16(data, offset + 10));
    }
    if ((size == 16 || size == 40 || size == 52 || size == 56 || size == 64 || size == 108 || size == 124) &&
        data.size() >= offset + 16) {
        const qint32 width = qint32(read32(data, offset + 4));
        const qint32 height = qint32(read32(data, offset + 8));
        return width > 0 && height != 0 && read16(data, offset + 12) == 1 &&
               isBitmapBitCount(read16(data, offset + 14));
    }
    return false;
}

bool looksLikePcx(const QByteArray& data)
{
    if (data.size() < 128 || quint8(data.at(0)) != 0x0A) return false;
    const quint8 encoding = quint8(data.at(2));
    const quint8 bits = quint8(data.at(3));
    return encoding == 1 && (bits == 1 || bits == 2 || bits == 4 || bits == 8);
}

bool looksLikePortableAnymap(const QByteArray& data)
{
    if (data.size() < 3 || data.at(0) != 'P' || data.at(1) < '1' || data.at(1) > '7') return false;
    const char c = data.at(2);
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool looksLikeMp3(const QByteArray& data)
{
    if (asciiEquals(data, 0, "ID3")) return true;
    return data.size() >= 2 && quint8(data.at(0)) == 0xFF &&
           (quint8(data.at(1)) & 0xE0) == 0xE0 && (quint8(data.at(1)) & 0x18) != 0x08;
}

bool looksLikeMpegTransportStream(const QByteArray& data)
{
    if (data.size() < 376 || quint8(data.at(0)) != 0x47) return false;
    return quint8(data.at(188)) == 0x47 || (data.size() > 376 && quint8(data.at(376)) == 0x47);
}

QString decodeUtf16(const QByteArray& data, int offset, int length, bool bigEndian)
{
    const int units = length / 2;
    QVector<ushort> chars(units);
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    for (int i = 0; i < units; ++i)
        chars[i] = bigEndian ? qFromBigEndian<quint16>(p + i * 2) : qFromLittleEndian<quint16>(p + i * 2);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QString::fromUtf16(
        reinterpret_cast<const char16_t*>(chars.constData()),
        chars.size());
#else
    return QString::fromUtf16(chars.constData(), chars.size());
#endif
}

QString getTextPrefix(const QByteArray& data, int maximumBytes, bool* ok = nullptr)
{
    if (ok) *ok = false;
    if (data.isEmpty()) return QString();
    const int length = qMin(data.size(), maximumBytes);
    if (length >= 3 && startsWith(data, {0xEF,0xBB,0xBF})) {
        if (ok) *ok = true;
        return QString::fromUtf8(data.constData() + 3, length - 3);
    }
    if (length >= 2 && quint8(data.at(0)) == 0xFF && quint8(data.at(1)) == 0xFE) {
        if (ok) *ok = true;
        return decodeUtf16(data, 2, length - 2, false);
    }
    if (length >= 2 && quint8(data.at(0)) == 0xFE && quint8(data.at(1)) == 0xFF) {
        if (ok) *ok = true;
        return decodeUtf16(data, 2, length - 2, true);
    }

    int oddZeroes = 0, evenZeroes = 0;
    const int pairs = qMin(length / 2, 128);
    for (int i = 0; i < pairs; ++i) {
        if (quint8(data.at(i * 2)) == 0) ++evenZeroes;
        if (quint8(data.at(i * 2 + 1)) == 0) ++oddZeroes;
    }
    if (pairs >= 4 && oddZeroes > pairs / 2 && evenZeroes < pairs / 5) {
        if (ok) *ok = true;
        return decodeUtf16(data, 0, length - (length % 2), false);
    }
    if (pairs >= 4 && evenZeroes > pairs / 2 && oddZeroes < pairs / 5) {
        if (ok) *ok = true;
        return decodeUtf16(data, 0, length - (length % 2), true);
    }

    QTextCodec::ConverterState state;
    QString text = QTextCodec::codecForName("UTF-8")->toUnicode(data.constData(), length, &state);
    if (state.invalidChars == 0) {
        if (ok) *ok = true;
        return text;
    }
    QTextCodec* locale = QTextCodec::codecForLocale();
    if (!locale) return QString();
    if (ok) *ok = true;
    return locale->toUnicode(data.constData(), length);
}

bool looksLikeJson(const QString& text)
{
    if (text.isEmpty()) return false;
    const QChar first = text.at(0);
    if (first != QLatin1Char('{') && first != QLatin1Char('[')) return false;
    return text.lastIndexOf(first == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(']')) > 0;
}

bool looksLikeReadableText(const QByteArray& data)
{
    bool ok = false;
    const QString text = getTextPrefix(data, 4096, &ok);
    if (!ok || text.isEmpty()) return false;
    const int sampleLength = qMin(text.size(), 2048);
    int readable = 0;
    for (int i = 0; i < sampleLength; ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\r') || c == QLatin1Char('\n') || c == QLatin1Char('\t') ||
            c.category() != QChar::Other_Control)
            ++readable;
    }
    return sampleLength > 0 && readable >= sampleLength * 9 / 10;
}

ResourceFileFormat structural(const QString& type)
{
    if (type == QLatin1String("RT_GROUP_ICON")) return format(".grpicon", "Windows group icon resource", "application/octet-stream");
    if (type == QLatin1String("RT_GROUP_CURSOR")) return format(".grpcursor", "Windows group cursor resource", "application/octet-stream");
    if (type == QLatin1String("RT_CURSOR")) return format(".cur", "Windows cursor resource", "image/x-icon");
    if (type == QLatin1String("RT_POINTER")) return format(".ptr", "OS/2 pointer resource", "application/octet-stream");
    if (type == QLatin1String("RT_FONTDIR")) return format(".fontdir", "Font directory resource", "application/octet-stream");
    if (type == QLatin1String("RT_MENU")) return format(".menu", "Menu resource", "application/octet-stream");
    if (type == QLatin1String("RT_DIALOG")) return format(".dlg", "Dialog resource", "application/octet-stream");
    if (type == QLatin1String("RT_STRING")) return format(".str", "String-table resource", "application/octet-stream");
    if (type == QLatin1String("RT_VERSION")) return format(".version", "Version-information resource", "application/octet-stream");
    if (type == QLatin1String("RT_ACCELERATOR") || type == QLatin1String("RT_ACCELTABLE")) return format(".accel", "Accelerator-table resource", "application/octet-stream");
    if (type == QLatin1String("RT_MESSAGE") || type == QLatin1String("RT_MESSAGETABLE")) return format(".msgtable", "Message-table resource", "application/octet-stream");
    if (type == QLatin1String("RT_NAMETABLE")) return format(".nametable", "Name-table resource", "application/octet-stream");
    if (type == QLatin1String("RT_DISPLAYINFO")) return format(".displayinfo", "Display-information resource", "application/octet-stream");
    if (type == QLatin1String("RT_HELPTABLE")) return format(".helptable", "Help-table resource", "application/octet-stream");
    if (type == QLatin1String("RT_HELPSUBTABLE")) return format(".helpsubtable", "Help-subtable resource", "application/octet-stream");
    if (type == QLatin1String("RT_DLGINCLUDE")) return format(".dlginc", "Dialog-include resource", "application/octet-stream");
    if (type == QLatin1String("RT_DLGINIT")) return format(".dlginit", "Dialog-initialization resource", "application/octet-stream");
    if (type == QLatin1String("RT_TOOLBAR")) return format(".toolbar", "Toolbar resource", "application/octet-stream");
    return {};
}

ResourceFileFormat typed(const QString& type)
{
    if (type == QLatin1String("RT_BITMAP")) return format(".dib", "Device-independent bitmap resource", "image/bmp");
    if (type == QLatin1String("RT_ICON")) return format(".dib", "Icon image resource", "image/bmp");
    if (type == QLatin1String("RT_FONT")) return format(".fnt", "Bitmap font resource", "application/x-font");
    if (type == QLatin1String("RT_HTML")) return format(".html", "HTML document", "text/html");
    if (type == QLatin1String("RT_MANIFEST")) return format(".xml", "Application manifest", "application/xml");
    if (type == QLatin1String("RT_RCDATA")) return format(".rcdata", "Raw application resource", "application/octet-stream");
    if (type == QLatin1String("RT_ANIICON") || type == QLatin1String("RT_ANICURSOR")) return format(".ani", "Animated icon or cursor", "application/x-navi-animation");
    if (type == QLatin1String("RT_PLUGPLAY")) return format(".pnp", "Plug and Play resource", "application/octet-stream");
    if (type == QLatin1String("RT_VXD")) return format(".vxd", "Virtual device driver resource", "application/octet-stream");
    return {};
}

ResourceFileFormat detectRiff(const QByteArray& data)
{
    if (data.size() < 12 || (!asciiEquals(data, 0, "RIFF") && !asciiEquals(data, 0, "RIFX"))) return {};
    const QString form = asciiWindow(data, 8, 4);
    if (form == QLatin1String("AVI ")) return format(".avi", "AVI video", "video/x-msvideo");
    if (form == QLatin1String("WAVE")) return format(".wav", "WAVE audio", "audio/wav");
    if (form == QLatin1String("WEBP")) return format(".webp", "WebP image", "image/webp");
    if (form == QLatin1String("ACON")) return format(".ani", "Animated cursor", "application/x-navi-animation");
    if (form == QLatin1String("RMID")) return format(".rmi", "RIFF MIDI", "audio/midi");
    return format(".riff", "RIFF container", "application/riff");
}

ResourceFileFormat detectIff(const QByteArray& data)
{
    if (data.size() < 12 || !asciiEquals(data, 0, "FORM")) return {};
    const QString form = asciiWindow(data, 8, 4);
    if (form == QLatin1String("AIFF") || form == QLatin1String("AIFC")) return format(".aiff", "AIFF audio", "audio/aiff");
    if (form == QLatin1String("ILBM") || form == QLatin1String("PBM ") || form == QLatin1String("ACBM")) return format(".iff", "IFF bitmap image", "image/x-iff");
    if (form == QLatin1String("8SVX")) return format(".8svx", "8SVX audio", "audio/x-8svx");
    if (form == QLatin1String("ANIM")) return format(".anim", "IFF animation", "video/x-anim");
    return format(".iff", "IFF container", "application/x-iff");
}

ResourceFileFormat detectIsoBaseMedia(const QByteArray& data)
{
    if (data.size() < 12 || !asciiEquals(data, 4, "ftyp")) return {};
    const QString brand = asciiWindow(data, 8, 4);
    if (brand == QLatin1String("qt  ")) return format(".mov", "QuickTime movie", "video/quicktime");
    if (brand == QLatin1String("M4A ")) return format(".m4a", "MPEG-4 audio", "audio/mp4");
    if (brand == QLatin1String("M4B ")) return format(".m4b", "MPEG-4 audio", "audio/mp4");
    if (brand == QLatin1String("avif") || brand == QLatin1String("avis")) return format(".avif", "AVIF image", "image/avif");
    if (brand == QLatin1String("heic") || brand == QLatin1String("heix") || brand == QLatin1String("hevc") ||
        brand == QLatin1String("hevx") || brand == QLatin1String("mif1") || brand == QLatin1String("msf1"))
        return format(".heic", "HEIF image", "image/heic");
    return format(".mp4", "MPEG-4 container", "video/mp4");
}

ResourceFileFormat signature(const QByteArray& d)
{
    if (d.isEmpty()) return {};
    if (asciiEquals(d,0,"LIVE")) return format(".live","Xbox 360 LIVE container","application/x-xbox360-stfs");
    if (asciiEquals(d,0,"PIRS")) return format(".pirs","Xbox 360 PIRS container","application/x-xbox360-stfs");
    if (asciiEquals(d,0,"CON ")) return format(".con","Xbox 360 CON container","application/x-xbox360-stfs");
    if (asciiEquals(d,0,"FMIM")) return format(".fmim","Xbox 360 FMIM audio container","audio/x-xbox360-fmim");
    if (startsWith(d,{0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A})) return format(".png","PNG image","image/png");
    if (d.size() >= 3 && quint8(d[0]) == 0xFF && quint8(d[1]) == 0xD8 && quint8(d[2]) == 0xFF) return format(".jpg","JPEG image","image/jpeg");
    if (asciiEquals(d,0,"GIF87a") || asciiEquals(d,0,"GIF89a")) return format(".gif","GIF image","image/gif");
    if (asciiEquals(d,0,"BM")) return format(".bmp","Bitmap image","image/bmp");
    if (asciiEquals(d,0,"BA")) return format(".bmp","OS/2 bitmap array","image/bmp");
    if (asciiEquals(d,0,"IC") || asciiEquals(d,0,"CI") || asciiEquals(d,0,"CP") || asciiEquals(d,0,"PT")) return format(".ptr","OS/2 icon or pointer bitmap","application/octet-stream");
    if (startsWith(d,{0x49,0x49,0x2A,0x00}) || startsWith(d,{0x4D,0x4D,0x00,0x2A})) return format(".tif","TIFF image","image/tiff");
    if (startsWith(d,{0x00,0x00,0x01,0x00})) return format(".ico","Windows icon","image/x-icon");
    if (startsWith(d,{0x00,0x00,0x02,0x00})) return format(".cur","Windows cursor","image/x-icon");
    if (asciiEquals(d,0,"DDS ")) return format(".dds","DirectDraw surface","image/vnd-ms.dds");
    if (asciiEquals(d,0,"8BPS")) return format(".psd","Adobe Photoshop image","image/vnd.adobe.photoshop");
    if (asciiEquals(d,0,"qoif")) return format(".qoi","Quite OK Image","image/qoi");
    if (startsWith(d,{0x76,0x2F,0x31,0x01})) return format(".exr","OpenEXR image","image/x-exr");
    if (startsWith(d,{0x00,0x00,0x00,0x0C,0x6A,0x50,0x20,0x20,0x0D,0x0A,0x87,0x0A})) return format(".jp2","JPEG 2000 image","image/jp2");
    if (startsWith(d,{0xD7,0xCD,0xC6,0x9A})) return format(".wmf","Windows metafile","image/wmf");
    if (d.size() >= 44 && read32(d,0) == 1 && asciiEquals(d,40," EMF")) return format(".emf","Enhanced metafile","image/emf");
    if (looksLikePcx(d)) return format(".pcx","PCX image","image/x-pcx");
    if (looksLikePortableAnymap(d)) return format(".pnm","Portable anymap image","image/x-portable-anymap");

    ResourceFileFormat result = detectRiff(d); if (!result.extension.isEmpty()) return result;
    result = detectIff(d); if (!result.extension.isEmpty()) return result;
    result = detectIsoBaseMedia(d); if (!result.extension.isEmpty()) return result;

    if (startsWith(d,{0x1A,0x45,0xDF,0xA3})) {
        const QString prefix = asciiWindow(d,0,qMin(d.size(),256)).toLower();
        return prefix.contains(QStringLiteral("webm")) ? format(".webm","WebM video","video/webm") : format(".mkv","Matroska container","video/x-matroska");
    }
    if (startsWith(d,{0x00,0x00,0x01,0xBA})) return format(".mpg","MPEG program stream","video/mpeg");
    if (looksLikeMpegTransportStream(d)) return format(".ts","MPEG transport stream","video/mp2t");
    if (asciiEquals(d,0,"FLV")) return format(".flv","Flash video","video/x-flv");
    if (asciiEquals(d,0,"FWS") || asciiEquals(d,0,"CWS") || asciiEquals(d,0,"ZWS")) return format(".swf","Shockwave Flash","application/x-shockwave-flash");
    if (startsWith(d,{0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C})) return format(".asf","Advanced Systems Format","video/x-ms-asf");

    if (asciiEquals(d,0,"OggS")) return format(".ogg","Ogg container","application/ogg");
    if (asciiEquals(d,0,"fLaC")) return format(".flac","FLAC audio","audio/flac");
    if (asciiEquals(d,0,"MThd")) return format(".mid","MIDI sequence","audio/midi");
    if (asciiEquals(d,0,".snd")) return format(".au","Sun/NeXT audio","audio/basic");
    if (looksLikeMp3(d)) return format(".mp3","MPEG audio","audio/mpeg");

    if (startsWith(d,{0x50,0x4B,0x03,0x04}) || startsWith(d,{0x50,0x4B,0x05,0x06}) || startsWith(d,{0x50,0x4B,0x07,0x08})) return format(".zip","ZIP archive","application/zip");
    if (startsWith(d,{0x52,0x61,0x72,0x21,0x1A,0x07})) return format(".rar","RAR archive","application/vnd.rar");
    if (startsWith(d,{0x37,0x7A,0xBC,0xAF,0x27,0x1C})) return format(".7z","7-Zip archive","application/x-7z-compressed");
    if (startsWith(d,{0x1F,0x8B})) return format(".gz","GZip archive","application/gzip");
    if (asciiEquals(d,0,"BZh")) return format(".bz2","BZip2 archive","application/x-bzip2");
    if (startsWith(d,{0xFD,0x37,0x7A,0x58,0x5A,0x00})) return format(".xz","XZ archive","application/x-xz");
    if (asciiEquals(d,0,"MSCF")) return format(".cab","Microsoft Cabinet archive","application/vnd.ms-cab-compressed");
    if (d.size() > 262 && asciiEquals(d,257,"ustar")) return format(".tar","TAR archive","application/x-tar");

    if (asciiEquals(d,0,"%PDF-")) return format(".pdf","PDF document","application/pdf");
    if (startsWith(d,{0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1})) return format(".ole","OLE compound document","application/x-ole-storage");
    if (asciiEquals(d,0,"ITSF")) return format(".chm","Compiled HTML Help","application/vnd-ms-htmlhelp");
    if (asciiEquals(d,0,"SQLite format 3\0",16)) return format(".sqlite","SQLite database","application/vnd.sqlite3");
    if (asciiEquals(d,0,"regf")) return format(".hiv","Windows registry hive","application/octet-stream");
    if (startsWith(d,{0x4C,0x00,0x00,0x00,0x01,0x14,0x02,0x00})) return format(".lnk","Windows shortcut","application/x-ms-shortcut");
    if (asciiEquals(d,0,"Microsoft C/C++ MSF 7.00")) return format(".pdb","Program database","application/octet-stream");

    if (asciiEquals(d,0,"OTTO")) return format(".otf","OpenType font","font/otf");
    if (startsWith(d,{0x00,0x01,0x00,0x00}) || asciiEquals(d,0,"true")) return format(".ttf","TrueType font","font/ttf");
    if (asciiEquals(d,0,"ttcf")) return format(".ttc","TrueType collection","font/collection");
    if (asciiEquals(d,0,"wOFF")) return format(".woff","Web Open Font Format","font/woff");
    if (asciiEquals(d,0,"wOF2")) return format(".woff2","Web Open Font Format 2","font/woff2");

    if (startsWith(d,{0x4D,0x5A})) return format(".exe","DOS/Windows executable","application/vnd.microsoft.portable-executable");
    if (startsWith(d,{0x7F,0x45,0x4C,0x46})) return format(".elf","ELF executable","application/x-elf");
    if (startsWith(d,{0xCA,0xFE,0xBA,0xBE})) return format(".class","Java class","application/java-vm");
    if (asciiEquals(d,0,"dex\n")) return format(".dex","Android Dalvik executable","application/vnd.android.dex");
    if (startsWith(d,{0x00,0x61,0x73,0x6D})) return format(".wasm","WebAssembly module","application/wasm");
    if (startsWith(d,{0x1B,0x4C,0x75,0x61})) return format(".luac","Lua bytecode","application/octet-stream");

    bool textOk = false;
    QString text = getTextPrefix(d,2048,&textOk);
    if (textOk) {
        int i = 0;
        while (i < text.size() && (text.at(i) == QChar(0xFEFF) || text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('\t') || text.at(i) == QLatin1Char('\r') || text.at(i) == QLatin1Char('\n'))) ++i;
        const QString trimmed = text.mid(i);
        if (trimmed.startsWith(QStringLiteral("<!DOCTYPE html"),Qt::CaseInsensitive) || trimmed.startsWith(QStringLiteral("<html"),Qt::CaseInsensitive) ||
            trimmed.startsWith(QStringLiteral("<head"),Qt::CaseInsensitive) || trimmed.startsWith(QStringLiteral("<body"),Qt::CaseInsensitive)) return format(".html","HTML document","text/html");
        if (trimmed.startsWith(QStringLiteral("<svg"),Qt::CaseInsensitive)) return format(".svg","SVG image","image/svg+xml");
        if (trimmed.startsWith(QStringLiteral("<?xml"),Qt::CaseInsensitive) || trimmed.startsWith(QStringLiteral("<manifest"),Qt::CaseInsensitive) ||
            trimmed.startsWith(QStringLiteral("<assembly"),Qt::CaseInsensitive)) return format(".xml","XML document","application/xml");
        if (trimmed.startsWith(QStringLiteral("{\\rtf"),Qt::CaseInsensitive)) return format(".rtf","Rich Text Format document","application/rtf");
        if (looksLikeJson(trimmed)) return format(".json","JSON document","application/json");
    }
    if (looksLikeDib(d,0)) return format(".dib","Device-independent bitmap","image/bmp");
    return {};
}

} // namespace

ResourceFileFormat ResourceFormatDetector::detect(const QString& resourceType, const QByteArray& data)
{
    const QString type = resourceType.trimmed().toUpper();
    // Group resources use the same first bytes as ICO/CUR files, but their entries
    // contain resource IDs instead of file offsets.
    if (type == QLatin1String("RT_GROUP_ICON") || type == QLatin1String("RT_GROUP_CURSOR") || type == QLatin1String("RT_POINTER"))
        return structural(type);

    ResourceFileFormat result = signature(data);
    if (!result.extension.isEmpty()) return result;

    // Other known RT_* structures keep a descriptive extension when no complete
    // embedded file signature is present.
    result = structural(type);
    if (!result.extension.isEmpty()) return result;
    result = typed(type);
    if (!result.extension.isEmpty()) return result;
    if (looksLikeDib(data,0)) return format(".dib","Device-independent bitmap","image/bmp");
    if (looksLikeReadableText(data)) return format(".txt","Text","text/plain");
    return format(".bin","Unknown binary data","application/octet-stream");
}

QStringList ResourceConversion::availableExtensions(const ResourcePreview& preview)
{
    if (preview.rawDump) return {};
    if (!preview.embeddedExports.isEmpty()) {
        QStringList extensions;
        for (const EmbeddedExport& item : preview.embeddedExports)
            if (!extensions.contains(item.extension, Qt::CaseInsensitive)) extensions.append(item.extension);
        if (!extensions.isEmpty()) return extensions;
    }
    const QVector<QImage>& images = preview.conversionImages.isEmpty() ? preview.images : preview.conversionImages;
    if (!images.isEmpty()) return {QStringLiteral(".png"), QStringLiteral(".bmp")};
    if (!preview.text.isNull()) return {QStringLiteral(".txt")};
    return {};
}

QVector<ConvertedResourceFile> ResourceConversion::convert(const ResourcePreview& preview,
                                                            const QString& requestedExtension,
                                                            const QString& requestedBaseName)
{
    QVector<ConvertedResourceFile> result;
    if (preview.rawDump) return result;
    QString extension = requestedExtension.trimmed().toLower();
    if (!extension.startsWith(QLatin1Char('.'))) extension.prepend(QLatin1Char('.'));
    const QString baseName = sanitizeFileName(requestedBaseName);

    if (!preview.embeddedExports.isEmpty()) {
        for (const EmbeddedExport& item : preview.embeddedExports) {
            if (item.extension.compare(extension, Qt::CaseInsensitive) != 0) continue;
            ConvertedResourceFile exported;
            exported.fileName = sanitizeFileName(QFileInfo(item.fileName).fileName());
            exported.data = item.bytes;
            result.push_back(exported);
        }
        if (!result.isEmpty()) return result;
    }

    const QVector<QImage>& images = preview.conversionImages.isEmpty() ? preview.images : preview.conversionImages;
    if (!images.isEmpty() && (extension == QLatin1String(".png") || extension == QLatin1String(".bmp"))) {
        const QByteArray formatName = extension == QLatin1String(".png") ? QByteArrayLiteral("PNG") : QByteArrayLiteral("BMP");
        for (int i = 0; i < images.size(); ++i) {
            if (images[i].isNull()) continue;
            QByteArray encoded;
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly) || !images[i].save(&buffer, formatName.constData())) continue;
            QString suffix;
            if (!preview.conversionImages.isEmpty() && i < preview.conversionImageCodes.size())
                suffix = QStringLiteral("_char_%1").arg(preview.conversionImageCodes[i], 4, 16, QLatin1Char('0')).toUpper();
            else if (images.size() > 1)
                suffix = QStringLiteral("_%1").arg(i + 1, 3, 10, QLatin1Char('0'));
            ConvertedResourceFile exported;
            exported.fileName = baseName + suffix + extension;
            exported.data = encoded;
            result.push_back(exported);
        }
        return result;
    }

    if (extension == QLatin1String(".txt") && !preview.text.isNull()) {
        QByteArray encoded("\xEF\xBB\xBF", 3); // UTF-8 BOM, as in the C# UTF8Encoding(true) export.
        encoded += preview.text.toUtf8();
        ConvertedResourceFile exported;
        exported.fileName = baseName + extension;
        exported.data = encoded;
        result.push_back(exported);
    }
    return result;
}

QString sanitizeFileName(const QString& value)
{
    QString result = value.trimmed();
    if (result.isEmpty()) result = QStringLiteral("resource");
    result.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1F]")), QStringLiteral("_"));
    while (result.endsWith(QLatin1Char(' ')) || result.endsWith(QLatin1Char('.'))) result.chop(1);
    return result.isEmpty() ? QStringLiteral("resource") : result;
}

QString uniquePath(const QString& path)
{
    if (!QFileInfo::exists(path)) return path;
    const QFileInfo info(path);
    const QString directory = info.absolutePath();
    const QString extension = info.completeSuffix().isEmpty() ? QString() : QStringLiteral(".") + info.completeSuffix();
    const QString name = extension.isEmpty() ? info.fileName() : info.fileName().left(info.fileName().size() - extension.size());
    int index = 2;
    QString candidate;
    do {
        candidate = QDir(directory).filePath(name + QStringLiteral("_%1").arg(index++) + extension);
    } while (QFileInfo::exists(candidate));
    return candidate;
}

} // namespace peare
