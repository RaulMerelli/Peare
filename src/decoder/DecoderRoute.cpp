#include "DecoderRoute.h"

namespace peare {
namespace {

QString normalizedType(QString type)
{
    type = type.trimmed().toUpper();
    if (type.startsWith(QLatin1Char('#')))
        type.remove(0, 1);
    return type;
}

bool typeIs(const QString& type, const char* canonical, const char* alias = nullptr)
{
    if (type == QLatin1String(canonical))
        return true;
    return alias && type == QLatin1String(alias);
}

} // namespace

ResourceKind DecoderRoute::classify(const ResourceEntry& entry) noexcept
{
    // The declared resource type is authoritative. Platform and container
    // metadata only disambiguate aliases; payload heuristics remain outside
    // this classifier and are used by RawDetect only for unknown kinds.
    const QString type = normalizedType(entry.type);

    if (typeIs(type, "RT_ICON", "ICON")) return ResourceKind::Icon;
    if (typeIs(type, "RT_CURSOR", "CURSOR")) return ResourceKind::Cursor;
    if (typeIs(type, "RT_GROUP_ICON", "GROUP_ICON")) return ResourceKind::GroupIcon;
    if (typeIs(type, "RT_GROUP_CURSOR", "GROUP_CURSOR")) return ResourceKind::GroupCursor;
    if (typeIs(type, "RT_BITMAP", "BITMAP")) return ResourceKind::Bitmap;
    if (typeIs(type, "RT_POINTER", "POINTER")) return ResourceKind::Pointer;
    if (typeIs(type, "RT_STRING", "STRING")) return ResourceKind::StringTable;
    if (typeIs(type, "RT_DLGINCLUDE", "DLGINCLUDE")) return ResourceKind::DialogInclude;
    if (typeIs(type, "RT_MESSAGE", "RT_MESSAGETABLE") || type == QLatin1String("MESSAGETABLE"))
        return ResourceKind::MessageTable;
    if (typeIs(type, "RT_VERSION", "RT_VERSIONINFO") || type == QLatin1String("VERSION"))
        return ResourceKind::Version;
    if (typeIs(type, "RT_ACCELERATOR", "ACCELERATOR")) return ResourceKind::Accelerator;
    if (typeIs(type, "RT_ACCELTABLE", "ACCELTABLE")) return ResourceKind::AccelTable;
    if (typeIs(type, "RT_FONTDIR", "FONTDIR")) return ResourceKind::FontDirectory;
    if (typeIs(type, "RT_FONT", "FONT")) return ResourceKind::Font;
    if (typeIs(type, "RT_DISPLAYINFO", "DISPLAYINFO")) return ResourceKind::DisplayInfo;
    if (typeIs(type, "RT_HELPTABLE", "HELPTABLE")) return ResourceKind::HelpTable;
    if (typeIs(type, "RT_HELPSUBTABLE", "HELPSUBTABLE")) return ResourceKind::HelpSubTable;
    if (typeIs(type, "RT_NAMETABLE", "NAMETABLE")) return ResourceKind::NameTable;
    if (typeIs(type, "RT_MENU", "MENU")) return ResourceKind::Menu;
    if (typeIs(type, "RT_DIALOG", "DIALOG")) return ResourceKind::Dialog;
    if (type == QLatin1String("XBE_LOGO_RLE")) return ResourceKind::XbeLogoRle;
    if (type == QLatin1String("FMIM")) return ResourceKind::Fmim;

    // Numeric Windows resource identifiers are accepted only when the
    // originating container says Windows. This preserves named OS/2 types.
    if (!entry.isOs2 && (entry.format == ModuleFormat::PE || entry.format == ModuleFormat::NE)) {
        if (type == QLatin1String("1")) return ResourceKind::Cursor;
        if (type == QLatin1String("2")) return ResourceKind::Bitmap;
        if (type == QLatin1String("3")) return ResourceKind::Icon;
        if (type == QLatin1String("4")) return ResourceKind::Menu;
        if (type == QLatin1String("5")) return ResourceKind::Dialog;
        if (type == QLatin1String("6")) return ResourceKind::StringTable;
        if (type == QLatin1String("7")) return ResourceKind::FontDirectory;
        if (type == QLatin1String("8")) return ResourceKind::Font;
        if (type == QLatin1String("9")) return ResourceKind::Accelerator;
        if (type == QLatin1String("12")) return ResourceKind::GroupCursor;
        if (type == QLatin1String("14")) return ResourceKind::GroupIcon;
        if (type == QLatin1String("16")) return ResourceKind::Version;
        if (type == QLatin1String("17")) return ResourceKind::DialogInclude;
        if (type == QLatin1String("24")) return ResourceKind::MessageTable;
    }

    return ResourceKind::Unknown;
}

ResourceKind DecoderRoute::classify(const OpenedResource& resource) noexcept
{
    ResourceEntry entry;
    entry.type = resource.context.type;
    entry.name = resource.context.identifier;
    entry.language = resource.context.language;
    entry.codePage = resource.context.codePage;
    entry.format = resource.context.containerFormat;
    entry.isOs2 = resource.context.platform == ResourcePlatform::Os2;
    entry.baseId = resource.context.baseId;
    entry.dataOffset = resource.context.dataOffset;
    entry.dataSize = static_cast<quint64>(resource.payload.size());
    entry.data = resource.payload;
    return classify(entry);
}

} // namespace peare
