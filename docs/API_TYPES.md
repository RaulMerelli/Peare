# API C: tipi comuni

Header:

```c
#include <peare/peare_types.h>
```

## Handle Opener

```c
typedef struct peare_opener_handle_s *peare_opener_handle;
typedef struct peare_resource_handle_s *peare_resource_handle;
```

Handles are opaque. The consumer must not dereference them.

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
    PEARE_STATUS_UNSUPPORTED_INFO
} peare_status;
```

Differenze importanti:

- `NOT_APPLICABLE`: the operation does not apply to that payload;
- `DECODE_FAILED`: the type applies, but the payload could not be decoded;
- `VALUE_NOT_DETERMINABLE`: the field is known but cannot be reconstructed;
- `UNSUPPORTED_INFO`: the requested identifier is not implemented.

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

A blob contains a complete file in memory. An item in `peare_blob_array` is normally a complete PNG or UTF-8 TXT file.

The length is used exclusively to cross the C ABI.

## Ownership

All memory allocated by Peare must be released by Peare:

```c
peare_blob_free(&blob);
peare_blob_array_free(&array);
peare_value_free(&value);
peare_resource_context_free(&context);
```

Do not use `free()` on returned pointers.

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

Strings are UTF-8 and must not be treated as NUL-terminated; always use `length`.

## Valori di get_info

```c
typedef enum peare_value_type {
    PEARE_VALUE_NONE = 0,
    PEARE_VALUE_BOOL,
    PEARE_VALUE_INT64,
    PEARE_VALUE_UINT64,
    PEARE_VALUE_UTF8,
    PEARE_VALUE_BYTES
} peare_value_type;
```

For `UTF8` and `BYTES`, read `value.buffer.data` and `value.buffer.length`.

## Font options

```c
typedef struct peare_font_render_options {
    uint32_t scale;
    uint32_t padding;
    uint32_t foreground_rgba;
    uint32_t background_rgba;
} peare_font_render_options;
```

Colors use the `0xRRGGBBAA` format.


## XEX container

`PEARE_CONTAINER_XEX` identifies Xbox 360 XEX1 and XEX2 containers. Resources extracted from an embedded PE image retain XEX as their public container format and use `PEARE_PLATFORM_OTHER`; internally they are decoded with PE resource layouts.
