#pragma once

#include "peare/peare_types.h"
#include <stdint.h>

#define PEARE_RESOURCE_BRIDGE_MAGIC UINT64_C(0x5045415245524553)
#define PEARE_RESOURCE_BRIDGE_VERSION 1u

struct peare_resource_bridge {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    peare_status (*find_related)(peare_resource_handle source,
                                 const char *type_utf8,
                                 const char *identifier_utf8,
                                 const char *preferred_language_utf8,
                                 peare_resource_handle *out_resource);
};

inline const peare_resource_bridge *peare_resource_bridge_from(peare_resource_handle resource)
{
    if (!resource)
        return nullptr;
    const auto *bridge = reinterpret_cast<const peare_resource_bridge *>(resource);
    if (bridge->magic != PEARE_RESOURCE_BRIDGE_MAGIC ||
        bridge->version != PEARE_RESOURCE_BRIDGE_VERSION ||
        !bridge->find_related)
        return nullptr;
    return bridge;
}
