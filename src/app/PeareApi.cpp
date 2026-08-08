#include "PeareApi.h"

#include <QDir>
#include <QFileInfo>
#include <QtEndian>
#include <utility>

namespace pearegui {

QString blobToString(const peare_blob& blob)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(blob.bytes), static_cast<int>(blob.length));
}

QString statusText(peare_status status)
{
    return QString::fromUtf8(peare_status_message(status));
}

QString containerName(peare_container_format format)
{
    switch (format) {
    case PEARE_CONTAINER_DOS_MZ: return QStringLiteral("DOS MZ");
    case PEARE_CONTAINER_PE: return QStringLiteral("PE");
    case PEARE_CONTAINER_NE: return QStringLiteral("NE");
    case PEARE_CONTAINER_LE: return QStringLiteral("LE");
    case PEARE_CONTAINER_LX: return QStringLiteral("LX");
    case PEARE_CONTAINER_XEX: return QStringLiteral("XEX");
    case PEARE_CONTAINER_XBE: return QStringLiteral("XBE");
    case PEARE_CONTAINER_XUIZ: return QStringLiteral("XUIZ");
    case PEARE_CONTAINER_LIVE_PIRS: return QStringLiteral("LIVE/PIRS");
    case PEARE_CONTAINER_CON: return QStringLiteral("CON");
    case PEARE_CONTAINER_OS2_PACK: return QStringLiteral("OS/2 PACK");
    case PEARE_CONTAINER_OS2_EA: return QStringLiteral("OS/2 EA");
    case PEARE_CONTAINER_RESX: return QStringLiteral("RESX");
    case PEARE_CONTAINER_CUE_BIN: return QStringLiteral("BIN/CUE");
    case PEARE_CONTAINER_SZDD: return QStringLiteral("SZDD");
    case PEARE_CONTAINER_SIEMENS_IMG: return QStringLiteral("Siemens IMG");
    case PEARE_CONTAINER_SIEMENS_FWF: return QStringLiteral("Siemens FWF");
    case PEARE_CONTAINER_SIEMENS_FSF: return QStringLiteral("Siemens FSF");
    case PEARE_CONTAINER_ISO9660: return QStringLiteral("ISO 9660");
    case PEARE_CONTAINER_WIM: return QStringLiteral("WIM");
    case PEARE_CONTAINER_FAT: return QStringLiteral("FAT");
    case PEARE_CONTAINER_UDF: return QStringLiteral("UDF");
    case PEARE_CONTAINER_EXFAT: return QStringLiteral("exFAT");
    case PEARE_CONTAINER_VMDK: return QStringLiteral("VMDK");
    case PEARE_CONTAINER_VHD: return QStringLiteral("VHD");
    case PEARE_CONTAINER_VDI: return QStringLiteral("VDI");
    case PEARE_CONTAINER_QCOW: return QStringLiteral("QCOW");
    case PEARE_CONTAINER_QCOW2: return QStringLiteral("QCOW2");
    case PEARE_CONTAINER_FLOPPY_IMAGE: return QStringLiteral("Floppy image");
    case PEARE_CONTAINER_PS3_PUP: return QStringLiteral("PS3 PUP");
    case PEARE_CONTAINER_WAD: return QStringLiteral("WAD");
    case PEARE_CONTAINER_MTF: return QStringLiteral("MTF / BKF");
    case PEARE_CONTAINER_MDF_MDS: return QStringLiteral("MDF / MDS");
    case PEARE_CONTAINER_PARALLELS_HDD: return QStringLiteral("Parallels HDD");
    case PEARE_CONTAINER_PS2_ROMDIR: return QStringLiteral("PS2 ROMDIR");
    case PEARE_CONTAINER_MSI: return QStringLiteral("MSI");
    case PEARE_CONTAINER_MSG: return QStringLiteral("Outlook MSG");
    case PEARE_CONTAINER_APPX: return QStringLiteral("APPX");
    case PEARE_CONTAINER_MSIX: return QStringLiteral("MSIX");
    case PEARE_CONTAINER_APPXBUNDLE: return QStringLiteral("APPXBUNDLE");
    case PEARE_CONTAINER_EAPPX: return QStringLiteral("EAPPX");
    case PEARE_CONTAINER_EMSIX: return QStringLiteral("EMSIX");
    case PEARE_CONTAINER_EAPPXBUNDLE: return QStringLiteral("EAPPXBUNDLE");
    case PEARE_CONTAINER_EMSIXBUNDLE: return QStringLiteral("EMSIXBUNDLE");
    case PEARE_CONTAINER_XAP: return QStringLiteral("XAP");
    case PEARE_CONTAINER_VHDX: return QStringLiteral("VHDX");
    case PEARE_CONTAINER_SDI: return QStringLiteral("SDI");
    case PEARE_CONTAINER_XVA: return QStringLiteral("XVA");
    case PEARE_CONTAINER_SWAP: return QStringLiteral("Swap");
    case PEARE_CONTAINER_LVM: return QStringLiteral("LVM");
    case PEARE_CONTAINER_EXT: return QStringLiteral("ext");
    case PEARE_CONTAINER_NTFS: return QStringLiteral("NTFS");
    case PEARE_CONTAINER_XFS: return QStringLiteral("XFS");
    case PEARE_CONTAINER_JFS: return QStringLiteral("JFS");
    case PEARE_CONTAINER_HPFS: return QStringLiteral("HPFS");
    case PEARE_CONTAINER_FFU: return QStringLiteral("FFU");
    case PEARE_CONTAINER_SQUASHFS: return QStringLiteral("SquashFS");
    case PEARE_CONTAINER_HFSPLUS: return QStringLiteral("HFS+");
    case PEARE_CONTAINER_DMG: return QStringLiteral("DMG");
    case PEARE_CONTAINER_BTRFS: return QStringLiteral("Btrfs");
    case PEARE_CONTAINER_REGISTRY: return QStringLiteral("Registry");
    case PEARE_CONTAINER_BOOTCONFIG: return QStringLiteral("BCD");
    case PEARE_CONTAINER_CAB: return QStringLiteral("CAB");
    case PEARE_CONTAINER_RAW_DISK: return QStringLiteral("RAW");
    case PEARE_CONTAINER_ZIP: return QStringLiteral("ZIP");
    case PEARE_CONTAINER_TAR: return QStringLiteral("TAR");
    case PEARE_CONTAINER_LINUX_RAID: return QStringLiteral("Linux RAID");
    case PEARE_CONTAINER_DYNAMIC_DISK: return QStringLiteral("Dynamic Disk");
    case PEARE_CONTAINER_WINCE_ROM: return QStringLiteral("Windows CE ROM / IMGFS");
    default: return QStringLiteral("Unknown");
    }
}

