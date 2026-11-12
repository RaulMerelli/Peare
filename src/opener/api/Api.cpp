#include "peare/peare_opener.h"
#include "opener/OpenerSession.h"
#include "opener/api/PeareResourceSnapshot.h"
#include "opener/PeImageExport.h"
#include <QByteArray>
#include <QString>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
struct SessionState { peare::OpenerSession session; };
struct peare_opener_handle_s {
    std::shared_ptr<SessionState> state;
    QString password;
};

// A source to be opened. Exactly one backing is used: a file path, a lazy
// layer-backed store (a resource's content window), or an in-memory array.
// peare_opener_open consumes it; the caller cannot tell which backing it holds.
struct peare_source_handle_s {
    bool isFile = false;
    QString filePath;
    peare::fs::ByteStorePtr store;
    QByteArray bytes;
    QString name;
    QString subPath;  // non-empty: reopen `store` (a filesystem image) at this dir
};

namespace {
peare_status copyBytes(const char* data, size_t length, peare_blob* out) {
    if (!out) return PEARE_STATUS_INVALID_ARGUMENT;
    out->bytes=nullptr; out->length=0;
    if (!length) return PEARE_STATUS_OK;
    auto* copy=static_cast<uint8_t*>(std::malloc(length));
    if (!copy) return PEARE_STATUS_ALLOCATION_FAILED;
    std::memcpy(copy,data,length); out->bytes=copy; out->length=length; return PEARE_STATUS_OK;
}
peare_status copyStore(const peare::fs::IByteStore& store, peare_blob* out) {
    if (!out) return PEARE_STATUS_INVALID_ARGUMENT;
    out->bytes = nullptr; out->length = 0;
    const std::int64_t capacity = store.capacity();
    if (capacity < 0 || static_cast<std::uint64_t>(capacity) >
        std::numeric_limits<size_t>::max()) return PEARE_STATUS_ALLOCATION_FAILED;
    const size_t length = static_cast<size_t>(capacity);
    if (!length)
        return store.validate() ? PEARE_STATUS_OK : PEARE_STATUS_INTEGRITY_FAILED;
    auto* copy = static_cast<std::uint8_t*>(std::malloc(length));
    if (!copy) return PEARE_STATUS_ALLOCATION_FAILED;
    if (!store.copyTo(copy, length)) {
        std::free(copy);
        return PEARE_STATUS_INTEGRITY_FAILED;
    }
    out->bytes = copy; out->length = length;
    return PEARE_STATUS_OK;
}
peare_status copyByteArray(const QByteArray& bytes, peare_blob* out) { return copyBytes(bytes.constData(),static_cast<size_t>(bytes.size()),out); }
peare_status copyUtf8(const QString& text, peare_blob* out) { return copyByteArray(text.toUtf8(),out); }
peare_container_format toCFormat(peare::ModuleFormat f)
{
    switch (f) {
    case peare::ModuleFormat::DosMZ: return PEARE_CONTAINER_DOS_MZ;
    case peare::ModuleFormat::PE: return PEARE_CONTAINER_PE;
    case peare::ModuleFormat::NE: return PEARE_CONTAINER_NE;
    case peare::ModuleFormat::LE: return PEARE_CONTAINER_LE;
    case peare::ModuleFormat::LX: return PEARE_CONTAINER_LX;
    case peare::ModuleFormat::XEX: return PEARE_CONTAINER_XEX;
    case peare::ModuleFormat::XBE: return PEARE_CONTAINER_XBE;
    case peare::ModuleFormat::XUIZ: return PEARE_CONTAINER_XUIZ;
    case peare::ModuleFormat::LIVE_PIRS: return PEARE_CONTAINER_LIVE_PIRS;
    case peare::ModuleFormat::CON: return PEARE_CONTAINER_CON;
    case peare::ModuleFormat::OS2_PACK: return PEARE_CONTAINER_OS2_PACK;
    case peare::ModuleFormat::SZDD: return PEARE_CONTAINER_SZDD;
    case peare::ModuleFormat::KWAJ: return PEARE_CONTAINER_KWAJ;
    case peare::ModuleFormat::XBCOMPRESS: return PEARE_CONTAINER_XBCOMPRESS;
    case peare::ModuleFormat::NSIS: return PEARE_CONTAINER_NSIS;
    case peare::ModuleFormat::SIEMENS_IMG: return PEARE_CONTAINER_SIEMENS_IMG;
    case peare::ModuleFormat::SIEMENS_ADONIS: return PEARE_CONTAINER_SIEMENS_ADONIS;
    case peare::ModuleFormat::GZIP: return PEARE_CONTAINER_GZIP;
    case peare::ModuleFormat::XZ: return PEARE_CONTAINER_XZ;
    case peare::ModuleFormat::LZIP: return PEARE_CONTAINER_LZIP;
    case peare::ModuleFormat::ZSTD: return PEARE_CONTAINER_ZSTD;
    case peare::ModuleFormat::ARJ: return PEARE_CONTAINER_ARJ;
    case peare::ModuleFormat::XAR: return PEARE_CONTAINER_XAR;
    case peare::ModuleFormat::LZMA: return PEARE_CONTAINER_LZMA;
    case peare::ModuleFormat::BZIP2: return PEARE_CONTAINER_BZIP2;
    case peare::ModuleFormat::AR: return PEARE_CONTAINER_AR;
    case peare::ModuleFormat::CPIO: return PEARE_CONTAINER_CPIO;
    case peare::ModuleFormat::UNIX_COMPRESS: return PEARE_CONTAINER_UNIX_COMPRESS;
    case peare::ModuleFormat::UUE: return PEARE_CONTAINER_UUE;
    case peare::ModuleFormat::INTEL_HEX: return PEARE_CONTAINER_INTEL_HEX;
    case peare::ModuleFormat::DEB: return PEARE_CONTAINER_DEB;
    case peare::ModuleFormat::RPM: return PEARE_CONTAINER_RPM;
    case peare::ModuleFormat::CHM: return PEARE_CONTAINER_CHM;
    case peare::ModuleFormat::LHA: return PEARE_CONTAINER_LHA;
    case peare::ModuleFormat::SEVEN_ZIP: return PEARE_CONTAINER_SEVEN_ZIP;
    case peare::ModuleFormat::RAR4: return PEARE_CONTAINER_RAR4;
    case peare::ModuleFormat::RAR5: return PEARE_CONTAINER_RAR5;
    case peare::ModuleFormat::SIEMENS_FWF: return PEARE_CONTAINER_SIEMENS_FWF;
    case peare::ModuleFormat::SIEMENS_FSF: return PEARE_CONTAINER_SIEMENS_FSF;
    case peare::ModuleFormat::ISO9660: return PEARE_CONTAINER_ISO9660;
    case peare::ModuleFormat::WIM: return PEARE_CONTAINER_WIM;
    case peare::ModuleFormat::FAT: return PEARE_CONTAINER_FAT;
    case peare::ModuleFormat::UDF: return PEARE_CONTAINER_UDF;
    case peare::ModuleFormat::UFS: return PEARE_CONTAINER_UFS;
    case peare::ModuleFormat::EXFAT: return PEARE_CONTAINER_EXFAT;
    case peare::ModuleFormat::VMDK: return PEARE_CONTAINER_VMDK;
    case peare::ModuleFormat::VHD: return PEARE_CONTAINER_VHD;
    case peare::ModuleFormat::VDI: return PEARE_CONTAINER_VDI;
    case peare::ModuleFormat::QCOW: return PEARE_CONTAINER_QCOW;
    case peare::ModuleFormat::QCOW2: return PEARE_CONTAINER_QCOW2;
    case peare::ModuleFormat::VHDX: return PEARE_CONTAINER_VHDX;
    case peare::ModuleFormat::SDI: return PEARE_CONTAINER_SDI;
    case peare::ModuleFormat::XVA: return PEARE_CONTAINER_XVA;
    case peare::ModuleFormat::SWAP: return PEARE_CONTAINER_SWAP;
    case peare::ModuleFormat::LVM: return PEARE_CONTAINER_LVM;
    case peare::ModuleFormat::EXT: return PEARE_CONTAINER_EXT;
    case peare::ModuleFormat::NTFS: return PEARE_CONTAINER_NTFS;
    case peare::ModuleFormat::XFS: return PEARE_CONTAINER_XFS;
    case peare::ModuleFormat::JFS: return PEARE_CONTAINER_JFS;
    case peare::ModuleFormat::HPFS: return PEARE_CONTAINER_HPFS;
    case peare::ModuleFormat::SQUASHFS: return PEARE_CONTAINER_SQUASHFS;
    case peare::ModuleFormat::CRAMFS: return PEARE_CONTAINER_CRAMFS;
    case peare::ModuleFormat::HFSPLUS: return PEARE_CONTAINER_HFSPLUS;
    case peare::ModuleFormat::DMG: return PEARE_CONTAINER_DMG;
    case peare::ModuleFormat::BTRFS: return PEARE_CONTAINER_BTRFS;
    case peare::ModuleFormat::REGISTRY: return PEARE_CONTAINER_REGISTRY;
    case peare::ModuleFormat::BOOTCONFIG: return PEARE_CONTAINER_BOOTCONFIG;
    case peare::ModuleFormat::CAB: return PEARE_CONTAINER_CAB;
    case peare::ModuleFormat::RAW_DISK: return PEARE_CONTAINER_RAW_DISK;
    case peare::ModuleFormat::ANDROID_SPARSE: return PEARE_CONTAINER_ANDROID_SPARSE;
    case peare::ModuleFormat::ANDROID_SUPER: return PEARE_CONTAINER_ANDROID_SUPER;
    case peare::ModuleFormat::XNB: return PEARE_CONTAINER_XNB;
    case peare::ModuleFormat::DXBC: return PEARE_CONTAINER_DXBC;
    case peare::ModuleFormat::NUP: return PEARE_CONTAINER_NUP;
    case peare::ModuleFormat::XDBF: return PEARE_CONTAINER_XDBF;
    case peare::ModuleFormat::ANDROID_APK: return PEARE_CONTAINER_ANDROID_APK;
    case peare::ModuleFormat::ZIP: return PEARE_CONTAINER_ZIP;
    case peare::ModuleFormat::TAR: return PEARE_CONTAINER_TAR;
    case peare::ModuleFormat::ASAR: return PEARE_CONTAINER_ASAR;
    case peare::ModuleFormat::LINUX_RAID: return PEARE_CONTAINER_LINUX_RAID;
    case peare::ModuleFormat::DYNAMIC_DISK: return PEARE_CONTAINER_DYNAMIC_DISK;
    case peare::ModuleFormat::WINCE_ROM: return PEARE_CONTAINER_WINCE_ROM;
    case peare::ModuleFormat::FFU: return PEARE_CONTAINER_FFU;
    case peare::ModuleFormat::OS2_EA: return PEARE_CONTAINER_OS2_EA;
    case peare::ModuleFormat::RESX: return PEARE_CONTAINER_RESX;
    case peare::ModuleFormat::CUE_BIN: return PEARE_CONTAINER_CUE_BIN;
    case peare::ModuleFormat::FLOPPY_IMAGE: return PEARE_CONTAINER_FLOPPY_IMAGE;
    case peare::ModuleFormat::PS3_PUP: return PEARE_CONTAINER_PS3_PUP;
    case peare::ModuleFormat::WAD: return PEARE_CONTAINER_WAD;
    case peare::ModuleFormat::MTF: return PEARE_CONTAINER_MTF;
    case peare::ModuleFormat::MDF_MDS: return PEARE_CONTAINER_MDF_MDS;
    case peare::ModuleFormat::PARALLELS_HDD: return PEARE_CONTAINER_PARALLELS_HDD;
    case peare::ModuleFormat::PS2_ROMDIR: return PEARE_CONTAINER_PS2_ROMDIR;
    case peare::ModuleFormat::MSI: return PEARE_CONTAINER_MSI;
    case peare::ModuleFormat::MSG: return PEARE_CONTAINER_MSG;
    case peare::ModuleFormat::MAC_RESOURCE_FORK: return PEARE_CONTAINER_RESOURCE_FORK;
    case peare::ModuleFormat::WPI: return PEARE_CONTAINER_WPI;
    case peare::ModuleFormat::APPX: return PEARE_CONTAINER_APPX;
    case peare::ModuleFormat::MSIX: return PEARE_CONTAINER_MSIX;
    case peare::ModuleFormat::APPXBUNDLE: return PEARE_CONTAINER_APPXBUNDLE;
    case peare::ModuleFormat::EAPPX: return PEARE_CONTAINER_EAPPX;
    case peare::ModuleFormat::EMSIX: return PEARE_CONTAINER_EMSIX;
    case peare::ModuleFormat::EAPPXBUNDLE: return PEARE_CONTAINER_EAPPXBUNDLE;
    case peare::ModuleFormat::EMSIXBUNDLE: return PEARE_CONTAINER_EMSIXBUNDLE;
    case peare::ModuleFormat::XAP: return PEARE_CONTAINER_XAP;
    case peare::ModuleFormat::Unknown: return PEARE_CONTAINER_UNKNOWN;
    }
    return PEARE_CONTAINER_UNKNOWN;
}
peare_platform toCPlatform(peare::ResourcePlatform p) {
 switch(p){case peare::ResourcePlatform::Windows:return PEARE_PLATFORM_WINDOWS;case peare::ResourcePlatform::Os2:return PEARE_PLATFORM_OS2;case peare::ResourcePlatform::Other:return PEARE_PLATFORM_OTHER;default:return PEARE_PLATFORM_UNKNOWN;}}
void clearContext(peare_resource_context* c){if(c)std::memset(c,0,sizeof(*c));}


void freeSnapshotItem(peare_resource_snapshot_item* item)
{
    if (!item) return;
    std::free(item->payload.bytes);
    item->payload = {};
    std::free(item->context.source_name_utf8.bytes);
    std::free(item->context.type_utf8.bytes);
    std::free(item->context.identifier_utf8.bytes);
    std::free(item->context.language_utf8.bytes);
    std::free(item->archive_path_utf8.bytes);
    item->archive_path_utf8 = {};
    delete static_cast<peare::fs::ByteStorePtr*>(item->lazy_content);
    item->lazy_content = nullptr;
    std::free(item->sub_path);
    item->sub_path = nullptr;
    item->is_directory = 0;
    item->context = {};
}

peare_status fillSnapshotItem(const peare::OpenedResource& opened,
                              peare_resource_snapshot_item* item)
{
    if (!item || !opened.isValid()) return PEARE_STATUS_INVALID_ARGUMENT;
    *item = {};
    peare_status status = copyByteArray(opened.payload, &item->payload);
    if (status != PEARE_STATUS_OK) return status;
    // Layer-backed content is not read here: keep only the store, so building
    // this item (and the whole sibling list) costs nothing in bytes. The content
    // is materialised on demand in peare_resource_get_payload.
    if (opened.contentStore)
        item->lazy_content = new (std::nothrow) peare::fs::ByteStorePtr(opened.contentStore);
    item->is_directory = opened.isDirectory ? 1 : 0;
    if (opened.isDirectory) {
        const QByteArray sp = opened.subPath.toUtf8();
        item->sub_path = static_cast<char*>(std::malloc(static_cast<size_t>(sp.size()) + 1));
        if (item->sub_path) {
            std::memcpy(item->sub_path, sp.constData(), static_cast<size_t>(sp.size()));
            item->sub_path[sp.size()] = '\0';
        }
    }
    const auto& source = opened.context;
    item->context.container_format = toCFormat(source.containerFormat);
    item->context.platform = toCPlatform(source.platform);
    item->context.codepage = source.codePage;
    item->context.data_offset = source.dataOffset;
    item->context.data_size = source.dataSize;
    item->context.base_id = source.baseId;
    item->context.resource_index = source.resourceIndex;
    item->context.is_container = source.isContainer ? 1 : 0;
    status = copyUtf8(source.sourceName, &item->context.source_name_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.type, &item->context.type_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.identifier, &item->context.identifier_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.language, &item->context.language_utf8);
    if (status == PEARE_STATUS_OK)
        status = copyUtf8(opened.archivePath, &item->archive_path_utf8);
    if (status != PEARE_STATUS_OK) freeSnapshotItem(item);
    return status;
}

void destroySnapshot(peare_resource_handle_s* handle)
{
    if (!handle) return;
    freeSnapshotItem(&handle->primary);
    for (size_t i = 0; i < handle->related_count; ++i)
        freeSnapshotItem(&handle->related[i]);
    std::free(handle->related);
    handle->related = nullptr;
    handle->related_count = 0;
}

peare_status makeResourceHandle(const std::shared_ptr<SessionState>& state,
                                int resourceIndex,
                                peare_resource_handle* out_resource)
{
    if (!out_resource) return PEARE_STATUS_INVALID_ARGUMENT;
    *out_resource = nullptr;
    if (!state) return PEARE_STATUS_INVALID_HANDLE;
    const peare::OpenedResource primary = state->session.openResource(resourceIndex);
    if (!primary.isValid()) return PEARE_STATUS_NOT_FOUND;

    auto* handle = new (std::nothrow) peare_resource_handle_s{};
    if (!handle) return PEARE_STATUS_ALLOCATION_FAILED;
    handle->magic = PEARE_RESOURCE_SNAPSHOT_MAGIC;
    handle->version = PEARE_RESOURCE_SNAPSHOT_VERSION;

    const peare_status status = fillSnapshotItem(primary, &handle->primary);
    if (status != PEARE_STATUS_OK) {
        destroySnapshot(handle); delete handle; return status;
    }
    // Do not build the unused related snapshot for each resource.
    *out_resource = handle;
    return PEARE_STATUS_OK;
}
}

