#include "peare/peare_opener.h"
#include "opener/OpenerSession.h"
#include "opener/api/PeareResourceSnapshot.h"
#include "opener/PeImageExport.h"
#include <QByteArray>
#include <QString>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
struct SessionState { peare::OpenerSession session; };
struct peare_opener_handle_s { std::shared_ptr<SessionState> state; };

namespace {
peare_status copyBytes(const char* data, size_t length, peare_blob* out) {
    if (!out) return PEARE_STATUS_INVALID_ARGUMENT;
    out->bytes=nullptr; out->length=0;
    if (!length) return PEARE_STATUS_OK;
    auto* copy=static_cast<uint8_t*>(std::malloc(length));
    if (!copy) return PEARE_STATUS_ALLOCATION_FAILED;
    std::memcpy(copy,data,length); out->bytes=copy; out->length=length; return PEARE_STATUS_OK;
}
peare_status copyByteArray(const QByteArray& bytes, peare_blob* out) { return copyBytes(bytes.constData(),static_cast<size_t>(bytes.size()),out); }
peare_status copyUtf8(const QString& text, peare_blob* out) { return copyByteArray(text.toUtf8(),out); }
peare_container_format toCFormat(peare::ModuleFormat f) {
 switch(f){case peare::ModuleFormat::DosMZ:return PEARE_CONTAINER_DOS_MZ;case peare::ModuleFormat::PE:return PEARE_CONTAINER_PE;case peare::ModuleFormat::NE:return PEARE_CONTAINER_NE;case peare::ModuleFormat::LE:return PEARE_CONTAINER_LE;case peare::ModuleFormat::LX:return PEARE_CONTAINER_LX;case peare::ModuleFormat::XEX:return PEARE_CONTAINER_XEX;case peare::ModuleFormat::XBE:return PEARE_CONTAINER_XBE;case peare::ModuleFormat::XUIZ:return PEARE_CONTAINER_XUIZ;case peare::ModuleFormat::LIVE_PIRS:return PEARE_CONTAINER_LIVE_PIRS;case peare::ModuleFormat::CON:return PEARE_CONTAINER_CON;case peare::ModuleFormat::OS2_PACK:return PEARE_CONTAINER_OS2_PACK;case peare::ModuleFormat::SZDD:return PEARE_CONTAINER_SZDD;case peare::ModuleFormat::SIEMENS_IMG:return PEARE_CONTAINER_SIEMENS_IMG;case peare::ModuleFormat::SIEMENS_FWF:return PEARE_CONTAINER_SIEMENS_FWF;case peare::ModuleFormat::ISO9660:return PEARE_CONTAINER_ISO9660;default:return PEARE_CONTAINER_UNKNOWN;}}
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
    delete static_cast<peare::fs::ByteStorePtr*>(item->lazy_content);
    item->lazy_content = nullptr;
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
    const auto& source = opened.context;
    item->context.container_format = toCFormat(source.containerFormat);
    item->context.platform = toCPlatform(source.platform);
    item->context.codepage = source.codePage;
    item->context.data_offset = source.dataOffset;
    item->context.data_size = source.dataSize;
    item->context.base_id = source.baseId;
    item->context.resource_index = source.resourceIndex;
    status = copyUtf8(source.sourceName, &item->context.source_name_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.type, &item->context.type_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.identifier, &item->context.identifier_utf8);
    if (status == PEARE_STATUS_OK) status = copyUtf8(source.language, &item->context.language_utf8);
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

    peare_status status = fillSnapshotItem(primary, &handle->primary);
    if (status != PEARE_STATUS_OK) {
        destroySnapshot(handle); delete handle; return status;
    }

