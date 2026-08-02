#include "ResourceDecoder.h"

#include "resources/ModuleResources.h"
#include "resources/ResourceResolver.h"
#include "resources/RT_FONT/RT_FONT.h"
#include "resources/RT_BITMAP/RT_BITMAP.h"
#include "DecoderRoute.h"

#include <exception>

namespace peare {
namespace {

ModuleFormat inferredFormatFor(const OpenedResource& resource);

ResourceEntry decoderEntryFor(const OpenedResource& resource)
{
    ResourceEntry entry;
    entry.type = resource.context.type;
    entry.name = resource.context.identifier;
    entry.language = resource.context.language;
    entry.dataOffset = resource.context.dataOffset;
    entry.dataSize = static_cast<quint64>(resource.payload.size());
    entry.codePage = resource.context.codePage;
    entry.format = resource.context.containerFormat != ModuleFormat::Unknown
        ? resource.context.containerFormat
        : inferredFormatFor(resource);
    entry.isOs2 = resource.context.platform == ResourcePlatform::Os2;
    entry.baseId = resource.context.baseId;
    entry.data = resource.payload;
    return entry;
}


quint16 readLe16(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

QString normalizedTypeName(QString type)
{
    type = type.trimmed().toUpper();
    if (type.startsWith(QLatin1Char('#')))
        type.remove(0, 1);
    return type;
}

bool looksLikePeStringTable(const QByteArray& data)
{
    qsizetype offset = 0;
    int entries = 0;
    while (offset + 2 <= data.size() && entries < 16) {
        const quint16 length = readLe16(data, int(offset));
        offset += 2;
        const quint64 bytes = quint64(length) * 2u;
        if (bytes > quint64(data.size() - offset))
            return false;
        offset += qsizetype(bytes);
        ++entries;
    }
    return entries == 16 && offset == data.size();
}

bool looksLikePeMessageTable(const QByteArray& data)
{
    if (data.size() < 4)
        return false;
    const quint32 blocks = quint32(uchar(data.at(0))) |
                           (quint32(uchar(data.at(1))) << 8) |
                           (quint32(uchar(data.at(2))) << 16) |
                           (quint32(uchar(data.at(3))) << 24);
    if (blocks == 0 || blocks > 0x10000u)
        return false;
    const quint64 tableEnd = 4u + quint64(blocks) * 12u;
    if (tableEnd > quint64(data.size()))
        return false;
    for (quint32 i = 0; i < blocks; ++i) {
        const int base = 4 + int(i) * 12;
        const quint32 low = quint32(uchar(data.at(base))) |
                            (quint32(uchar(data.at(base + 1))) << 8) |
                            (quint32(uchar(data.at(base + 2))) << 16) |
                            (quint32(uchar(data.at(base + 3))) << 24);
        const quint32 high = quint32(uchar(data.at(base + 4))) |
                             (quint32(uchar(data.at(base + 5))) << 8) |
                             (quint32(uchar(data.at(base + 6))) << 16) |
                             (quint32(uchar(data.at(base + 7))) << 24);
        const quint32 entries = quint32(uchar(data.at(base + 8))) |
                                (quint32(uchar(data.at(base + 9))) << 8) |
                                (quint32(uchar(data.at(base + 10))) << 16) |
                                (quint32(uchar(data.at(base + 11))) << 24);
        if (high < low || entries < tableEnd || entries >= quint32(data.size()))
            return false;
    }
    return true;
}

bool looksLikePeMenu(const QByteArray& data)
{
    if (data.size() < 8 || readLe16(data, 0) != 0 || readLe16(data, 2) != 0)
        return false;
    // A standard PE menu template starts with version/offset WORDs followed by
    // UTF-16 strings. Require at least one zero high byte in the first text area
    // to avoid treating a legacy ANSI menu as Unicode merely because it starts
    // with zero flags.
    const int limit = qMin(data.size(), 48);
    for (int i = 7; i < limit; i += 2)
        if (data.at(i) == '\0' && data.at(i - 1) != '\0')
            return true;
    return false;
}

ModuleFormat inferredFormatFor(const OpenedResource& resource)
{
    if (resource.context.platform == ResourcePlatform::Os2)
        return ModuleFormat::LX; // All OS/2 resource decoders share this branch.
    if (resource.context.platform != ResourcePlatform::Windows)
        return ModuleFormat::Unknown;

    const QString type = normalizedTypeName(resource.context.type);
    if (type == QLatin1String("RT_STRING") || type == QLatin1String("STRING"))
        return looksLikePeStringTable(resource.payload) ? ModuleFormat::PE : ModuleFormat::NE;
    if (type == QLatin1String("RT_MESSAGE") || type == QLatin1String("RT_MESSAGETABLE") ||
        type == QLatin1String("MESSAGETABLE"))
        return looksLikePeMessageTable(resource.payload) ? ModuleFormat::PE : ModuleFormat::NE;
    if (type == QLatin1String("RT_MENU") || type == QLatin1String("MENU"))
        return looksLikePeMenu(resource.payload) ? ModuleFormat::PE : ModuleFormat::NE;

    // Other currently supported Windows decoders either inspect their own
    // payload headers or use identical structures for PE and NE.
    return ModuleFormat::Unknown;
}

bool fontRangeFor(const OpenedResource& resource, int* first, int* last, int* height)
{
    const QByteArray& data = resource.payload;
    if (resource.context.platform == ResourcePlatform::Os2) {
        const int marker = data.indexOf(QByteArrayLiteral("OS/2 FONT"));
        const int metrics = marker >= 8 && marker - 8 <= 16 ? marker - 8 + 20 : 20;
        if (metrics < 0 || metrics + 136 > data.size()) return false;
        *first = int(readLe16(data, metrics + 114));
        *last = int(readLe16(data, metrics + 116));
        if (height) *height = int(readLe16(data, metrics + 80)) + int(readLe16(data, metrics + 82));
        return *last >= *first;
    }
    if (data.size() < 118) return false;
    const quint16 version = readLe16(data, 0);
    if (version != 0x0100 && version != 0x0200 && version != 0x0300) return false;
    *first = int(quint8(data.at(95)));
    *last = int(quint8(data.at(96)));
    if (height) *height = int(readLe16(data, 88));
    return *last >= *first;
}

class NullResolver final : public resources::IResourceResolver {
public:
    const ResourceEntry* find(const QString&, const QString&, const QString&) const override
    {
        return nullptr;
    }
};

} // namespace

QVector<DecodedImageInfo> ResourceDecoder::imageInfo(const OpenedResource& resource) noexcept
{
    QVector<DecodedImageInfo> result;
    if (!resource.isValid())
        return result;

    try {
        const ResourceKind kind = DecoderRoute::classify(resource);
        if (kind == ResourceKind::Bitmap || kind == ResourceKind::Pointer) {
            const QVector<resources::Img> images = resources::RT_BITMAP::getDetailed(resource.payload);
            result.reserve(images.size());
            for (const resources::Img& image : images) {
                DecodedImageInfo info;
                info.width = image.Size.width();
                info.height = image.Size.height();
                info.bitsPerPixel = image.BitCount;
                result.append(info);
            }
            return result;
        }

        const ResourcePreview decoded = preview(resource);
        result.reserve(decoded.images.size());
        for (const QImage& image : decoded.images) {
            DecodedImageInfo info;
            info.width = image.width();
            info.height = image.height();
            info.bitsPerPixel = image.depth();
            result.append(info);
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

ResourcePreview ResourceDecoder::preview(const OpenedResource& resource) noexcept
{
    static const NullResolver resolver;
    return preview(resource, resolver);
}

ResourcePreview ResourceDecoder::preview(const OpenedResource& resource,
                                         const resources::IResourceResolver& resolver) noexcept
{
    ResourcePreview result;
    if (!resource.isValid()) {
        result.error = resource.error.isEmpty()
            ? QStringLiteral("Invalid resource handle")
            : resource.error;
        return result;
    }

    try {
        const ResourceEntry entry = decoderEntryFor(resource);
        return resources::ModuleResources::preview(entry, resolver);
    } catch (const std::exception& exception) {
        result.error = QStringLiteral("Resource loaded, but preview failed: %1")
            .arg(QString::fromLocal8Bit(exception.what()));
    } catch (...) {
        result.error = QStringLiteral("Resource loaded, but preview failed: unknown exception");
    }
    return result;
}


bool ResourceDecoder::fontCharacterRange(const OpenedResource& resource, int* firstCharacter,
                                         int* lastCharacter, int* pixelHeight) noexcept
{
    if (!firstCharacter || !lastCharacter || !resource.isValid()) return false;
    try {
        return fontRangeFor(resource, firstCharacter, lastCharacter, pixelHeight);
    } catch (...) {
        return false;
    }
}

QImage ResourceDecoder::renderFont(const OpenedResource& resource, const QString& text,
                                   int scale, int padding, QRgb foregroundRgba,
                                   QRgb backgroundRgba, QString* error) noexcept
{
    if (error) error->clear();
    if (!resource.isValid()) {
        if (error) *error = resource.error.isEmpty()
            ? QStringLiteral("Invalid resource handle") : resource.error;
        return {};
    }
    try {
        return resources::RT_FONT::renderText(decoderEntryFor(resource), text, scale,
                                              padding, foregroundRgba,
                                              backgroundRgba, error);
    } catch (const std::exception& exception) {
        if (error) *error = QStringLiteral("Font rendering failed: %1")
            .arg(QString::fromLocal8Bit(exception.what()));
    } catch (...) {
        if (error) *error = QStringLiteral("Font rendering failed: unknown exception");
    }
    return {};
}

} // namespace peare
