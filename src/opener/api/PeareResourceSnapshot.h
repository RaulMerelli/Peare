#pragma once

#include "peare/peare_types.h"
#include <stdint.h>
#include <stddef.h>

#define PEARE_RESOURCE_SNAPSHOT_MAGIC UINT64_C(0x5045415245534E50)
#define PEARE_RESOURCE_SNAPSHOT_VERSION 1u

struct peare_resource_snapshot_item {
    peare_blob payload;
    peare_resource_context context;
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
