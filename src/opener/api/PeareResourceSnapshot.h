#pragma once

#include "peare/peare_types.h"
#include <stdint.h>
#include <stddef.h>

#define PEARE_RESOURCE_SNAPSHOT_MAGIC UINT64_C(0x5045415245534E50)
#define PEARE_RESOURCE_SNAPSHOT_VERSION 1u

struct peare_resource_snapshot_item {
    peare_blob payload;
    peare_resource_context context;
    /* Opaque owner of a deferred layer-backed content store
     * (new peare::fs::ByteStorePtr). When set, the payload is read lazily on
     * peare_resource_get_payload instead of being materialised up front. Freed
     * in freeSnapshotItem. Consumers never see or allocate this. */
    void *lazy_content;
    /* Directory-level container: lazy_content holds the whole filesystem image
     * store and sub_path the directory within it. get_source reopens the image at
     * sub_path; get_payload yields nothing (a directory has no bytes). */
    int is_directory;
    char *sub_path; /* UTF-8, malloc'd; freed in freeSnapshotItem */
};

struct peare_resource_handle_s {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    peare_resource_snapshot_item primary;
    peare_resource_snapshot_item *related;
    size_t related_count;
};

inline bool peare_resource_snapshot_valid(peare_resource_handle resource)
{
    return resource &&
           resource->magic == PEARE_RESOURCE_SNAPSHOT_MAGIC &&
           resource->version == PEARE_RESOURCE_SNAPSHOT_VERSION;
}
