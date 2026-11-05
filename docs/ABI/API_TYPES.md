# Common C ABI types

Header:

```c
#include <peare/peare_types.h>
```

## Handles

```c
typedef struct peare_opener_handle_s *peare_opener_handle;
typedef struct peare_resource_handle_s *peare_resource_handle;
typedef struct peare_source_handle_s *peare_source_handle;
```

Handles are opaque. Do not access their fields.
Create a source from a file or resource, then pass it to `peare_opener_open()`.

## Status

```c
typedef enum peare_status {
    PEARE_STATUS_OK = 0,
    PEARE_STATUS_INVALID_ARGUMENT,
    PEARE_STATUS_INVALID_HANDLE,
    PEARE_STATUS_NOT_OPEN,
    PEARE_STATUS_NOT_FOUND,
    PEARE_STATUS_OUT_OF_RANGE,
    PEARE_STATUS_OPEN_FAILED,
    PEARE_STATUS_ALLOCATION_FAILED,
    PEARE_STATUS_INTERNAL_ERROR,
    PEARE_STATUS_NOT_APPLICABLE,
    PEARE_STATUS_DECODE_FAILED,
    PEARE_STATUS_VALUE_NOT_DETERMINABLE,
    PEARE_STATUS_UNSUPPORTED_INFO,
    PEARE_STATUS_INTEGRITY_FAILED,
    PEARE_STATUS_PASSWORD_REQUIRED,
    PEARE_STATUS_PASSWORD_INVALID
} peare_status;
```

Important status values:

- `NOT_APPLICABLE`: the operation does not apply to that payload.
- `DECODE_FAILED`: the type applies, but the payload could not be decoded.
- `VALUE_NOT_DETERMINABLE`: the field is known but cannot be reconstructed.
- `UNSUPPORTED_INFO`: the requested identifier is not implemented.
- `INTEGRITY_FAILED`: a resource checksum or integrity check failed.
- `PASSWORD_REQUIRED`: the opened archive needs a password.
- `PASSWORD_INVALID`: the password is incorrect, or the archive is damaged.

## Blob

```c
typedef struct peare_blob {
    uint8_t *bytes;
    size_t length;
} peare_blob;

typedef struct peare_blob_array {
    peare_blob *items;
    size_t count;
} peare_blob_array;
```

A blob contains one complete file in memory. An item in `peare_blob_array` is
normally a complete PNG or UTF-8 text file.

The length field gives the byte count across the C ABI.

## Ownership

Release each result with the matching function from its owning library:

```c
peare_blob_free(&blob);
peare_blob_array_free(&array);
peare_value_free(&value);
peare_resource_context_free(&context);
```

Use `peare_blob_array_free()` and `peare_value_free()` for Decoder results.
Use `peare_resource_conversion_array_free()` for Opener conversion results.
Do not call `free()` on a pointer from Peare.

## Opener context

```c
typedef struct peare_resource_context {
    peare_container_format container_format;
    peare_platform platform;
    peare_blob source_name_utf8;
    peare_blob type_utf8;
    peare_blob identifier_utf8;
    peare_blob language_utf8;
    uint32_t codepage;
    uint64_t data_offset;
    uint64_t data_size;
    int64_t base_id;
    int64_t resource_index;
} peare_resource_context;
```

Strings use UTF-8. They are not NUL-terminated. Use `length` to read each string.

## XEX container

`PEARE_CONTAINER_XEX` identifies Xbox 360 XEX1 and XEX2 containers. Resources
from an embedded PE image keep XEX as their public container format. They use
`PEARE_PLATFORM_OTHER` and PE resource layouts for decoding.


## Appended container values

`PEARE_CONTAINER_QCOW` and `PEARE_CONTAINER_QCOW2` identify QCOW v1 and QCOW2
v2/v3. `PEARE_CONTAINER_FLOPPY_IMAGE` identifies standard-capacity raw PC
floppy images. The file system inside a floppy does not change this value.

`PEARE_CONTAINER_PS3_PUP` identifies PlayStation 3 PUP v1 packages. The APPX,
MSIX, APPXBUNDLE, and XAP values identify ZIP-based application packages.
`PEARE_CONTAINER_MSG` identifies Outlook structured-storage messages.
`PEARE_CONTAINER_ASAR` identifies Electron ASAR archives.
`PEARE_CONTAINER_GZIP` identifies RFC 1952 streams. Peare checks each member's
CRC32 and uncompressed size. It joins valid concatenated members.

The font-specific values after `PEARE_CONTAINER_GZIP` are reserved legacy IDs.
The Opener does not emit them. `PEARE_CONTAINER_RESOURCE_FORK` identifies a
generic Apple resource fork.

These enum values were appended. This preserves earlier numeric ABI values.