QByteArray platformContextUtf8(peare_platform platform)
{
    switch (platform) {
    case PEARE_PLATFORM_WINDOWS: return QByteArrayLiteral("Windows");
    case PEARE_PLATFORM_OS2: return QByteArrayLiteral("OS/2");
    case PEARE_PLATFORM_OTHER: return QByteArrayLiteral("Other");
    default: return {};
    }
}

QString sanitizeFileName(QString value)
{
    static const QString invalid = QStringLiteral("<>:\\|?*\"/");
    for (QChar& c : value) if (invalid.contains(c) || c.unicode() < 32) c = QLatin1Char('_');
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('.'))) value.chop(1);
    return value.isEmpty() ? QStringLiteral("resource") : value;
}

QString uniquePath(const QString& path)
{
    if (!QFileInfo::exists(path)) return path;
    const QFileInfo info(path);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix = info.completeSuffix();
    for (int n = 2; ; ++n) {
        const QString name = suffix.isEmpty()
            ? QStringLiteral("%1_%2").arg(stem).arg(n)
            : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
        const QString candidate = QDir(dir).filePath(name);
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

QString originalExtension(const QString& type, const QByteArray& payload)
{
    const QString t = type.trimmed().toUpper();
    if (t == QStringLiteral("PE_MODULE")) return QStringLiteral(".image");
    if (t == QStringLiteral("XUIZ_CONTAINER") || payload.startsWith("XUIZ")) return QStringLiteral(".xzp");
    if (t.contains(QStringLiteral("BITMAP"))) return QStringLiteral(".bmp");
    if (t.contains(QStringLiteral("GROUP_ICON")) || t == QStringLiteral("RT_ICON") || t == QStringLiteral("ICON")) return QStringLiteral(".ico");
    if (t.contains(QStringLiteral("GROUP_CURSOR")) || t == QStringLiteral("RT_CURSOR") || t == QStringLiteral("CURSOR")) return QStringLiteral(".cur");
    if (t.contains(QStringLiteral("FONT"))) return QStringLiteral(".fnt");
    if (t.contains(QStringLiteral("STRING")) || t.contains(QStringLiteral("MENU")) || t.contains(QStringLiteral("DIALOG")) || t.contains(QStringLiteral("MESSAGE"))) return QStringLiteral(".bin");
    if (payload.startsWith("BM")) return QStringLiteral(".bmp");
    if (payload.startsWith("\x89PNG\r\n\x1a\n")) return QStringLiteral(".png");
    return QStringLiteral(".bin");
}

Resource::Resource(peare_resource_handle handle) : handle_(handle) {}
Resource::~Resource() { if (handle_) peare_resource_destroy(handle_); }
Resource::Resource(Resource&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}
Resource& Resource::operator=(Resource&& other) noexcept
{
    if (this != &other) {
        if (handle_) peare_resource_destroy(handle_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

QByteArray Resource::payload() const
{
    peare_blob blob{};
    if (!handle_ || peare_resource_get_payload(handle_, &blob) != PEARE_STATUS_OK) return {};
    QByteArray result(reinterpret_cast<const char*>(blob.bytes), static_cast<int>(blob.length));
    peare_blob_free(&blob);
    return result;
}

QStringList Resource::convertedExtensions() const
{
    QStringList result; peare_blob_array array{};
    if (!handle_ || peare_resource_get_converted_extensions(handle_, &array) != PEARE_STATUS_OK) return result;
    for (size_t i=0;i<array.count;++i) result.push_back(blobToString(array.items[i]));
    peare_resource_conversion_array_free(&array); return result;
}

QVector<QByteArray> Resource::convert(const QString& extension) const
{
    QVector<QByteArray> result; peare_blob_array array{}; const QByteArray ext=extension.toUtf8();
    if (!handle_ || peare_resource_convert(handle_, ext.constData(), &array) != PEARE_STATUS_OK) return result;
    for (size_t i=0;i<array.count;++i) result.push_back(QByteArray(reinterpret_cast<const char*>(array.items[i].bytes), int(array.items[i].length)));
    peare_resource_conversion_array_free(&array); return result;
}

ResourceContext Resource::context() const
{
    ResourceContext result;
    peare_resource_context context{};
    if (!handle_ || peare_resource_get_context(handle_, &context) != PEARE_STATUS_OK) return result;
    result.containerFormat = context.container_format;
    result.platform = context.platform;
    result.sourceName = blobToString(context.source_name_utf8);
    result.type = blobToString(context.type_utf8);
    result.identifier = blobToString(context.identifier_utf8);
    result.language = blobToString(context.language_utf8);
    result.codepage = context.codepage;
    result.dataOffset = context.data_offset;
    result.dataSize = context.data_size;
    result.isContainer = context.is_container != 0;
    peare_resource_context_free(&context);
    return result;
}

std::unique_ptr<Session> Session::openNested(size_t folder, size_t resource) const
{
    if (!handle_) return nullptr;
    peare_resource_handle rh = nullptr;
    if (peare_opener_open_resource_at(handle_, folder, resource, &rh) != PEARE_STATUS_OK || !rh)
        return nullptr;
    peare_source_handle src = nullptr;
    std::unique_ptr<Session> child(new Session());
    if (peare_resource_get_source(rh, &src) == PEARE_STATUS_OK && src) {
        child->open_ = (peare_opener_open(child->handle_, src) == PEARE_STATUS_OK);
        peare_source_destroy(src);
    }
    peare_resource_destroy(rh);
    // Only surface it if it actually opened as something navigable.
    if (!child->open_ || child->folderCount() == 0)
        return nullptr;
    return child;
}

Session::Session() { peare_opener_create(&handle_); }
Session::~Session() { if (handle_) peare_opener_destroy(handle_); }
bool Session::openFile(const QString& path, QString* error)
{
    if (!handle_ && peare_opener_create(&handle_) != PEARE_STATUS_OK) return false;
    const QByteArray utf8 = path.toUtf8();
    const peare_status status = peare_opener_open_file(handle_, utf8.constData());
    open_ = status == PEARE_STATUS_OK;
    if (!open_ && error) *error = statusText(status);
    return open_;
}
void Session::close() { if (handle_) peare_opener_close(handle_); open_ = false; }
peare_container_format Session::containerFormat() const
{
    peare_container_format format = PEARE_CONTAINER_UNKNOWN;
    return handle_ && peare_opener_get_container_format(handle_, &format) == PEARE_STATUS_OK
        ? format : PEARE_CONTAINER_UNKNOWN;
}
QString Session::description() const
{
    peare_blob b{};
    if (!handle_ || peare_opener_get_description(handle_, &b) != PEARE_STATUS_OK) return {};
    const QString result = blobToString(b);
    peare_blob_free(&b);
    return result;
}
size_t Session::folderCount() const { size_t n=0; return handle_ && peare_opener_get_folder_count(handle_,&n)==PEARE_STATUS_OK ? n : 0; }
QString Session::folderType(size_t folder) const
{
    peare_blob b{}; if (!handle_ || peare_opener_get_folder_type(handle_,folder,&b)!=PEARE_STATUS_OK) return {};
    const QString s=blobToString(b); peare_blob_free(&b); return s;
}
size_t Session::resourceCount(size_t folder) const { size_t n=0; return handle_ && peare_opener_get_resource_count(handle_,folder,&n)==PEARE_STATUS_OK ? n : 0; }
Resource Session::openResource(size_t folder, size_t resource) const
{
    peare_resource_handle h=nullptr;
    if (!handle_ || peare_opener_open_resource_at(handle_,folder,resource,&h)!=PEARE_STATUS_OK) return {};
    return Resource(h);
}

Resource Session::findResource(const QString& type, const QString& identifier, const QString& language) const
{
    peare_resource_handle h = nullptr;
    const QByteArray t = type.toUtf8(), i = identifier.toUtf8(), l = language.toUtf8();
    if (!handle_ || peare_opener_find_resource(handle_, t.constData(), i.constData(),
            l.isEmpty() ? nullptr : l.constData(), &h) != PEARE_STATUS_OK) return {};
    return Resource(h);
}

static QVector<quint16> groupChildIds(const QByteArray& payload, bool cursor)
{
    QVector<quint16> ids;
    if (payload.size() < 6) return ids;
    const quint16 count = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(payload.constData()+4));
    const int stride = cursor ? 14 : 14;
    for (quint16 n=0; n<count; ++n) {
        const int off = 6 + int(n)*stride;
        if (off + stride > payload.size()) break;
        ids.push_back(qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(payload.constData()+off+12)));
    }
    return ids;
}

Preview decode(const Resource& resource, const Session* session)
{
    Preview result;
    if (!resource.isValid()) { result.error=QStringLiteral("Invalid resource"); return result; }
    const QByteArray payload = resource.payload();
    const ResourceContext context = resource.context();
    const QByteArray platform = platformContextUtf8(context.platform);
    const QByteArray type = context.type.toUtf8();
    const char* platformArg = platform.isEmpty() ? nullptr : platform.constData();
    const char* typeArg = type.isEmpty() ? nullptr : type.constData();
    peare_blob_array pngs{};
    const peare_status imageStatus=peare_decode_images(
        reinterpret_cast<const uint8_t*>(payload.constData()), static_cast<size_t>(payload.size()),
        platformArg, typeArg, &pngs);
    if (imageStatus==PEARE_STATUS_OK) {
        for (size_t i=0;i<pngs.count;++i) {
            QByteArray png(reinterpret_cast<const char*>(pngs.items[i].bytes),static_cast<int>(pngs.items[i].length));
            QImage image; image.loadFromData(png,"PNG");
            if (!image.isNull()) { result.pngs.push_back(png); result.images.push_back(image); }
        }
    }
    peare_blob_array texts{};
    const peare_status textStatus=peare_decode_texts(
        reinterpret_cast<const uint8_t*>(payload.constData()), static_cast<size_t>(payload.size()),
        platformArg, typeArg, &texts);
    if (textStatus==PEARE_STATUS_OK) {
        QStringList parts;
        for (size_t i=0;i<texts.count;++i) {
            QByteArray text(reinterpret_cast<const char*>(texts.items[i].bytes),static_cast<int>(texts.items[i].length));
            result.texts.push_back(text); parts.push_back(QString::fromUtf8(text));
        }
        result.text=parts.join(QStringLiteral("\r\n"));
    }
    peare_blob_array_free(&pngs); peare_blob_array_free(&texts);

    const QString normalizedType = context.type.trimmed().toUpper();
    if (session && (normalizedType == QStringLiteral("RT_GROUP_ICON") || normalizedType == QStringLiteral("RT_GROUP_CURSOR"))) {
        const bool cursor = normalizedType == QStringLiteral("RT_GROUP_CURSOR");
        const QString childType = cursor ? QStringLiteral("RT_CURSOR") : QStringLiteral("RT_ICON");
        for (quint16 id : groupChildIds(payload, cursor)) {
            Resource child = session->findResource(childType, QStringLiteral("#%1").arg(id), context.language);
            if (!child.isValid()) child = session->findResource(childType, QStringLiteral("#%1").arg(id));
            if (!child.isValid()) continue;
            Preview decoded = decode(child, nullptr);
            result.images += decoded.images;
            result.pngs += decoded.pngs;
        }
    }

    if (result.images.isEmpty() && result.text.isEmpty() &&
        imageStatus!=PEARE_STATUS_NOT_APPLICABLE && textStatus!=PEARE_STATUS_NOT_APPLICABLE)
        result.error=statusText(imageStatus==PEARE_STATUS_DECODE_FAILED?imageStatus:textStatus);
    return result;
}

bool getUnsigned(const Resource& resource, peare_info_id id, quint64* value, const QString& resourceTypeOverride)
{
    if (!value) return false; *value=0; peare_value v{};
    const QByteArray payload = resource.payload();
    const ResourceContext context = resource.context();
    const QByteArray platform = platformContextUtf8(context.platform);
    const QByteArray type = (resourceTypeOverride.isEmpty() ? context.type : resourceTypeOverride).toUtf8();
    const peare_status s=peare_get_info(
        reinterpret_cast<const uint8_t*>(payload.constData()), static_cast<size_t>(payload.size()),
        platform.isEmpty() ? nullptr : platform.constData(),
        type.isEmpty() ? nullptr : type.constData(), id, &v);
    if (s!=PEARE_STATUS_OK || v.type!=PEARE_VALUE_UINT64) { peare_value_free(&v); return false; }
    *value=v.value.unsigned_value; peare_value_free(&v); return true;
}

QImage renderFont(const Resource& resource, const QString& text, quint32 scale, quint32 padding,
                  quint32 foregroundRgba, quint32 backgroundRgba,
                  const QString& resourceTypeOverride)
{
    peare_font_render_options options{scale,padding,foregroundRgba,backgroundRgba};
    peare_blob png{}; const QByteArray utf8=text.toUtf8();
    const QByteArray payload = resource.payload();
    const ResourceContext context = resource.context();
    const QByteArray platform = platformContextUtf8(context.platform);
    const QByteArray type = (resourceTypeOverride.isEmpty() ? context.type : resourceTypeOverride).toUtf8();
    if (peare_font_render(
            reinterpret_cast<const uint8_t*>(payload.constData()), static_cast<size_t>(payload.size()),
            platform.isEmpty() ? nullptr : platform.constData(),
            type.isEmpty() ? nullptr : type.constData(),
            utf8.constData(), &options, &png)!=PEARE_STATUS_OK) return {};
    QImage image; image.loadFromData(png.bytes,static_cast<int>(png.length),"PNG"); peare_blob_free(&png); return image;
}

} // namespace pearegui