    const int count = state->session.resourceCount();
    if (count > 1) {
        handle->related = static_cast<peare_resource_snapshot_item*>(
            std::calloc(static_cast<size_t>(count - 1), sizeof(peare_resource_snapshot_item)));
        if (!handle->related) {
            destroySnapshot(handle); delete handle; return PEARE_STATUS_ALLOCATION_FAILED;
        }
        for (int i = 0; i < count; ++i) {
            if (i == resourceIndex) continue;
            const peare::OpenedResource opened = state->session.openResource(i);
            if (!opened.isValid()) continue;
            status = fillSnapshotItem(opened, &handle->related[handle->related_count]);
            if (status != PEARE_STATUS_OK) {
                destroySnapshot(handle); delete handle; return status;
            }
            ++handle->related_count;
        }
    }
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
    delete opener;
}

peare_status peare_opener_open_file(peare_opener_handle opener, const char* path_utf8)
{
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    if (!path_utf8)
        return PEARE_STATUS_INVALID_ARGUMENT;
    try {
        auto next = std::make_shared<SessionState>();
        const bool opened = next->session.openFile(QString::fromUtf8(path_utf8));
        opener->state = std::move(next);
        return opened ? PEARE_STATUS_OK : PEARE_STATUS_OPEN_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

peare_status peare_opener_open_buffer(peare_opener_handle opener,
                                      const uint8_t* bytes,
                                      size_t length,
                                      const char* source_name_utf8)
{
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    if ((!bytes && length != 0) || length > static_cast<size_t>(INT_MAX))
        return PEARE_STATUS_INVALID_ARGUMENT;
    try {
        const QByteArray data(reinterpret_cast<const char*>(bytes), static_cast<int>(length));
        const QString source = source_name_utf8
            ? QString::fromUtf8(source_name_utf8)
            : QStringLiteral("memory.bin");
        auto next = std::make_shared<SessionState>();
        const bool opened = next->session.openBuffer(data, source);
        opener->state = std::move(next);
        return opened ? PEARE_STATUS_OK : PEARE_STATUS_OPEN_FAILED;
    } catch (...) {
        return PEARE_STATUS_INTERNAL_ERROR;
    }
}

peare_status peare_opener_close(peare_opener_handle opener)
{
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;
    try {
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

peare_status peare_opener_open_resource(peare_opener_handle opener,
                                             size_t folder_index,
                                             const char* identifier_utf8,
                                             const char* preferred_language_utf8,
                                             peare_resource_handle* out_resource)
{
    if (!out_resource || !identifier_utf8)
        return PEARE_STATUS_INVALID_ARGUMENT;
    *out_resource = nullptr;
    if (!opener || !opener->state)
        return PEARE_STATUS_INVALID_HANDLE;

    const auto& folders = opener->state->session.folders();
    if (folder_index >= static_cast<size_t>(folders.size()))
        return PEARE_STATUS_OUT_OF_RANGE;

    try {
        const QString& folder_type = folders.at(static_cast<int>(folder_index)).type;
        const int index = opener->state->session.findResource(
            folder_type,
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
    // Layer-backed: read the content now (this is the only place a file's bytes
    // are materialised — one file, on demand, when the consumer asks for it).
    if (resource->primary.lazy_content) {
        auto* store = static_cast<peare::fs::ByteStorePtr*>(resource->primary.lazy_content);
        if (store && *store) {
            const std::vector<std::uint8_t> bytes = (*store)->readAll();
            return copyBytes(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size(), out_payload);
        }
    }
    return copyBytes(reinterpret_cast<const char*>(resource->primary.payload.bytes),
                     resource->primary.payload.length, out_payload);
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
    peare_status status = copyBytes(reinterpret_cast<const char*>(source.source_name_utf8.bytes), source.source_name_utf8.length, &out_context->source_name_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.type_utf8.bytes), source.type_utf8.length, &out_context->type_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.identifier_utf8.bytes), source.identifier_utf8.length, &out_context->identifier_utf8);
    if (status == PEARE_STATUS_OK) status = copyBytes(reinterpret_cast<const char*>(source.language_utf8.bytes), source.language_utf8.length, &out_context->language_utf8);
    if (status != PEARE_STATUS_OK) peare_resource_context_free(out_context);
    return status;
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
    }
    return "Unknown status";
}
}
