#include "peare/peare_decoder.h"
#include "decoder/ResourceDecoder.h"
#include "decoder/DecoderRoute.h"
#include "decoder/resources/RT_FONT/RT_FONT.h"
#include "decoder/resources/RT_FONT/OS2_RT_FONT.h"
#include "decoder/resources/WAD/WadImageDecoder.h"
#include "font/BitmapFont.h"
#include "font/SfntFont.h"
#include "font/AfmFont.h"
#include "font/PfmFont.h"
#include "font/MacFondFont.h"
#include "font/SnfFont.h"
#include "font/SvgFont.h"
#include "font/SfnFont.h"
#include "font/ShpFont.h"
#include "font/Type1Font.h"
#include "font/Type3Font.h"
#include <QByteArray>
#include <QBuffer>
#include <QImage>
#include <QSet>
#include <QString>
#include <QTextCodec>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
namespace {
peare_status copyBytes(const char* data, size_t length, peare_blob* out) {
    if (!out) return PEARE_STATUS_INVALID_ARGUMENT;
    out->bytes = nullptr;
    out->length = 0;
    if (!length) return PEARE_STATUS_OK;
    auto* p = static_cast<uint8_t*>(std::malloc(length));
    if (!p) return PEARE_STATUS_ALLOCATION_FAILED;
    std::memcpy(p, data, length);
    out->bytes = p;
    out->length = length;
    return PEARE_STATUS_OK;
}
peare_status copyByteArray(const QByteArray& b, peare_blob* out) {
    return copyBytes(b.constData(), static_cast<size_t>(b.size()), out);
}
peare_status copyUtf8(const QString& s, peare_blob* out) {
    return copyByteArray(s.toUtf8(), out);
}
peare::ResourcePlatform platformFromText(const char* platformUtf8) {
    if (!platformUtf8 || !*platformUtf8) return peare::ResourcePlatform::Unknown;
    const QString value = QString::fromUtf8(platformUtf8).trimmed().toUpper();
    if (value == QStringLiteral("OS/2") || value == QStringLiteral("OS2"))
        return peare::ResourcePlatform::Os2;
    if (value == QStringLiteral("WINDOWS") || value == QStringLiteral("WIN"))
        return peare::ResourcePlatform::Windows;
    return peare::ResourcePlatform::Other;
}

QString normalizedContextText(const char* text) {
    return text ? QString::fromUtf8(text).trimmed() : QString();
}

peare_status loadResource(const uint8_t* payload, size_t payloadLength, const char* platformUtf8,
                          const char* resourceTypeUtf8, peare::OpenedResource* out) {
    if (!out) return PEARE_STATUS_INVALID_ARGUMENT;
    *out = {};
    if ((!payload && payloadLength != 0) || payloadLength > static_cast<size_t>(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    out->resourceIndex = 0;
    if (payloadLength != 0) {
        out->payload =
            QByteArray(reinterpret_cast<const char*>(payload), static_cast<int>(payloadLength));
    }
    out->context.platform = platformFromText(platformUtf8);
    out->context.type = normalizedContextText(resourceTypeUtf8);
    out->context.dataSize = static_cast<quint64>(payloadLength);
    return PEARE_STATUS_OK;
}

void freeLocalBlob(peare_blob* blob) {
    if (!blob) return;
    std::free(blob->bytes);
    blob->bytes = nullptr;
    blob->length = 0;
}

peare_container_format toCFormat(peare::ModuleFormat f) {
    switch (f) {
    case peare::ModuleFormat::DosMZ:
        return PEARE_CONTAINER_DOS_MZ;
    case peare::ModuleFormat::PE:
        return PEARE_CONTAINER_PE;
    case peare::ModuleFormat::NE:
        return PEARE_CONTAINER_NE;
    case peare::ModuleFormat::LE:
        return PEARE_CONTAINER_LE;
    case peare::ModuleFormat::LX:
        return PEARE_CONTAINER_LX;
    case peare::ModuleFormat::XEX:
        return PEARE_CONTAINER_XEX;
    case peare::ModuleFormat::XBE:
        return PEARE_CONTAINER_XBE;
    case peare::ModuleFormat::XUIZ:
        return PEARE_CONTAINER_XUIZ;
    case peare::ModuleFormat::LIVE_PIRS:
        return PEARE_CONTAINER_LIVE_PIRS;
    case peare::ModuleFormat::CON:
        return PEARE_CONTAINER_CON;
    case peare::ModuleFormat::OS2_PACK:
        return PEARE_CONTAINER_OS2_PACK;
    case peare::ModuleFormat::SZDD:
        return PEARE_CONTAINER_SZDD;
    case peare::ModuleFormat::SIEMENS_IMG:
        return PEARE_CONTAINER_SIEMENS_IMG;
    case peare::ModuleFormat::SIEMENS_ADONIS:
        return PEARE_CONTAINER_SIEMENS_ADONIS;
    case peare::ModuleFormat::SIEMENS_FWF:
        return PEARE_CONTAINER_SIEMENS_FWF;
    case peare::ModuleFormat::SIEMENS_FSF:
        return PEARE_CONTAINER_SIEMENS_FSF;
    default:
        return PEARE_CONTAINER_UNKNOWN;
    }
}
bool isBitmapItemInfoId(peare_info_id infoId) noexcept {
    switch (infoId) {
    case PEARE_INFO_BITMAP_WIDTH:
    case PEARE_INFO_BITMAP_HEIGHT:
    case PEARE_INFO_BITMAP_BPP:
    case PEARE_INFO_BITMAP_COMPRESSION:
        return true;
    default:
        return false;
    }
}

bool isTextResourceKind(peare::ResourceKind kind) noexcept {
    switch (kind) {
    case peare::ResourceKind::StringTable:
    case peare::ResourceKind::DialogInclude:
    case peare::ResourceKind::MessageTable:
    case peare::ResourceKind::Version:
    case peare::ResourceKind::Accelerator:
    case peare::ResourceKind::AccelTable:
    case peare::ResourceKind::FontDirectory:
    case peare::ResourceKind::Font:
    case peare::ResourceKind::DisplayInfo:
    case peare::ResourceKind::HelpTable:
    case peare::ResourceKind::HelpSubTable:
    case peare::ResourceKind::NameTable:
    case peare::ResourceKind::Menu:
    case peare::ResourceKind::Dialog:
        return true;
    default:
        return false;
    }
}

peare_platform toCPlatform(peare::ResourcePlatform platform) {
    switch (platform) {
    case peare::ResourcePlatform::Windows:
        return PEARE_PLATFORM_WINDOWS;
    case peare::ResourcePlatform::Os2:
        return PEARE_PLATFORM_OS2;
    case peare::ResourcePlatform::Other:
        return PEARE_PLATFORM_OTHER;
    case peare::ResourcePlatform::Unknown:
        return PEARE_PLATFORM_UNKNOWN;
    }
    return PEARE_PLATFORM_UNKNOWN;
}

void clearContext(peare_resource_context* context) {
    if (context) std::memset(context, 0, sizeof(*context));
}

void clearValue(peare_value* value) {
    if (value) std::memset(value, 0, sizeof(*value));
}

peare_status setUnsignedValue(uint64_t number, peare_value* out) {
    clearValue(out);
    out->type = PEARE_VALUE_UINT64;
    out->value.unsigned_value = number;
    return PEARE_STATUS_OK;
}

peare_status setUtf8Value(const QString& text, peare_value* out);

peare_status setBooleanValue(bool value, peare_value* out) {
    clearValue(out);
    out->type = PEARE_VALUE_BOOL;
    out->value.boolean_value = value ? 1 : 0;
    return PEARE_STATUS_OK;
}

quint16 readLe16(const QByteArray& data, int offset) {
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 readLe32(const QByteArray& data, int offset) {
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString readAnsiZ(const QByteArray& data, int offset) {
    if (offset <= 0 || offset >= data.size()) return {};
    int end = offset;
    while (end < data.size() && data.at(end) != '\0') ++end;
    return QTextCodec::codecForLocale()->toUnicode(data.constData() + offset, end - offset);
}

QString readFixedText(const QByteArray& data, int offset, int length) {
    if (offset < 0 || length <= 0 || offset + length > data.size()) return {};
    QByteArray raw = data.mid(offset, length);
    const int zero = raw.indexOf('\0');
    if (zero >= 0) raw.truncate(zero);
    return QString::fromLatin1(raw).trimmed();
}

peare_status getWindowsFontInfo(const QByteArray& data, peare_info_id infoId, peare_value* out) {
    if (data.size() < 118) return PEARE_STATUS_DECODE_FAILED;
    const quint16 version = readLe16(data, 0);
    if (version != 0x0100 && version != 0x0200 && version != 0x0300)
        return PEARE_STATUS_DECODE_FAILED;
    const quint16 points = readLe16(data, 68);
    const quint16 ascent = readLe16(data, 74);
    const quint16 pixWidth = readLe16(data, 86);
    const quint16 pixHeight = readLe16(data, 88);
    const quint8 pitchAndFamily = quint8(data.at(90));
    const quint16 averageWidth = readLe16(data, 91);
    const quint16 maximumWidth = readLe16(data, 93);
    const quint8 first = quint8(data.at(95));
    const quint8 last = quint8(data.at(96));
    const quint8 defaultOffset = quint8(data.at(97));
    const quint8 breakOffset = quint8(data.at(98));
    switch (infoId) {
    case PEARE_INFO_FONT_FACE_NAME:
        return setUtf8Value(readAnsiZ(data, int(readLe32(data, 105))), out);
    case PEARE_INFO_FONT_DEVICE_NAME:
        return setUtf8Value(readAnsiZ(data, int(readLe32(data, 101))), out);
    case PEARE_INFO_FONT_COPYRIGHT:
        return setUtf8Value(readFixedText(data, 6, 60), out);
    case PEARE_INFO_FONT_CODEPAGE:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_FIRST_CHARACTER:
        return setUnsignedValue(first, out);
    case PEARE_INFO_FONT_LAST_CHARACTER:
        return setUnsignedValue(last, out);
    case PEARE_INFO_FONT_DEFAULT_CHARACTER:
        return setUnsignedValue(unsigned(first) + defaultOffset, out);
    case PEARE_INFO_FONT_BREAK_CHARACTER:
        return setUnsignedValue(unsigned(first) + breakOffset, out);
    case PEARE_INFO_FONT_FIXED_WIDTH:
        return setBooleanValue((pitchAndFamily & 1u) == 0u, out);
    case PEARE_INFO_FONT_POINT_SIZE:
        return setUnsignedValue(points, out);
    case PEARE_INFO_FONT_PIXEL_HEIGHT:
        return setUnsignedValue(pixHeight, out);
    case PEARE_INFO_FONT_ASCENT:
        return setUnsignedValue(ascent, out);
    case PEARE_INFO_FONT_DESCENT:
        return setUnsignedValue(pixHeight > ascent ? pixHeight - ascent : 0, out);
    case PEARE_INFO_FONT_AVERAGE_WIDTH:
        return setUnsignedValue(averageWidth, out);
    case PEARE_INFO_FONT_MAXIMUM_WIDTH:
        return setUnsignedValue(maximumWidth, out);
    case PEARE_INFO_FONT_GLYPH_COUNT:
        return last >= first ? setUnsignedValue(unsigned(last) - first + 1u, out)
                             : PEARE_STATUS_DECODE_FAILED;
    default:
        return PEARE_STATUS_NOT_APPLICABLE;
    }
}

peare_status getOs2FontInfo(const QByteArray& data, peare_info_id infoId, peare_value* out) {
    const int marker = data.indexOf(QByteArrayLiteral("OS/2 FONT"));
    const int metrics = marker >= 8 && marker - 8 <= 16 ? marker - 8 + 20 : 20;
    if (metrics < 0 || metrics + 136 > data.size()) return PEARE_STATUS_DECODE_FAILED;
    const quint16 codePage = readLe16(data, metrics + 74);
    const quint16 first = readLe16(data, metrics + 114);
    const quint16 last = readLe16(data, metrics + 116);
    const quint16 def = readLe16(data, metrics + 118);
    const quint16 brk = readLe16(data, metrics + 120);
    const quint16 pointTenths = readLe16(data, metrics + 122);
    const quint16 ascent = readLe16(data, metrics + 80);
    const quint16 descent = readLe16(data, metrics + 82);
    switch (infoId) {
    case PEARE_INFO_FONT_FACE_NAME:
        return setUtf8Value(readFixedText(data, metrics + 40, 32), out);
    case PEARE_INFO_FONT_DEVICE_NAME:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_COPYRIGHT:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_CODEPAGE:
        return codePage ? setUnsignedValue(codePage, out) : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_FIRST_CHARACTER:
        return setUnsignedValue(first, out);
    case PEARE_INFO_FONT_LAST_CHARACTER:
        return setUnsignedValue(last, out);
    case PEARE_INFO_FONT_DEFAULT_CHARACTER:
        return setUnsignedValue(def, out);
    case PEARE_INFO_FONT_BREAK_CHARACTER:
        return setUnsignedValue(brk, out);
    case PEARE_INFO_FONT_FIXED_WIDTH:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_POINT_SIZE:
        return pointTenths ? setUnsignedValue((unsigned(pointTenths) + 5u) / 10u, out)
                           : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_PIXEL_HEIGHT:
        return setUnsignedValue(unsigned(ascent) + descent, out);
    case PEARE_INFO_FONT_ASCENT:
        return setUnsignedValue(ascent, out);
    case PEARE_INFO_FONT_DESCENT:
        return setUnsignedValue(descent, out);
    case PEARE_INFO_FONT_AVERAGE_WIDTH:
    case PEARE_INFO_FONT_MAXIMUM_WIDTH:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_FONT_GLYPH_COUNT:
        return last >= first ? setUnsignedValue(unsigned(last) - first + 1u, out)
                             : PEARE_STATUS_DECODE_FAILED;
    default:
        return PEARE_STATUS_NOT_APPLICABLE;
    }
}

peare_status getFontInfo(const peare::OpenedResource& resource, peare_info_id infoId,
                         peare_value* out) {
    peare::font::Type1Font type1Font;
    if (peare::font::parseType3Font(resource.payload, &type1Font, nullptr) ||
        peare::font::parseType1Font(resource.payload, &type1Font, nullptr)) {
        double widthTotal = 0;
        double maximumWidth = 0;
        for (const peare::font::Type1Glyph& glyph : type1Font.glyphs) {
            widthTotal += glyph.advance;
            maximumWidth = qMax(maximumWidth, glyph.advance);
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return type1Font.name.isEmpty() ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                            : setUtf8Value(type1Font.name, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(type1Font.glyphs.size()), out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(qMax(1.0, type1Font.ascent - type1Font.descent)), out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(quint64(qMax(0.0, type1Font.ascent)), out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(quint64(qMax(0.0, -type1Font.descent)), out);
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(type1Font.glyphs.isEmpty() ? 0
                : quint64(widthTotal / type1Font.glyphs.size() * 16 / type1Font.unitsPerEm), out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(quint64(maximumWidth * 16 / type1Font.unitsPerEm), out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(type1Font.glyphs.isEmpty() ||
                std::all_of(type1Font.glyphs.cbegin(), type1Font.glyphs.cend(),
                    [&type1Font](const peare::font::Type1Glyph& glyph) {
                        return glyph.advance == type1Font.glyphs.first().advance;
                    }), out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::SnfFont snfFont;
    if (peare::font::parseSnf(resource.payload, &snfFont, nullptr)) {
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return snfFont.name.isEmpty() ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                          : setUtf8Value(snfFont.name, out);
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return setUnsignedValue(snfFont.firstCode, out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return setUnsignedValue(snfFont.lastCode, out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::ShpFont shpFont;
    if (peare::font::parseShpFont(resource.payload, &shpFont, nullptr)) {
        quint32 first = 0xffffffffu;
        quint32 last = 0;
        for (const peare::font::ShpGlyph& glyph : shpFont.glyphs) {
            first = qMin(first, quint32(glyph.code));
            last = qMax(last, quint32(glyph.code));
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(shpFont.name, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(shpFont.glyphs.size()), out);
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return setUnsignedValue(first, out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return setUnsignedValue(last, out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(shpFont.above) + shpFont.below, out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(shpFont.above, out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(shpFont.below, out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::SfnFont sfnFont;
    if (peare::font::parseSfnFont(resource.payload, &sfnFont, nullptr)) {
        quint32 first = 0xffffffffu;
        quint32 last = 0;
        quint64 widthTotal = 0;
        quint32 maximumWidth = 0;
        bool fixedWidth = !sfnFont.glyphs.isEmpty();
        for (int i = 0; i < sfnFont.glyphs.size(); ++i) {
            const peare::font::SfnGlyph& glyph = sfnFont.glyphs.at(i);
            first = qMin(first, glyph.codepoint);
            last = qMax(last, glyph.codepoint);
            widthTotal += glyph.advanceX;
            maximumWidth = qMax(maximumWidth, quint32(glyph.advanceX));
            if (i && glyph.advanceX != sfnFont.glyphs.first().advanceX) fixedWidth = false;
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(sfnFont.familyName, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(sfnFont.glyphs.size()), out);
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return setUnsignedValue(first, out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return setUnsignedValue(last, out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(fixedWidth, out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(sfnFont.height, out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(sfnFont.baseline, out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(sfnFont.height - sfnFont.baseline, out);
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(sfnFont.glyphs.isEmpty() ? 0
                : widthTotal / quint64(sfnFont.glyphs.size()), out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(maximumWidth, out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::SvgFont svgFont;
    if (peare::font::parseSvgFont(resource.payload, &svgFont, nullptr)) {
        double widthTotal = 0;
        double maximumWidth = 0;
        bool fixedWidth = true;
        for (int i = 0; i < svgFont.glyphs.size(); ++i) {
            const double width = svgFont.glyphs.at(i).advance;
            widthTotal += width;
            maximumWidth = qMax(maximumWidth, width);
            if (i && width != svgFont.glyphs.first().advance) fixedWidth = false;
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(svgFont.familyName, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(svgFont.glyphs.size()), out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(qMax(1, qRound(svgFont.ascent - svgFont.descent))), out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(quint64(qMax(0, qRound(svgFont.ascent))), out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(quint64(qMax(0, qRound(-svgFont.descent))), out);
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(svgFont.glyphs.isEmpty() ? 0
                : quint64(qRound(widthTotal / svgFont.glyphs.size())), out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(quint64(qRound(maximumWidth)), out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(fixedWidth, out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::AfmFont afmFont;
    if (peare::font::parseAfm(resource.payload, &afmFont, nullptr) ||
        peare::font::parsePfm(resource.payload, &afmFont, nullptr)) {
        int first = -1;
        int last = -1;
        double widthTotal = 0;
        double maximumWidth = 0;
        bool fixedWidth = true;
        for (int i = 0; i < afmFont.glyphs.size(); ++i) {
            const peare::font::AfmGlyph& glyph = afmFont.glyphs.at(i);
            if (glyph.code >= 0) {
                first = first < 0 ? glyph.code : qMin(first, glyph.code);
                last = qMax(last, glyph.code);
            }
            widthTotal += glyph.advance;
            maximumWidth = qMax(maximumWidth, glyph.advance);
            if (i && glyph.advance != afmFont.glyphs.first().advance) fixedWidth = false;
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(afmFont.familyName, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(afmFont.glyphs.size()), out);
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return first < 0 ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                             : setUnsignedValue(quint64(first), out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return last < 0 ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                            : setUnsignedValue(quint64(last), out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(fixedWidth, out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(qMax(0, qRound(afmFont.ascender - afmFont.descender))), out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(quint64(qMax(0, qRound(afmFont.ascender))), out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(quint64(qMax(0, qRound(-afmFont.descender))), out);
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(afmFont.glyphs.isEmpty() ? 0
                : quint64(qMax(0, qRound(widthTotal / afmFont.glyphs.size()))), out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(quint64(qMax(0, qRound(maximumWidth))), out);
        case PEARE_INFO_FONT_DEVICE_NAME:
        case PEARE_INFO_FONT_COPYRIGHT:
        case PEARE_INFO_FONT_CODEPAGE:
        case PEARE_INFO_FONT_DEFAULT_CHARACTER:
        case PEARE_INFO_FONT_BREAK_CHARACTER:
        case PEARE_INFO_FONT_POINT_SIZE:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        default:
            return PEARE_STATUS_NOT_APPLICABLE;
        }
    }
    peare::font::BitmapFont bitmapFont;
    if (peare::font::parse(resource.payload, &bitmapFont, nullptr)) {
        quint32 first = 0xffffffffu;
        quint32 last = 0;
        quint64 widthTotal = 0;
        quint32 maximumWidth = 0;
        for (const peare::font::Glyph& glyph : bitmapFont.glyphs) {
            if (glyph.codepoint != 0xffffffffu) {
                first = qMin(first, glyph.codepoint);
                last = qMax(last, glyph.codepoint);
            }
            widthTotal += quint64(glyph.advance);
            maximumWidth = qMax(maximumWidth, quint32(glyph.advance));
        }
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(bitmapFont.name, out);
        case PEARE_INFO_FONT_DEVICE_NAME:
        case PEARE_INFO_FONT_COPYRIGHT:
        case PEARE_INFO_FONT_CODEPAGE:
        case PEARE_INFO_FONT_DEFAULT_CHARACTER:
        case PEARE_INFO_FONT_BREAK_CHARACTER:
        case PEARE_INFO_FONT_POINT_SIZE:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return first == 0xffffffffu ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                        : setUnsignedValue(first, out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return first == 0xffffffffu ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                        : setUnsignedValue(last, out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(bitmapFont.fixedWidth, out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(bitmapFont.height), out);
        case PEARE_INFO_FONT_ASCENT:
            return setUnsignedValue(quint64(bitmapFont.ascent), out);
        case PEARE_INFO_FONT_DESCENT:
            return setUnsignedValue(quint64(bitmapFont.descent), out);
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(bitmapFont.glyphs.isEmpty() ? 0
                : widthTotal / quint64(bitmapFont.glyphs.size()), out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(maximumWidth, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(quint64(bitmapFont.glyphs.size()), out);
        default:
            return PEARE_STATUS_NOT_APPLICABLE;
        }
    }
    peare::font::MacFondFont fondFont;
    if (peare::font::parseMacFond(resource.payload, &fondFont, nullptr)) {
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        case PEARE_INFO_FONT_FIRST_CHARACTER:
            return setUnsignedValue(fondFont.firstCharacter, out);
        case PEARE_INFO_FONT_LAST_CHARACTER:
            return setUnsignedValue(fondFont.lastCharacter, out);
        default:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        }
    }
    peare::font::SfntFont outlineFont;
    if (peare::font::parseSfnt(resource.payload, &outlineFont, nullptr)) {
        switch (infoId) {
        case PEARE_INFO_FONT_FACE_NAME:
            return setUtf8Value(outlineFont.familyName, out);
        case PEARE_INFO_FONT_FIXED_WIDTH:
            return setBooleanValue(outlineFont.fixedWidth, out);
        case PEARE_INFO_FONT_PIXEL_HEIGHT:
            return setUnsignedValue(quint64(qMax(1, int(outlineFont.ascent) + int(outlineFont.descent))), out);
        case PEARE_INFO_FONT_ASCENT:
            return outlineFont.ascent > 0 ? setUnsignedValue(quint64(outlineFont.ascent), out)
                                          : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        case PEARE_INFO_FONT_DESCENT:
            return outlineFont.descent > 0 ? setUnsignedValue(quint64(outlineFont.descent), out)
                                           : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        case PEARE_INFO_FONT_AVERAGE_WIDTH:
            return setUnsignedValue(quint64(outlineFont.averageAdvance) * 16 / outlineFont.unitsPerEm, out);
        case PEARE_INFO_FONT_MAXIMUM_WIDTH:
            return setUnsignedValue(quint64(outlineFont.maximumAdvance) * 16 / outlineFont.unitsPerEm, out);
        case PEARE_INFO_FONT_GLYPH_COUNT:
            return setUnsignedValue(outlineFont.glyphCount, out);
        case PEARE_INFO_FONT_DEVICE_NAME:
        case PEARE_INFO_FONT_COPYRIGHT:
        case PEARE_INFO_FONT_CODEPAGE:
        case PEARE_INFO_FONT_FIRST_CHARACTER:
        case PEARE_INFO_FONT_LAST_CHARACTER:
        case PEARE_INFO_FONT_DEFAULT_CHARACTER:
        case PEARE_INFO_FONT_BREAK_CHARACTER:
        case PEARE_INFO_FONT_POINT_SIZE:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        default:
            return PEARE_STATUS_NOT_APPLICABLE;
        }
    }
    if (resource.context.platform == peare::ResourcePlatform::Os2)
        return getOs2FontInfo(resource.payload, infoId, out);
    return getWindowsFontInfo(resource.payload, infoId, out);
}

peare_status getPointerHotspot(const peare::OpenedResource& resource, peare_info_id infoId,
                               peare_value* out) {
    if (resource.payload.size() < 4) return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    if (resource.context.platform == peare::ResourcePlatform::Os2)
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    if (infoId == PEARE_INFO_POINTER_HOTSPOT_X)
        return setUnsignedValue(readLe16(resource.payload, 0), out);
    if (infoId == PEARE_INFO_POINTER_HOTSPOT_Y)
        return setUnsignedValue(readLe16(resource.payload, 2), out);
    return PEARE_STATUS_NOT_APPLICABLE;
}

peare_status setUtf8Value(const QString& text, peare_value* out) {
    clearValue(out);
    const QByteArray utf8 = text.toUtf8();
    if (utf8.isEmpty()) {
        out->type = PEARE_VALUE_UTF8;
        return PEARE_STATUS_OK;
    }
    auto* copy = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(utf8.size())));
    if (!copy) return PEARE_STATUS_ALLOCATION_FAILED;
    std::memcpy(copy, utf8.constData(), static_cast<size_t>(utf8.size()));
    out->type = PEARE_VALUE_UTF8;
    out->value.buffer.data = copy;
    out->value.buffer.length = static_cast<size_t>(utf8.size());
    return PEARE_STATUS_OK;
}

void clearBlobArray(peare_blob_array* array) {
    if (!array) return;
    array->items = nullptr;
    array->count = 0;
}

peare_status allocateBlobArray(size_t count, peare_blob_array* out) {
    clearBlobArray(out);
    if (count == 0) return PEARE_STATUS_OK;
    if (count > SIZE_MAX / sizeof(peare_blob)) return PEARE_STATUS_ALLOCATION_FAILED;
    auto* items = static_cast<peare_blob*>(std::calloc(count, sizeof(peare_blob)));
    if (!items) return PEARE_STATUS_ALLOCATION_FAILED;
    out->items = items;
    out->count = count;
    return PEARE_STATUS_OK;
}

bool prepareFontResource(peare::OpenedResource* resource) {
    if (!resource) return false;
    QVector<peare::font::SfntFont> sfntFaces;
    if (peare::font::parseTtcCollection(resource->payload, &sfntFaces, nullptr) &&
        !sfntFaces.isEmpty())
        resource->payload = sfntFaces.first().sfntBytes;
    QVector<QByteArray> sfnFaces;
    if (peare::font::parseSfnCollection(resource->payload, &sfnFaces, nullptr) &&
        !sfnFaces.isEmpty())
        resource->payload = sfnFaces.first();

    const QString type = resource->context.type.trimmed().toUpper();
    if (type == QStringLiteral("RT_FONT") || type == QStringLiteral("FONT")) return true;
    if (type == QStringLiteral("RT_FONTDIR") || type == QStringLiteral("FONTDIR")) return false;

    if (peare::resources::RT_FONT::LooksLikeOs2Fnt(resource->payload)) {
        resource->context.type = QStringLiteral("RT_FONT");
        resource->context.platform = peare::ResourcePlatform::Os2;
        return true;
    }
    if (peare::resources::RT_FONT::LooksLikeWindowsFnt(resource->payload)) {
        resource->context.type = QStringLiteral("RT_FONT");
        if (resource->context.platform == peare::ResourcePlatform::Unknown)
            resource->context.platform = peare::ResourcePlatform::Windows;
        return true;
    }
    if (peare::resources::RT_FONT::LooksLikeFont(resource->payload)) {
        resource->context.type = QStringLiteral("RT_FONT");
        return true;
    }
    peare::font::MacFondFont fond;
    if (peare::font::parseMacFond(resource->payload, &fond, nullptr)) {
        resource->context.type = QStringLiteral("RT_FONT");
        return true;
    }
    peare::font::BitmapFont bitmap;
    if (peare::font::parse(resource->payload, &bitmap, nullptr) &&
        bitmap.format == peare::font::Format::MacNfnt) {
        resource->context.type = QStringLiteral("RT_FONT");
        return true;
    }
    return false;
}

bool isFontResource(peare::OpenedResource resource) {
    return prepareFontResource(&resource);
}

QRgb fromRgba32(uint32_t rgba) {
    return qRgba((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu);
}

} // namespace
extern "C" {
peare_status peare_decode_images(const uint8_t* payload, size_t payload_length,
                                 const char* platform_utf8, const char* resource_type_utf8,
                                 peare_blob_array* out_pngs) {
    if (!out_pngs) return PEARE_STATUS_INVALID_ARGUMENT;
    clearBlobArray(out_pngs);
    peare::OpenedResource opened;
    peare_status load_status =
        loadResource(payload, payload_length, platform_utf8, resource_type_utf8, &opened);
    if (load_status != PEARE_STATUS_OK) return load_status;

    try {
        if (isFontResource(opened)) return PEARE_STATUS_NOT_APPLICABLE;
        const peare::ResourcePreview preview = peare::ResourceDecoder::preview(opened);
        if (!preview.error.isEmpty()) return PEARE_STATUS_DECODE_FAILED;
        // Fonts are rendered only through peare_font_render(); preview images are a GUI concern.
        if (preview.images.isEmpty()) return PEARE_STATUS_NOT_APPLICABLE;

        peare_status status =
            allocateBlobArray(static_cast<size_t>(preview.images.size()), out_pngs);
        if (status != PEARE_STATUS_OK) return status;

        for (int i = 0; i < preview.images.size(); ++i) {
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !preview.images.at(i).save(&buffer, "PNG")) {
                peare_blob_array_free(out_pngs);
                return PEARE_STATUS_DECODE_FAILED;
            }
            status = copyByteArray(png, &out_pngs->items[static_cast<size_t>(i)]);
            if (status != PEARE_STATUS_OK) {
                peare_blob_array_free(out_pngs);
                return status;
            }
        }
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_decode_images_with_resolver(const uint8_t* payload, size_t payload_length,
                                               const char* platform_utf8,
                                               const char* resource_type_utf8,
                                               peare_resource_resolver_fn resolver,
                                               void* resolver_user_data,
                                               peare_blob_array* out_pngs) {
    if (!out_pngs || !resolver || (!payload && payload_length) || payload_length > size_t(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    clearBlobArray(out_pngs);
    const QString type = normalizedContextText(resource_type_utf8);
    if (type != QLatin1String("WAD_DOOM_PATCH") && type != QLatin1String("WAD_DOOM_SPRITE") &&
        type != QLatin1String("WAD_TEXTUREX"))
        return peare_decode_images(payload, payload_length, platform_utf8, resource_type_utf8, out_pngs);
    const QByteArray data(reinterpret_cast<const char*>(payload), int(payload_length));
    const peare::resources::WadLumpResolver findLump = [resolver, resolver_user_data](
        const QString& type, const QString& name) {
        const QByteArray typeUtf8 = type.toUtf8();
        const QByteArray nameUtf8 = name.toUtf8();
        const uint8_t* bytes = nullptr;
        size_t length = 0;
        if (resolver(resolver_user_data, typeUtf8.constData(), size_t(typeUtf8.size()),
                     nameUtf8.constData(), size_t(nameUtf8.size()), &bytes, &length) != PEARE_STATUS_OK ||
            (!bytes && length) || length > size_t(INT_MAX))
            return QByteArray();
        return QByteArray(reinterpret_cast<const char*>(bytes), int(length));
    };
    try {
        QString error;
        const QVector<QImage> images = peare::resources::WadImageDecoder::decode(data, type, findLump, &error);
        if (images.isEmpty()) return error.isEmpty() ? PEARE_STATUS_NOT_APPLICABLE : PEARE_STATUS_DECODE_FAILED;
        peare_status status = allocateBlobArray(size_t(images.size()), out_pngs);
        if (status != PEARE_STATUS_OK) return status;
        for (int i = 0; i < images.size(); ++i) {
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !images[i].save(&buffer, "PNG")) {
                peare_blob_array_free(out_pngs);
                return PEARE_STATUS_DECODE_FAILED;
            }
            status = copyByteArray(png, &out_pngs->items[size_t(i)]);
            if (status != PEARE_STATUS_OK) {
                peare_blob_array_free(out_pngs);
                return status;
            }
        }
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_decode_texts(const uint8_t* payload, size_t payload_length,
                                const char* platform_utf8, const char* resource_type_utf8,
                                peare_blob_array* out_texts) {
    if (!out_texts) return PEARE_STATUS_INVALID_ARGUMENT;
    clearBlobArray(out_texts);
    peare::OpenedResource opened;
    peare_status load_status =
        loadResource(payload, payload_length, platform_utf8, resource_type_utf8, &opened);
    if (load_status != PEARE_STATUS_OK) return load_status;

    try {
        const peare::ResourcePreview preview = peare::ResourceDecoder::preview(opened);
        if (!preview.error.isEmpty()) return PEARE_STATUS_DECODE_FAILED;
        if (preview.rawDump || preview.text.isEmpty()) return PEARE_STATUS_NOT_APPLICABLE;

        peare_status status = allocateBlobArray(1, out_texts);
        if (status != PEARE_STATUS_OK) return status;
        status = copyUtf8(preview.text, &out_texts->items[0]);
        if (status != PEARE_STATUS_OK) peare_blob_array_free(out_texts);
        return status;
    } catch (const std::bad_alloc&) {
        peare_blob_array_free(out_texts);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        peare_blob_array_free(out_texts);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_font_render_with_codepage(const uint8_t* payload, size_t payload_length,
                               const char* platform_utf8, const char* resource_type_utf8,
                               uint32_t code_page, const char* text_utf8,
                               const peare_font_render_options* options, peare_blob* out_png) {
    if (!out_png) return PEARE_STATUS_INVALID_ARGUMENT;
    out_png->bytes = nullptr;
    out_png->length = 0;
    peare::OpenedResource opened;
    peare_status load_status =
        loadResource(payload, payload_length, platform_utf8, resource_type_utf8, &opened);
    if (load_status != PEARE_STATUS_OK) return load_status;
    if (code_page) opened.context.codePage = code_page;
    if (!text_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    if (!prepareFontResource(&opened)) return PEARE_STATUS_NOT_APPLICABLE;

    uint32_t scale = 1;
    uint32_t padding = 8;
    uint32_t foreground = 0x000000FFu;
    uint32_t background = 0xFFFFFFFFu;
    if (options) {
        scale = options->scale;
        padding = options->padding;
        foreground = options->foreground_rgba;
        background = options->background_rgba;
    }
    if (scale == 0 || scale > static_cast<uint32_t>(INT_MAX) ||
        padding > static_cast<uint32_t>(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;

    try {
        QString error;
        const QImage image = peare::ResourceDecoder::renderFont(
            opened, QString::fromUtf8(text_utf8), static_cast<int>(scale),
            static_cast<int>(padding), fromRgba32(foreground), fromRgba32(background), &error);
        if (image.isNull()) return PEARE_STATUS_DECODE_FAILED;

        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return PEARE_STATUS_DECODE_FAILED;
        return copyByteArray(png, out_png);
    } catch (const std::bad_alloc&) {
        freeLocalBlob(out_png);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        freeLocalBlob(out_png);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_font_supports_text_with_codepage(
    const uint8_t* payload, size_t payload_length, const char* platform_utf8,
    const char* resource_type_utf8, uint32_t code_page, const char* text_utf8,
    int* out_supported) {
    if (!out_supported || !text_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    *out_supported = 0;
    peare::OpenedResource opened;
    const peare_status status = loadResource(payload, payload_length, platform_utf8,
                                             resource_type_utf8, &opened);
    if (status != PEARE_STATUS_OK) return status;
    if (code_page) opened.context.codePage = code_page;
    if (!prepareFontResource(&opened)) return PEARE_STATUS_NOT_APPLICABLE;

    try {
        const QString text = QString::fromUtf8(text_utf8);
        peare::font::SfntFont sfnt;
        if (peare::font::parseSfnt(opened.payload, &sfnt, nullptr)) {
            QSet<quint32> mappedCodepoints;
            for (quint32 codepoint : sfnt.characterMap)
                mappedCodepoints.insert(codepoint);
            *out_supported = 1;
            for (quint32 codepoint : text.toUcs4()) {
                if (!QChar::isSpace(codepoint) && !mappedCodepoints.contains(codepoint)) {
                    *out_supported = 0;
                    break;
                }
            }
            return PEARE_STATUS_OK;
        }

        for (uint codepoint : text.toUcs4()) {
            if (QChar::isSpace(codepoint)) continue;
            QString error;
            const QImage image = peare::ResourceDecoder::renderFont(
                opened, QString::fromUcs4(&codepoint, 1), 1, 4, qRgba(0, 0, 0, 255),
                qRgba(255, 255, 255, 255), &error);
            if (image.isNull()) return PEARE_STATUS_DECODE_FAILED;
            bool hasInk = false;
            for (int y = 0; !hasInk && y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixelColor(x, y) != QColor(Qt::white)) {
                        hasInk = true;
                        break;
                    }
                }
            }
            if (!hasInk) return PEARE_STATUS_OK;
        }
        *out_supported = 1;
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_get_item_info(const uint8_t* payload, size_t payload_length,
                                 const char* platform_utf8, const char* resource_type_utf8,
                                 size_t item_index, peare_info_id info_id, peare_value* out_value) {
    if (!out_value) return PEARE_STATUS_INVALID_ARGUMENT;
    clearValue(out_value);
    peare::OpenedResource opened;
    peare_status load_status =
        loadResource(payload, payload_length, platform_utf8, resource_type_utf8, &opened);
    if (load_status != PEARE_STATUS_OK) return load_status;
    if (!isBitmapItemInfoId(info_id)) return PEARE_STATUS_UNSUPPORTED_INFO;

    try {
        if (isFontResource(opened)) return PEARE_STATUS_NOT_APPLICABLE;
        const QVector<peare::DecodedImageInfo> images = peare::ResourceDecoder::imageInfo(opened);
        if (images.isEmpty()) return PEARE_STATUS_NOT_APPLICABLE;
        if (item_index >= static_cast<size_t>(images.size())) return PEARE_STATUS_OUT_OF_RANGE;

        const peare::DecodedImageInfo& image = images.at(static_cast<int>(item_index));
        switch (info_id) {
        case PEARE_INFO_BITMAP_WIDTH:
            return setUnsignedValue(static_cast<uint64_t>(image.width), out_value);
        case PEARE_INFO_BITMAP_HEIGHT:
            return setUnsignedValue(static_cast<uint64_t>(image.height), out_value);
        case PEARE_INFO_BITMAP_BPP:
            return image.bitsPerPixel > 0
                       ? setUnsignedValue(static_cast<uint64_t>(image.bitsPerPixel), out_value)
                       : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        case PEARE_INFO_BITMAP_COMPRESSION:
            return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
        default:
            return PEARE_STATUS_UNSUPPORTED_INFO;
        }
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_get_info(const uint8_t* payload, size_t payload_length,
                            const char* platform_utf8, const char* resource_type_utf8,
                            peare_info_id info_id, peare_value* out_value) {
    if (!out_value) return PEARE_STATUS_INVALID_ARGUMENT;
    clearValue(out_value);
    peare::OpenedResource opened;
    peare_status load_status =
        loadResource(payload, payload_length, platform_utf8, resource_type_utf8, &opened);
    if (load_status != PEARE_STATUS_OK) return load_status;

    const auto& context = opened.context;
    switch (info_id) {
    case PEARE_INFO_RESOURCE_PLATFORM:
        return platform_utf8 && *platform_utf8
                   ? setUtf8Value(QString::fromUtf8(platform_utf8), out_value)
                   : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_RESOURCE_CONTAINER_FORMAT:
        return PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_RESOURCE_TYPE:
        return resource_type_utf8 && *resource_type_utf8
                   ? setUtf8Value(QString::fromUtf8(resource_type_utf8), out_value)
                   : PEARE_STATUS_VALUE_NOT_DETERMINABLE;
    case PEARE_INFO_RESOURCE_LANGUAGE:
        return context.language.isEmpty() ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                          : setUtf8Value(context.language, out_value);
    case PEARE_INFO_TEXT_CODEPAGE: {
        const peare::ResourceKind kind = peare::DecoderRoute::classify(opened);
        if (!isTextResourceKind(kind)) return PEARE_STATUS_NOT_APPLICABLE;
        return context.codePage == 0 ? PEARE_STATUS_VALUE_NOT_DETERMINABLE
                                     : setUnsignedValue(context.codePage, out_value);
    }
    case PEARE_INFO_FONT_FACE_NAME:
    case PEARE_INFO_FONT_DEVICE_NAME:
    case PEARE_INFO_FONT_COPYRIGHT:
    case PEARE_INFO_FONT_CODEPAGE:
    case PEARE_INFO_FONT_FIRST_CHARACTER:
    case PEARE_INFO_FONT_LAST_CHARACTER:
    case PEARE_INFO_FONT_DEFAULT_CHARACTER:
    case PEARE_INFO_FONT_BREAK_CHARACTER:
    case PEARE_INFO_FONT_FIXED_WIDTH:
    case PEARE_INFO_FONT_POINT_SIZE:
    case PEARE_INFO_FONT_PIXEL_HEIGHT:
    case PEARE_INFO_FONT_ASCENT:
    case PEARE_INFO_FONT_DESCENT:
    case PEARE_INFO_FONT_AVERAGE_WIDTH:
    case PEARE_INFO_FONT_MAXIMUM_WIDTH:
    case PEARE_INFO_FONT_GLYPH_COUNT:
        return prepareFontResource(&opened) ? getFontInfo(opened, info_id, out_value)
                                            : PEARE_STATUS_NOT_APPLICABLE;
    case PEARE_INFO_POINTER_HOTSPOT_X:
    case PEARE_INFO_POINTER_HOTSPOT_Y: {
        const peare::ResourceKind kind = peare::DecoderRoute::classify(opened);
        return (kind == peare::ResourceKind::Pointer || kind == peare::ResourceKind::Cursor)
                   ? getPointerHotspot(opened, info_id, out_value)
                   : PEARE_STATUS_NOT_APPLICABLE;
    }
    case PEARE_INFO_BITMAP_WIDTH:
    case PEARE_INFO_BITMAP_HEIGHT:
    case PEARE_INFO_BITMAP_BPP:
    case PEARE_INFO_BITMAP_COMPRESSION:
        return peare_get_item_info(payload, payload_length, platform_utf8, resource_type_utf8, 0,
                                   info_id, out_value);
    case PEARE_INFO_NONE:
        return PEARE_STATUS_UNSUPPORTED_INFO;
    default:
        return PEARE_STATUS_UNSUPPORTED_INFO;
    }
}

void peare_blob_free(peare_blob* blob) {
    freeLocalBlob(blob);
}

peare_status peare_font_get_codepoints_with_codepage(const uint8_t* payload, size_t payload_length,
                                       const char* platform_utf8, const char* resource_type_utf8,
                                       uint32_t code_page,
                                       peare_font_codepoint_array* out_codepoints) {
    if (!out_codepoints) return PEARE_STATUS_INVALID_ARGUMENT;
    out_codepoints->items = nullptr;
    out_codepoints->count = 0;
    out_codepoints->glyph_index_mode = 0;
    try {
        peare::OpenedResource opened;
        const peare_status status = loadResource(payload, payload_length, platform_utf8,
                                                 resource_type_utf8, &opened);
        if (status != PEARE_STATUS_OK) return status;
        if (code_page) opened.context.codePage = code_page;
        if (!prepareFontResource(&opened)) return PEARE_STATUS_NOT_APPLICABLE;
        QVector<quint32> characters;
        int glyphIndexMode = 0;
        peare::font::Type1Font type1Font;
        peare::font::SfntFont font;
        if (peare::resources::RT_FONT::LooksLikeOs2Fnt(opened.payload)) {
            if (!peare::resources::OS2_RT_FONT::GetCodepoints(opened.payload, &characters))
                return PEARE_STATUS_DECODE_FAILED;
        } else if (peare::resources::RT_FONT::LooksLikeMetricsFont(opened.payload)) {
            return PEARE_STATUS_NOT_APPLICABLE;
        } else if (peare::resources::RT_FONT::LooksLikeWindowsFnt(opened.payload)) {
            peare_value first = {};
            peare_value last = {};
            if (getFontInfo(opened, PEARE_INFO_FONT_FIRST_CHARACTER, &first) != PEARE_STATUS_OK ||
                getFontInfo(opened, PEARE_INFO_FONT_LAST_CHARACTER, &last) != PEARE_STATUS_OK ||
                first.value.unsigned_value > last.value.unsigned_value ||
                last.value.unsigned_value - first.value.unsigned_value >= 65536)
                return PEARE_STATUS_DECODE_FAILED;
            characters.reserve(int(last.value.unsigned_value - first.value.unsigned_value + 1));
            for (quint64 code = first.value.unsigned_value;
                 code <= last.value.unsigned_value; ++code)
                characters.push_back(quint32(code));
        } else if (peare::font::parseType3Font(opened.payload, &type1Font, nullptr) ||
            peare::font::parseType1Font(opened.payload, &type1Font, nullptr)) {
            glyphIndexMode = 1;
            for (int i = 0; i < type1Font.glyphs.size(); ++i)
                characters.push_back(quint32(i));
        } else if (peare::font::parseSfnt(opened.payload, &font, nullptr)) {
            glyphIndexMode = 1;
            for (quint32 i = 0; i < font.glyphCount; ++i)
                characters.push_back(i);
        } else {
            peare::font::BitmapFont bitmapFont;
            if (peare::font::parse(opened.payload, &bitmapFont, nullptr)) {
                if (bitmapFont.format == peare::font::Format::Bmf ||
                    bitmapFont.format == peare::font::Format::Psf1 ||
                    bitmapFont.format == peare::font::Format::Psf2) {
                    for (const peare::font::Glyph& glyph : bitmapFont.glyphs)
                        if (glyph.codepoint != 0xffffffffu)
                            characters.push_back(glyph.codepoint);
                    if (characters.isEmpty() && !bitmapFont.glyphs.isEmpty()) {
                        glyphIndexMode = 1;
                        for (int i = 0; i < bitmapFont.glyphs.size(); ++i)
                            characters.push_back(quint32(i));
                    }
                } else {
                    glyphIndexMode = 1;
                    for (int i = 0; i < bitmapFont.glyphs.size(); ++i)
                        characters.push_back(quint32(i));
                }
            } else {
                peare::font::SfnFont sfnFont;
                if (peare::font::parseSfnFont(opened.payload, &sfnFont, nullptr)) {
                    glyphIndexMode = 1;
                    for (int i = 0; i < sfnFont.glyphs.size(); ++i)
                        characters.push_back(quint32(i));
                } else {
                    peare::font::SvgFont svgFont;
                    if (peare::font::parseSvgFont(opened.payload, &svgFont, nullptr)) {
                        for (const peare::font::SvgGlyph& glyph : svgFont.glyphs) {
                            const QVector<uint> unicode = glyph.unicode.toUcs4();
                            if (unicode.size() == 1) characters.push_back(unicode.first());
                        }
                        if (characters.isEmpty()) {
                            glyphIndexMode = 1;
                            for (int i = 0; i < svgFont.glyphs.size(); ++i)
                                characters.push_back(quint32(i));
                        }
                    } else {
                        peare::font::ShpFont shpFont;
                        if (!peare::font::parseShpFont(opened.payload, &shpFont, nullptr))
                            return PEARE_STATUS_NOT_APPLICABLE;
                        if (opened.context.codePage && shpFont.unicode &&
                            shpFont.encoding == 1 &&
                            peare::font::mapPackedShpUnicode(&shpFont,
                                opened.context.codePage, nullptr)) {
                            for (auto it = shpFont.glyphByUnicode.cbegin();
                                 it != shpFont.glyphByUnicode.cend(); ++it)
                                characters.push_back(it.key());
                        } else {
                            glyphIndexMode = 1;
                            for (int i = 0; i < shpFont.glyphs.size(); ++i)
                                characters.push_back(quint32(i));
                        }
                    }
                }
            }
        }
        std::sort(characters.begin(), characters.end());
        characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
        if (size_t(characters.size()) > SIZE_MAX / sizeof(uint32_t))
            return PEARE_STATUS_ALLOCATION_FAILED;
        auto* items = characters.isEmpty() ? nullptr : static_cast<uint32_t*>(
            std::malloc(size_t(characters.size()) * sizeof(uint32_t)));
        if (!items && !characters.isEmpty()) return PEARE_STATUS_ALLOCATION_FAILED;
        for (int i = 0; i < characters.size(); ++i) items[i] = characters.at(i);
        out_codepoints->items = items;
        out_codepoints->count = size_t(characters.size());
        out_codepoints->glyph_index_mode = glyphIndexMode;
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_font_render_glyph_index_with_codepage(const uint8_t* payload,
                                           size_t payload_length,
                                           const char* platform_utf8,
                                           const char* resource_type_utf8,
                                           uint32_t code_page,
                                           uint32_t glyph_index,
                                           const peare_font_render_options* options,
                                           peare_blob* out_png) {
    if (!out_png) return PEARE_STATUS_INVALID_ARGUMENT;
    out_png->bytes = nullptr;
    out_png->length = 0;
    peare::OpenedResource opened;
    const peare_status status = loadResource(payload, payload_length, platform_utf8,
                                             resource_type_utf8, &opened);
    if (status != PEARE_STATUS_OK) return status;
    if (code_page) opened.context.codePage = code_page;
    if (!prepareFontResource(&opened)) return PEARE_STATUS_NOT_APPLICABLE;
    uint32_t scale = 1;
    uint32_t padding = 0;
    uint32_t foreground = 0x000000FFu;
    uint32_t background = 0xFFFFFFFFu;
    if (options) {
        scale = options->scale;
        padding = options->padding;
        foreground = options->foreground_rgba;
        background = options->background_rgba;
    }
    if (!scale || scale > uint32_t(INT_MAX) || padding > uint32_t(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    try {
        QString error;
        const QImage image = peare::ResourceDecoder::renderFontGlyph(
            opened, glyph_index, int(scale), int(padding), fromRgba32(foreground),
            fromRgba32(background), &error);
        if (image.isNull()) return PEARE_STATUS_DECODE_FAILED;
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return PEARE_STATUS_DECODE_FAILED;
        return copyByteArray(png, out_png);
    } catch (const std::bad_alloc&) {
        freeLocalBlob(out_png);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        freeLocalBlob(out_png);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_font_render(const uint8_t* payload, size_t payload_length,
                               const char* platform_utf8, const char* resource_type_utf8,
                               const char* text_utf8, const peare_font_render_options* options,
                               peare_blob* out_png) {
    return peare_font_render_with_codepage(payload, payload_length, platform_utf8,
        resource_type_utf8, 0, text_utf8, options, out_png);
}

peare_status peare_font_get_codepoints(const uint8_t* payload, size_t payload_length,
                                       const char* platform_utf8, const char* resource_type_utf8,
                                       peare_font_codepoint_array* out_codepoints) {
    return peare_font_get_codepoints_with_codepage(payload, payload_length, platform_utf8,
        resource_type_utf8, 0, out_codepoints);
}

peare_status peare_font_get_collection_faces(const uint8_t* payload, size_t payload_length,
                                             peare_blob_array* out_faces) {
    if (!out_faces || (!payload && payload_length) ||
        payload_length > static_cast<size_t>(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    clearBlobArray(out_faces);
    try {
        const QByteArray bytes(reinterpret_cast<const char*>(payload), int(payload_length));
        QVector<QByteArray> faces;
        QVector<peare::font::SfntFont> sfntFaces;
        if (peare::font::parseTtcCollection(bytes, &sfntFaces, nullptr)) {
            faces.reserve(sfntFaces.size());
            for (const auto& face : sfntFaces) faces.push_back(face.sfntBytes);
        } else if (!peare::font::parseSfnCollection(bytes, &faces, nullptr)) {
            return PEARE_STATUS_NOT_APPLICABLE;
        }
        peare_status status = allocateBlobArray(static_cast<size_t>(faces.size()), out_faces);
        if (status != PEARE_STATUS_OK) return status;
        for (int i = 0; i < faces.size(); ++i) {
            status = copyByteArray(faces.at(i), &out_faces->items[size_t(i)]);
            if (status != PEARE_STATUS_OK) {
                peare_blob_array_free(out_faces);
                return status;
            }
        }
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        peare_blob_array_free(out_faces);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        peare_blob_array_free(out_faces);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

peare_status peare_font_render_glyph_index(const uint8_t* payload, size_t payload_length,
                                           const char* platform_utf8,
                                           const char* resource_type_utf8,
                                           uint32_t glyph_index,
                                           const peare_font_render_options* options,
                                           peare_blob* out_png) {
    return peare_font_render_glyph_index_with_codepage(payload, payload_length, platform_utf8,
        resource_type_utf8, 0, glyph_index, options, out_png);
}

peare_status peare_font_render_glyph_indexes_with_codepage(
    const uint8_t* payload, size_t payload_length, const char* platform_utf8,
    const char* resource_type_utf8, uint32_t code_page, const uint32_t* glyph_indexes,
    size_t glyph_count, const peare_font_render_options* options, peare_blob_array* out_pngs) {
    if (!out_pngs) return PEARE_STATUS_INVALID_ARGUMENT;
    clearBlobArray(out_pngs);
    if ((!glyph_indexes && glyph_count) || glyph_count > 8192)
        return PEARE_STATUS_INVALID_ARGUMENT;
    peare::OpenedResource opened;
    const peare_status status = loadResource(payload, payload_length, platform_utf8,
                                             resource_type_utf8, &opened);
    if (status != PEARE_STATUS_OK) return status;
    if (code_page) opened.context.codePage = code_page;
    if (!prepareFontResource(&opened)) return PEARE_STATUS_NOT_APPLICABLE;
    uint32_t scale = 1;
    uint32_t padding = 0;
    uint32_t foreground = 0x000000FFu;
    uint32_t background = 0xFFFFFFFFu;
    if (options) {
        scale = options->scale;
        padding = options->padding;
        foreground = options->foreground_rgba;
        background = options->background_rgba;
    }
    if (!scale || scale > uint32_t(INT_MAX) || padding > uint32_t(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    try {
        QVector<quint32> indexes;
        indexes.reserve(int(glyph_count));
        for (size_t i = 0; i < glyph_count; ++i) indexes.push_back(glyph_indexes[i]);
        QString error;
        const QVector<QImage> images = peare::ResourceDecoder::renderFontGlyphs(
            opened, indexes, int(scale), int(padding), fromRgba32(foreground),
            fromRgba32(background), &error);
        if (images.size() != int(glyph_count)) return PEARE_STATUS_DECODE_FAILED;
        peare_status result = allocateBlobArray(glyph_count, out_pngs);
        if (result != PEARE_STATUS_OK) return result;
        for (int i = 0; i < images.size(); ++i) {
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !images.at(i).save(&buffer, "PNG")) {
                peare_blob_array_free(out_pngs);
                return PEARE_STATUS_DECODE_FAILED;
            }
            result = copyByteArray(png, &out_pngs->items[size_t(i)]);
            if (result != PEARE_STATUS_OK) {
                peare_blob_array_free(out_pngs);
                return result;
            }
        }
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        peare_blob_array_free(out_pngs);
        return PEARE_STATUS_DECODE_FAILED;
    }
}

void peare_blob_array_free(peare_blob_array* array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) freeLocalBlob(&array->items[i]);
    std::free(array->items);
    clearBlobArray(array);
}

void peare_font_codepoint_array_free(peare_font_codepoint_array* array) {
    if (!array) return;
    std::free(array->items);
    array->items = nullptr;
    array->count = 0;
    array->glyph_index_mode = 0;
}

void peare_value_free(peare_value* value) {
    if (!value) return;
    if (value->type == PEARE_VALUE_UTF8 || value->type == PEARE_VALUE_BYTES)
        std::free(const_cast<uint8_t*>(value->value.buffer.data));
    clearValue(value);
}
}