extern "C" {
peare_status peare_opener_create(peare_opener_handle* out_opener)
{
    if (!out_opener)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_opener = nullptr;
    try {
        auto* handle = new peare_opener_handle_s;
        handle->state = std::make_shared<SessionState>();
        *out_opener = handle;
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

void peare_opener_destroy(peare_opener_handle opener)
{
    if (opener) opener->password.fill(QChar(0));
    delete opener;
}

peare_status peare_opener_set_password(peare_opener_handle opener,
                                       const char* password_utf8)
{
    if (!opener || !opener->state) return PEARE_STATUS_INVALID_HANDLE;
    opener->password.fill(QChar(0));
    opener->password = password_utf8 ? QString::fromUtf8(password_utf8) : QString();
    return PEARE_STATUS_OK;
}

// ---- Unified open --------------------------------------------------------

// Internal only: building a file source is not part of the public ABI (external
// callers open files by path via peare_opener_open_file). Kept as a helper for
// that shim.
static peare_status makeFileSource(const char* path_utf8, peare_source_handle* out_source)
{
    if (!out_source) return PEARE_STATUS_INVALID_ARGUMENT;
    *out_source = nullptr;
    if (!path_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    auto* s = new (std::nothrow) peare_source_handle_s{};
    if (!s) return PEARE_STATUS_ALLOCATION_FAILED;
    s->isFile = true;
    s->filePath = QString::fromUtf8(path_utf8);
    s->name = s->filePath;
    *out_source = s;
    return PEARE_STATUS_OK;
}

peare_status peare_resource_get_source(peare_resource_handle resource, peare_source_handle* out_source)
{
    if (!out_source) return PEARE_STATUS_INVALID_ARGUMENT;
    *out_source = nullptr;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    auto* s = new (std::nothrow) peare_source_handle_s{};
    if (!s) return PEARE_STATUS_ALLOCATION_FAILED;
    const peare_resource_snapshot_item& it = resource->primary;
    // Prefer the lazy store (no materialisation here); fall back to the array.
    if (it.lazy_content) {
        auto* store = static_cast<peare::fs::ByteStorePtr*>(it.lazy_content);
        if (store) s->store = *store;
    }
    // Directory container: reopen the fs image (in the store) at its subpath.
    if (it.is_directory && it.sub_path)
        s->subPath = QString::fromUtf8(it.sub_path);
    if (!s->store && !it.is_directory)
        s->bytes = QByteArray(reinterpret_cast<const char*>(it.payload.bytes),
                              static_cast<int>(it.payload.length));
    s->name = it.context.identifier_utf8.bytes
        ? QString::fromUtf8(reinterpret_cast<const char*>(it.context.identifier_utf8.bytes),
                            static_cast<int>(it.context.identifier_utf8.length))
        : QStringLiteral("resource.bin");
    *out_source = s;
    return PEARE_STATUS_OK;
}

peare_status peare_opener_open(peare_opener_handle opener, peare_source_handle source)
{
    if (!opener || !opener->state) return PEARE_STATUS_INVALID_HANDLE;
    if (!source) return PEARE_STATUS_INVALID_ARGUMENT;
    if (source->store && !source->store->validate())
        return PEARE_STATUS_INTEGRITY_FAILED;
    try {
        auto next = std::make_shared<SessionState>();
        bool opened = false;
        if (source->isFile) {
            opened = next->session.openFile(source->filePath, opener->password);
        } else if (source->store) {
            // Open the positioned source without copying the full nested image.
            // A subPath selects one directory for lazy file-system enumeration.
            opened = next->session.openStore(source->store, source->name, source->subPath,
                                             opener->password);
        } else {
            opened = next->session.openBuffer(source->bytes, source->name, opener->password);
        }
        opener->state = std::move(next);
        if (opened) return PEARE_STATUS_OK;
        switch (opener->state->session.info().openError) {
        case peare::ModuleOpenError::PasswordRequired:
            return PEARE_STATUS_PASSWORD_REQUIRED;
        case peare::ModuleOpenError::PasswordInvalidOrCorrupt:
            return PEARE_STATUS_PASSWORD_INVALID;
        case peare::ModuleOpenError::None:
            return PEARE_STATUS_OPEN_FAILED;
        }
        return PEARE_STATUS_OPEN_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

void peare_source_destroy(peare_source_handle source)
{
    delete source;
}

// ---- Convenience shims over the unified path -----------------------------

peare_status peare_opener_open_file(peare_opener_handle opener, const char* path_utf8)
{
    peare_source_handle src = nullptr;
    peare_status st = makeFileSource(path_utf8, &src);
    if (st != PEARE_STATUS_OK) return st;
    st = peare_opener_open(opener, src);
    peare_source_destroy(src);
    return st;
}

peare_status peare_opener_close(peare_opener_handle opener)
{
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    try {
        opener->password.fill(QChar(0));
        opener->password = QString();
        opener->state = std::make_shared<SessionState>();
        return PEARE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

peare_status peare_opener_get_container_format(peare_opener_handle opener,
                                                peare_container_format* out_format)
{
    if (!out_format)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_format = PEARE_CONTAINER_UNKNOWN;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    if (!opener->state->session.isOpen())
        return PEARE_STATUS_NOT_FOUND;
    *out_format = toCFormat(opener->state->session.info().format);
    return PEARE_STATUS_OK;
}

peare_status peare_opener_get_description(peare_opener_handle opener,
                                             peare_blob* out_description_utf8)
{
    if (!out_description_utf8)
        return PEARE_STATUS_INVALID_ARGUMENT;
    out_description_utf8->bytes = nullptr;
    out_description_utf8->length = 0;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    if (!opener->state->session.isOpen())
        return PEARE_STATUS_NOT_OPEN;
    return copyUtf8(opener->state->session.info().description, out_description_utf8);
}

peare_status peare_opener_get_folder_count(peare_opener_handle opener, size_t* out_count)
{
    if (!out_count)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_count = 0;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    if (!opener->state->session.isOpen())
        return PEARE_STATUS_NOT_OPEN;
    *out_count = static_cast<size_t>(opener->state->session.folders().size());
    return PEARE_STATUS_OK;
}

peare_status peare_opener_get_folder_type(peare_opener_handle opener,
                                          size_t folder_index,
                                          peare_blob* out_type_utf8)
{
    if (!out_type_utf8)
        return PEARE_STATUS_INVALID_ARGUMENT;
    out_type_utf8->bytes = nullptr;
    out_type_utf8->length = 0;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    const auto& folders = opener->state->session.folders();
    if (folder_index >= static_cast<size_t>(folders.size()))
        return PEARE_STATUS_OUT_OF_RANGE;
    return copyUtf8(folders.at(static_cast<int>(folder_index)).type, out_type_utf8);
}

peare_status peare_opener_get_resource_count(peare_opener_handle opener,
                                             size_t folder_index,
                                             size_t* out_count)
{
    if (!out_count)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_count = 0;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    const auto& folders = opener->state->session.folders();
    if (folder_index >= static_cast<size_t>(folders.size()))
        return PEARE_STATUS_OUT_OF_RANGE;
    *out_count = static_cast<size_t>(folders.at(static_cast<int>(folder_index)).resourceIndices.size());
    return PEARE_STATUS_OK;
}

peare_status peare_opener_open_resource_at(peare_opener_handle opener,
                                           size_t folder_index,
                                           size_t resource_index,
                                           peare_resource_handle* out_resource)
{
    if (!out_resource)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_resource = nullptr;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;

    const auto& folders = opener->state->session.folders();
    if (folder_index >= static_cast<size_t>(folders.size()))
        return PEARE_STATUS_OUT_OF_RANGE;
    const auto& indices = folders.at(static_cast<int>(folder_index)).resourceIndices;
    if (resource_index >= static_cast<size_t>(indices.size()))
        return PEARE_STATUS_OUT_OF_RANGE;

    try {
        return makeResourceHandle(opener->state, indices.at(static_cast<int>(resource_index)), out_resource);
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

peare_status peare_opener_find_resource(peare_opener_handle opener,
                                        const char* type_utf8,
                                        const char* identifier_utf8,
                                        const char* preferred_language_utf8,
                                        peare_resource_handle* out_resource)
{
    if (!out_resource || !type_utf8 || !identifier_utf8)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_resource = nullptr;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;

    try {
        const int index = opener->state->session.findResource(
            QString::fromUtf8(type_utf8),
            QString::fromUtf8(identifier_utf8),
            preferred_language_utf8 ? QString::fromUtf8(preferred_language_utf8) : QString());
        if (index < 0)
            return PEARE_STATUS_NOT_FOUND;
        return makeResourceHandle(opener->state, index, out_resource);
    } catch (const std::bad_alloc&) {
        return PEARE_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

void peare_resource_destroy(peare_resource_handle resource)
{
    if (!peare_resource_snapshot_valid(resource)) return;
    destroySnapshot(resource);
    resource->magic = 0;
    delete resource;
}

peare_status peare_resource_get_payload(peare_resource_handle resource, peare_blob* out_payload)
{
    if (!out_payload) return PEARE_STATUS_INVALID_ARGUMENT;
    out_payload->bytes = nullptr; out_payload->length = 0;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    // Directory containers have no file payload; their store is the full image.
    // Open the directory as a container instead of materializing the image.
    if (resource->primary.is_directory) return PEARE_STATUS_OK;
    // Materialize one layer-backed file only when the caller requests its bytes.
    if (resource->primary.lazy_content) {
        auto* store = static_cast<peare::fs::ByteStorePtr*>(resource->primary.lazy_content);
        if (store && *store) {
            return copyStore(**store, out_payload);
        }
    }
    return copyBytes(reinterpret_cast<const char*>(resource->primary.payload.bytes),
                     resource->primary.payload.length, out_payload);
}

peare_status peare_resource_read(peare_resource_handle resource, std::uint64_t offset,
                                 std::uint8_t* buffer, std::size_t capacity,
                                 std::size_t* out_read)
{
    if (!out_read || (capacity && !buffer)) return PEARE_STATUS_INVALID_ARGUMENT;
    *out_read = 0;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    if (resource->primary.is_directory) return PEARE_STATUS_NOT_APPLICABLE;
    const std::size_t amount = std::min<std::size_t>(capacity, INT_MAX);
    if (resource->primary.lazy_content) {
        auto* store = static_cast<peare::fs::ByteStorePtr*>(resource->primary.lazy_content);
        if (!store || !*store) return PEARE_STATUS_INTERNAL_ERROR;
        const std::int64_t size = (*store)->capacity();
        if (size < 0) return PEARE_STATUS_DECODE_FAILED;
        if (offset > std::uint64_t(size)) return PEARE_STATUS_OUT_OF_RANGE;
        if (offset == std::uint64_t(size)) return PEARE_STATUS_OK;
        if (!amount) return PEARE_STATUS_OK;
        const int read = (*store)->read(std::int64_t(offset), buffer, int(amount));
        if (read < 0) return PEARE_STATUS_DECODE_FAILED;
        if (!read) return PEARE_STATUS_INTEGRITY_FAILED;
        *out_read = std::size_t(read);
        return PEARE_STATUS_OK;
    }
    const std::size_t size = resource->primary.payload.length;
    if (offset > size) return PEARE_STATUS_OUT_OF_RANGE;
    if (offset == size) return PEARE_STATUS_OK;
    if (!amount) return PEARE_STATUS_OK;
    const std::size_t read = std::min(amount, size - std::size_t(offset));
    std::memcpy(buffer, resource->primary.payload.bytes + std::size_t(offset), read);
    *out_read = read;
    return PEARE_STATUS_OK;
}

peare_status peare_resource_get_converted_extensions(peare_resource_handle resource, peare_blob_array* out_extensions_utf8)
{
    if (!out_extensions_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    out_extensions_utf8->items = nullptr; out_extensions_utf8->count = 0;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    const auto& context = resource->primary.context;
    const QString type = QString::fromUtf8(reinterpret_cast<const char*>(context.type_utf8.bytes), int(context.type_utf8.length));
    if (type != QStringLiteral("PE_MODULE")) return PEARE_STATUS_NOT_APPLICABLE;
    auto* items = static_cast<peare_blob*>(std::calloc(1, sizeof(peare_blob)));
    if (!items) return PEARE_STATUS_ALLOCATION_FAILED;
    const peare_status status = copyBytes(".dll", 4, &items[0]);
    if (status != PEARE_STATUS_OK) { std::free(items); return status; }
    out_extensions_utf8->items = items; out_extensions_utf8->count = 1;
    return PEARE_STATUS_OK;
}

peare_status peare_resource_convert(peare_resource_handle resource, const char* extension_utf8, peare_blob_array* out_files)
{
    if (!out_files || !extension_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    out_files->items = nullptr; out_files->count = 0;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    const auto& context = resource->primary.context;
    const QString type = QString::fromUtf8(reinterpret_cast<const char*>(context.type_utf8.bytes), int(context.type_utf8.length));
    QString extension = QString::fromUtf8(extension_utf8).trimmed().toLower();
    const QByteArray source(reinterpret_cast<const char*>(resource->primary.payload.bytes), int(resource->primary.payload.length));
    if (type != QStringLiteral("PE_MODULE")) return PEARE_STATUS_NOT_APPLICABLE;
    if (extension != QStringLiteral(".dll") && extension != QStringLiteral(".exe")) return PEARE_STATUS_NOT_APPLICABLE;
    const peare::PeImageLayoutResult layout = peare::PeImageExport::classify(source);
    QByteArray converted;
    if (layout.kind == peare::PeImageLayoutKind::File ||
        layout.kind == peare::PeImageLayoutKind::Hybrid) {
        // A PE that is already a valid file image must be preserved.  Rebuilding it
        // as though RVAs were byte offsets corrupts the output or can fail silently.
        converted = source;
    } else if (layout.kind == peare::PeImageLayoutKind::LoadedImage) {
        QString error;
        converted = peare::PeImageExport::rebuildFileImage(source, &error);
    } else {
        return PEARE_STATUS_DECODE_FAILED;
    }
    if (converted.isEmpty()) return PEARE_STATUS_DECODE_FAILED;
    auto* items = static_cast<peare_blob*>(std::calloc(1, sizeof(peare_blob)));
    if (!items) return PEARE_STATUS_ALLOCATION_FAILED;
    const peare_status status = copyByteArray(converted, &items[0]);
    if (status != PEARE_STATUS_OK) { std::free(items); return status; }
    out_files->items = items; out_files->count = 1;
    return PEARE_STATUS_OK;
}

void peare_resource_conversion_array_free(peare_blob_array* array)
{
    if (!array) return;
    for (size_t i=0;i<array->count;++i) { std::free(array->items[i].bytes); array->items[i] = {}; }
    std::free(array->items); array->items=nullptr; array->count=0;
}

peare_status peare_resource_get_context(peare_resource_handle resource,
                                        peare_resource_context* out_context)
{
    if (!out_context) return PEARE_STATUS_INVALID_ARGUMENT;
    clearContext(out_context);
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    const peare_resource_context& source = resource->primary.context;
    out_context->container_format = source.container_format;
    out_context->platform = source.platform;
    out_context->codepage = source.codepage;
    out_context->data_offset = source.data_offset;
    out_context->data_size = source.data_size;
    out_context->base_id = source.base_id;
    out_context->resource_index = source.resource_index;
    out_context->is_container = source.is_container;
    peare_status status = copyBytes(reinterpret_cast<const char*>(source.source_name_utf8.bytes), source.source_name_utf8.length, &out_context->source_name_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.type_utf8.bytes), source.type_utf8.length, &out_context->type_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.identifier_utf8.bytes), source.identifier_utf8.length, &out_context->identifier_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.language_utf8.bytes), source.language_utf8.length, &out_context->language_utf8);
    if (status != PEARE_STATUS_OK) peare_resource_context_free(out_context);
    return status;
}

peare_status peare_resource_get_archive_path(peare_resource_handle resource,
                                             peare_blob* out_path_utf8)
{
    if (!out_path_utf8) return PEARE_STATUS_INVALID_ARGUMENT;
    out_path_utf8->bytes = nullptr;
    out_path_utf8->length = 0;
    if (!peare_resource_snapshot_valid(resource)) return PEARE_STATUS_INVALID_HANDLE;
    const peare_blob& path = resource->primary.archive_path_utf8;
    if (!path.length) return PEARE_STATUS_NOT_APPLICABLE;
    return copyBytes(reinterpret_cast<const char*>(path.bytes), path.length,
                     out_path_utf8);
}

void peare_blob_free(peare_blob* blob)
{
    if (!blob)
        return;
    std::free(blob->bytes);
    blob->bytes = nullptr;
    blob->length = 0;
}

void peare_resource_context_free(peare_resource_context* context)
{
    if (!context)
        return;
    peare_blob_free(&context->source_name_utf8);
    peare_blob_free(&context->type_utf8);
    peare_blob_free(&context->identifier_utf8);
    peare_blob_free(&context->language_utf8);
    clearContext(context);
}

const char* peare_status_message(peare_status status)
{
    switch (status) {
    case PEARE_STATUS_OK: return "Success";
    case PEARE_STATUS_INVALID_ARGUMENT: return "Invalid argument";
    case PEARE_STATUS_INVALID_HANDLE: return "Invalid handle";
    case PEARE_STATUS_NOT_OPEN: return "No file is open";
    case PEARE_STATUS_NOT_FOUND: return "Resource not found";
    case PEARE_STATUS_OUT_OF_RANGE: return "Index out of range";
    case PEARE_STATUS_OPEN_FAILED: return "Unable to open input";
    case PEARE_STATUS_ALLOCATION_FAILED: return "Allocation failed";
    case PEARE_STATUS_INTERNAL_ERROR: return "Internal error";
    case PEARE_STATUS_NOT_APPLICABLE: return "Operation not applicable to this resource";
    case PEARE_STATUS_DECODE_FAILED: return "Resource decoding failed";
    case PEARE_STATUS_VALUE_NOT_DETERMINABLE: return "Known value could not be determined";
    case PEARE_STATUS_UNSUPPORTED_INFO: return "Information identifier is not supported";
    case PEARE_STATUS_INTEGRITY_FAILED: return "Resource integrity verification failed";
    case PEARE_STATUS_PASSWORD_REQUIRED: return "A password is required";
    case PEARE_STATUS_PASSWORD_INVALID: return "The password is incorrect or the archive is damaged";
    }
    return "Unknown status";
}
}



