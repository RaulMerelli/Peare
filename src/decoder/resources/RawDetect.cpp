#include "RawDetect.h"
#include "RT_BITMAP/RT_BITMAP.h"
#include "RT_CURSOR/RT_CURSOR.h"
#include "RT_ICON/Win12MonochromeResource.h"
#include "RT_FONT/RT_FONT.h"
#include "WSZ/WszDecoder.h"
#include "FMIM/FMIM.h"

#include <QImage>
#include <QTextCodec>

namespace peare {
namespace resources {
namespace {

quint16 readUInt16(const QByteArray& data, int offset)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(bytes[0]) | (quint16(bytes[1]) << 8);
}

quint32 readUInt32(const QByteArray& data, int offset)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(bytes[0]) |
           (quint32(bytes[1]) << 8) |
           (quint32(bytes[2]) << 16) |
           (quint32(bytes[3]) << 24);
}

qint32 readInt32(const QByteArray& data, int offset)
{
    return qint32(readUInt32(data, offset));
}

bool isKnownBitCount(quint16 bitCount)
{
    return bitCount == 1 || bitCount == 2 || bitCount == 4 ||
           bitCount == 8 || bitCount == 16 || bitCount == 24 ||
           bitCount == 32;
}

bool isDibHeader(const QByteArray& data, int offset)
{
    if (offset < 0 || data.size() < offset + 12)
        return false;

    const quint32 headerSize = readUInt32(data, offset);
    if (headerSize == 12)
    {
        const quint16 width = readUInt16(data, offset + 4);
        const quint16 height = readUInt16(data, offset + 6);
        const quint16 planes = readUInt16(data, offset + 8);
        const quint16 bitCount = readUInt16(data, offset + 10);
        return width > 0 && height > 0 && planes == 1 && isKnownBitCount(bitCount);
    }

    if (headerSize == 16 || headerSize == 40 || headerSize == 52 ||
        headerSize == 56 || headerSize == 64 || headerSize == 108 ||
        headerSize == 124)
    {
        if (data.size() < offset + 16)
            return false;

        const qint32 width = readInt32(data, offset + 4);
        const qint32 height = readInt32(data, offset + 8);
        const quint16 planes = readUInt16(data, offset + 12);
        const quint16 bitCount = readUInt16(data, offset + 14);

        return width > 0 && height != 0 && planes == 1 && isKnownBitCount(bitCount);
    }

    return false;
}

bool hasBitmapResourceSignature(const QByteArray& data)
{
    if (data.size() < 2)
        return false;

    const quint16 signature = readUInt16(data, 0);
    return signature == 0x4142 || // BA - OS/2 bitmap array
           signature == 0x4D42 || // BM - bitmap file
           signature == 0x4943 || // CI
           signature == 0x4349 || // IC
           signature == 0x5043 || // CP
           signature == 0x5450;   // PT
}

bool hasStandardImageSignature(const QByteArray& data)
{
    if (data.size() >= 8 &&
        uchar(data[0]) == 0x89 && uchar(data[1]) == 0x50 &&
        uchar(data[2]) == 0x4E && uchar(data[3]) == 0x47 &&
        uchar(data[4]) == 0x0D && uchar(data[5]) == 0x0A &&
        uchar(data[6]) == 0x1A && uchar(data[7]) == 0x0A)
        return true; // PNG

    if (data.size() >= 3 && uchar(data[0]) == 0xFF &&
        uchar(data[1]) == 0xD8 && uchar(data[2]) == 0xFF)
        return true; // JPEG

    if (data.size() >= 6 &&
        (data.left(6) == QByteArrayLiteral("GIF87a") ||
         data.left(6) == QByteArrayLiteral("GIF89a")))
        return true;

    if (data.size() >= 4 &&
        ((uchar(data[0]) == 0x49 && uchar(data[1]) == 0x49 &&
          uchar(data[2]) == 0x2A && uchar(data[3]) == 0x00) ||
         (uchar(data[0]) == 0x4D && uchar(data[1]) == 0x4D &&
          uchar(data[2]) == 0x00 && uchar(data[3]) == 0x2A)))
        return true; // TIFF

    if (data.size() >= 2 && uchar(data[0]) == 0x42 && uchar(data[1]) == 0x4D)
        return true; // BMP file

    if (data.size() >= 4 && uchar(data[0]) == 0x00 && uchar(data[1]) == 0x00 &&
        ((uchar(data[2]) == 0x01 && uchar(data[3]) == 0x00) ||
         (uchar(data[2]) == 0x02 && uchar(data[3]) == 0x00)))
        return true; // ICO or CUR file

    if (data.size() >= 4 && uchar(data[0]) == 0xD7 && uchar(data[1]) == 0xCD &&
        uchar(data[2]) == 0xC6 && uchar(data[3]) == 0x9A)
        return true; // Placeable WMF

    if (data.size() >= 44 && readUInt32(data, 0) == 1 &&
        uchar(data[40]) == 0x20 && uchar(data[41]) == 0x45 &&
        uchar(data[42]) == 0x4D && uchar(data[43]) == 0x46)
        return true; // EMF

    return false;
}

bool looksLikeUtf16(const QByteArray& data, bool littleEndian)
{
    const int pairs = qMin(data.size() / 2, 128);
    if (pairs < 4)
        return false;

    int expectedZeroes = 0;
    int unexpectedZeroes = 0;
    for (int i = 0; i < pairs; ++i)
    {
        const uchar first = uchar(data[i * 2]);
        const uchar second = uchar(data[i * 2 + 1]);
        const uchar expected = littleEndian ? second : first;
        const uchar unexpected = littleEndian ? first : second;
        if (expected == 0) ++expectedZeroes;
        if (unexpected == 0) ++unexpectedZeroes;
    }

    return expectedZeroes >= pairs * 3 / 5 && unexpectedZeroes <= pairs / 5;
}

bool looksLikeSingleByteText(const QByteArray& data)
{
    int sampleLength = qMin(data.size(), 4096);
    while (sampleLength > 0 && data[sampleLength - 1] == 0)
        --sampleLength;
    if (sampleLength == 0)
        return false;

    int readable = 0;
    int zeroes = 0;
    for (int i = 0; i < sampleLength; ++i)
    {
        const uchar value = uchar(data[i]);
        if (value == 0)
            ++zeroes;
        if (value == 9 || value == 10 || value == 13 || value >= 32)
            ++readable;
    }

    return zeroes == 0 && readable >= sampleLength * 9 / 10;
}

bool looksLikeReadableText(const QString& text)
{
    if (text.isEmpty())
        return false;

    const int sampleLength = qMin(text.size(), 4096);
    int readable = 0;
    int zeroes = 0;
    for (int i = 0; i < sampleLength; ++i)
    {
        const QChar value = text[i];
        if (value == QChar(u'\0'))
            ++zeroes;
        if (value == QChar(u'\t') || value == QChar(u'\r') ||
            value == QChar(u'\n') || value.category() != QChar::Other_Control)
            ++readable;
    }

    return zeroes <= qMax(1, sampleLength / 100) &&
           readable >= sampleLength * 9 / 10;
}

bool hasKnownTextHeader(const QString& text)
{
    return text.startsWith(QStringLiteral("<!DOCTYPE html"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<html"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<head"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<body"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<?xml"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<svg"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<manifest"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("<assembly"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("{\\rtf"), Qt::CaseInsensitive) ||
           text.startsWith(QStringLiteral("@charset"), Qt::CaseInsensitive) ||
           text.startsWith(QChar(u'{')) || text.startsWith(QChar(u'['));
}

QString decodeUtf32(const QByteArray& data, int offset, bool littleEndian, bool* ok)
{
    *ok = false;
    if ((data.size() - offset) % 4 != 0)
        return {};

    QString result;
    result.reserve((data.size() - offset) / 4);
    for (int pos = offset; pos < data.size(); pos += 4)
    {
        const auto b0 = quint32(uchar(data[pos]));
        const auto b1 = quint32(uchar(data[pos + 1]));
        const auto b2 = quint32(uchar(data[pos + 2]));
        const auto b3 = quint32(uchar(data[pos + 3]));
        const quint32 codePoint = littleEndian
            ? b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
            : b3 | (b2 << 8) | (b1 << 16) | (b0 << 24);
        if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return {};
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const char32_t character = static_cast<char32_t>(codePoint);
        result.append(QString::fromUcs4(&character, 1));
#else
        result.append(QString::fromUcs4(&codePoint, 1));
#endif
    }
    *ok = true;
    return result;
}

QString decodeStrict(QTextCodec* codec, const QByteArray& bytes, bool* ok)
{
    QTextCodec::ConverterState state(QTextCodec::ConvertInvalidToNull);
    QString text = codec->toUnicode(bytes.constData(), bytes.size(), &state);
    *ok = state.invalidChars == 0;
    return *ok ? text : QString();
}

QString tryDecodeText(const QByteArray& data)
{
    if (data.isEmpty())
        return {};

    QString text;
    int offset = 0;
    bool hasBom = true;
    bool inferredUnicode = false;
    bool ok = false;

    if (data.size() >= 4 && uchar(data[0]) == 0x00 && uchar(data[1]) == 0x00 &&
        uchar(data[2]) == 0xFE && uchar(data[3]) == 0xFF)
    {
        offset = 4;
        text = decodeUtf32(data, offset, false, &ok);
    }
    else if (data.size() >= 4 && uchar(data[0]) == 0xFF && uchar(data[1]) == 0xFE &&
             uchar(data[2]) == 0x00 && uchar(data[3]) == 0x00)
    {
        offset = 4;
        text = decodeUtf32(data, offset, true, &ok);
    }
    else if (data.size() >= 3 && data.startsWith("\xEF\xBB\xBF"))
    {
        offset = 3;
        text = decodeStrict(QTextCodec::codecForName("UTF-8"), data.mid(offset), &ok);
    }
    else if (data.size() >= 2 && uchar(data[0]) == 0xFE && uchar(data[1]) == 0xFF)
    {
        offset = 2;
        text = decodeStrict(QTextCodec::codecForName("UTF-16BE"), data.mid(offset), &ok);
    }
    else if (data.size() >= 2 && uchar(data[0]) == 0xFF && uchar(data[1]) == 0xFE)
    {
        offset = 2;
        text = decodeStrict(QTextCodec::codecForName("UTF-16LE"), data.mid(offset), &ok);
    }
    else
    {
        hasBom = false;
        if (looksLikeUtf16(data, true))
        {
            text = decodeStrict(QTextCodec::codecForName("UTF-16LE"), data, &ok);
            inferredUnicode = true;
        }
        else if (looksLikeUtf16(data, false))
        {
            text = decodeStrict(QTextCodec::codecForName("UTF-16BE"), data, &ok);
            inferredUnicode = true;
        }
        else
        {
            text = decodeStrict(QTextCodec::codecForName("UTF-8"), data, &ok);
        }
    }

    if (!ok)
    {
        if (hasBom || !looksLikeSingleByteText(data))
            return {};

        // Encoding.Default in the C# implementation is represented by the
        // current locale codec in Qt.
        text = QTextCodec::codecForLocale()->toUnicode(data);
    }

    while (text.endsWith(QChar(u'\0')))
        text.chop(1);
    if (!looksLikeReadableText(text))
        return {};

    QString trimmed = text;
    int first = 0;
    while (first < trimmed.size() &&
           (trimmed[first] == QChar(0xFEFF) || trimmed[first] == QChar(u' ') ||
            trimmed[first] == QChar(u'\t') || trimmed[first] == QChar(u'\r') ||
            trimmed[first] == QChar(u'\n')))
        ++first;
    trimmed.remove(0, first);

    if (trimmed.isEmpty())
        return text;

    if (hasBom || inferredUnicode || hasKnownTextHeader(trimmed) ||
        looksLikeSingleByteText(data))
        return text;

    return {};
}

} // namespace


QString RawDetect::DumpRaw(const QByteArray& data, bool showAddressAndAscii)
{
    if (data.isEmpty())
        return QStringLiteral("No data.");

    static const char hexDigits[] = "0123456789ABCDEF";
    const qsizetype lineCount = (data.size() + 15) / 16;
    QString result;
    result.reserve(int(lineCount * (showAddressAndAscii ? 76 : 50) + 2));

    for (qsizetype line = 0; line < data.size(); line += 16)
    {
        const int lineLength = int(qMin<qsizetype>(16, data.size() - line));
        if (showAddressAndAscii)
        {
            const QString address = QString::number(line, 16).toUpper().rightJustified(4, QLatin1Char('0'));
            result += address;
            result += QStringLiteral(": ");
        }

        for (int j = 0; j < 16; ++j)
        {
            if (j < lineLength)
            {
                const uchar value = uchar(data.at(line + j));
                result += QLatin1Char(hexDigits[value >> 4]);
                result += QLatin1Char(hexDigits[value & 0x0F]);
                result += QLatin1Char(' ');
            }
            else
            {
                result += QStringLiteral("   ");
            }
        }

        if (showAddressAndAscii)
        {
            result += QStringLiteral("| ");
            for (int j = 0; j < lineLength; ++j)
            {
                const uchar value = uchar(data.at(line + j));
                result += (value >= 32 && value <= 126) ? QChar(value) : QChar(u'.');
            }
        }
        result += QStringLiteral("\r\n");
    }

    result += QStringLiteral("\r\n");
    return result;
}

ResourcePreview RawDetect::Get(const ResourceEntry& entry)
{
    if (entry.data.size() >= 4) {
        const QByteArray signature = entry.data.left(4);
        if (signature == QByteArrayLiteral("FMIM"))
            return FMIM::preview(entry);
    }

    ResourcePreview preview;
    const QByteArray& resData = entry.data;
    if (resData.isEmpty())
        return preview;

    // Fonts have a sufficiently distinctive header and must be tried before
    // generic binary/image detection.
    if (RT_FONT::LooksLikeOs2Fnt(resData))
    {
        ResourceEntry font = entry;
        font.type = QStringLiteral("RT_FONT");
        font.isOs2 = true;
        preview = RT_FONT::preview(font);
        if (!preview.images.isEmpty() || !preview.text.isEmpty())
            return preview;
        // A recognized binary font must not fall through to strict UTF-8
        // probing, which only produces a misleading decoding error. The GUI
        // falls back to the same hexadecimal/ASCII viewer used by C#.
        if (preview.images.isEmpty() && preview.text.isEmpty())
            preview.text = DumpRaw(resData, true);
        preview.rawDump = true;
        return preview;
    }

    if (RT_FONT::LooksLikeWindowsFnt(resData))
    {
        ResourceEntry font = entry;
        font.type = QStringLiteral("RT_FONT");
        preview = RT_FONT::preview(font);
        if (!preview.images.isEmpty() || !preview.text.isEmpty())
            return preview;
    }

    // Windows Media Player stores its internal skins as binary WSZ trees.
    // Keep this before the generic image and text probes, as in C#.
    QString wszText;
    if (WszDecoder::TryDecode(resData, wszText))
    {
        preview.text = wszText;
        return preview;
    }


    // Complete image files (PNG, JPEG, GIF, BMP, TIFF, ICO/CUR, WMF/EMF)
    // can be decoded directly by Qt without knowing the resource type name.
    if (hasStandardImageSignature(resData))
    {
        const QImage standardImage = QImage::fromData(resData);
        if (!standardImage.isNull())
        {
            preview.images.push_back(standardImage);
            return preview;
        }
    }

    // Windows 1.x/2.x monochrome icon/cursor resources have a
    // 14-byte header followed by AND and XOR masks, not a DIB header.
    if (Win12MonochromeResource::LooksLike(resData))
    {
        Img legacyImage;
        const quint8 figure = quint8(resData[0]);
        if (Win12MonochromeResource::TryDecode(resData, figure, legacyImage) &&
            !legacyImage.Bitmap.isNull())
        {
            preview.images.push_back(legacyImage.Bitmap);
            return preview;
        }
    }

    // A cursor resource is a DIB preceded by hotspot X/Y values.
    if (isDibHeader(resData, 4))
    {
        ResourceEntry cursor = entry;
        cursor.type = QStringLiteral("RT_CURSOR");
        preview = RT_CURSOR::preview(cursor);
        if (!preview.images.isEmpty())
            return preview;
    }

    // RT_BITMAP already handles Windows DIBs, OS/2 bitmap arrays and
    // OS/2 IC/CI/CP/PT pointer/icon formats.
    if (isDibHeader(resData, 0) || hasBitmapResourceSignature(resData))
    {
        ResourceEntry bitmap = entry;
        bitmap.type = QStringLiteral("RT_BITMAP");
        preview = RT_BITMAP::preview(bitmap);
        if (!preview.images.isEmpty())
            return preview;
    }

    // Text remains a string, so the UI can use its existing text viewer.
    const QString text = tryDecodeText(resData);
    if (!text.isNull())
    {
        preview.text = text;
        return preview;
    }

    preview.text = DumpRaw(resData, true);
    preview.rawDump = true;
    return preview;
}

} // namespace resources
} // namespace peare
