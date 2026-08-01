#ifndef PEARE_TYPES_H
#define PEARE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && (defined(PEARE_OPENER_BUILD) || defined(PEARE_DECODER_BUILD))
# define PEARE_COMMON_API __declspec(dllexport)
#elif defined(_WIN32)
# define PEARE_COMMON_API __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PEARE_COMMON_API __attribute__((visibility("default")))
#else
# define PEARE_COMMON_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct peare_opener_handle_s *peare_opener_handle;
typedef struct peare_resource_handle_s *peare_resource_handle;
/* A byte source to be opened. Created from a file path or from a resource's
 * content; both feed the single peare_opener_open entry point. Opaque; the
 * caller cannot tell whether it is a file, an in-memory array, or a lazy
 * layer-backed store. */
typedef struct peare_source_handle_s *peare_source_handle;

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

typedef enum peare_container_format {
    PEARE_CONTAINER_UNKNOWN = 0,
    PEARE_CONTAINER_DOS_MZ,
    PEARE_CONTAINER_PE,
    PEARE_CONTAINER_NE,
    PEARE_CONTAINER_LE,
    PEARE_CONTAINER_LX,
    PEARE_CONTAINER_XEX,
    PEARE_CONTAINER_XBE,
    PEARE_CONTAINER_XUIZ,
    PEARE_CONTAINER_LIVE_PIRS,
    PEARE_CONTAINER_CON,
    PEARE_CONTAINER_OS2_PACK,
    PEARE_CONTAINER_SZDD,
    PEARE_CONTAINER_SIEMENS_IMG,
    PEARE_CONTAINER_SIEMENS_FWF,
    PEARE_CONTAINER_ISO9660
} peare_container_format;

typedef enum peare_platform {
    PEARE_PLATFORM_UNKNOWN = 0,
    PEARE_PLATFORM_WINDOWS,
    PEARE_PLATFORM_OS2,
    PEARE_PLATFORM_OTHER
} peare_platform;

typedef struct peare_blob {
    uint8_t *bytes;
    size_t length;
} peare_blob;

typedef struct peare_blob_array {
    peare_blob *items;
    size_t count;
} peare_blob_array;

PEARE_COMMON_API void peare_blob_free(peare_blob *blob);


typedef enum peare_info_id {
    PEARE_INFO_NONE = 0,
    PEARE_INFO_FONT_FACE_NAME,
    PEARE_INFO_FONT_DEVICE_NAME,
    PEARE_INFO_FONT_COPYRIGHT,
    PEARE_INFO_FONT_CODEPAGE,
    PEARE_INFO_FONT_FIRST_CHARACTER,
    PEARE_INFO_FONT_LAST_CHARACTER,
    PEARE_INFO_FONT_DEFAULT_CHARACTER,
    PEARE_INFO_FONT_BREAK_CHARACTER,
    PEARE_INFO_FONT_FIXED_WIDTH,
    PEARE_INFO_FONT_POINT_SIZE,
    PEARE_INFO_FONT_PIXEL_HEIGHT,
    PEARE_INFO_FONT_ASCENT,
    PEARE_INFO_FONT_DESCENT,
    PEARE_INFO_FONT_AVERAGE_WIDTH,
    PEARE_INFO_FONT_MAXIMUM_WIDTH,
    PEARE_INFO_FONT_GLYPH_COUNT,
    PEARE_INFO_POINTER_HOTSPOT_X,
    PEARE_INFO_POINTER_HOTSPOT_Y,
    PEARE_INFO_BITMAP_WIDTH,
    PEARE_INFO_BITMAP_HEIGHT,
    PEARE_INFO_BITMAP_BPP,
    PEARE_INFO_BITMAP_COMPRESSION,
    PEARE_INFO_TEXT_CODEPAGE,
    PEARE_INFO_RESOURCE_PLATFORM,
    PEARE_INFO_RESOURCE_CONTAINER_FORMAT,
    PEARE_INFO_RESOURCE_TYPE,
    PEARE_INFO_RESOURCE_LANGUAGE
} peare_info_id;

typedef enum peare_value_type {
    PEARE_VALUE_NONE = 0,
    PEARE_VALUE_BOOL,
    PEARE_VALUE_INT64,
    PEARE_VALUE_UINT64,
    PEARE_VALUE_UTF8,
    PEARE_VALUE_BYTES
} peare_value_type;

typedef struct peare_value {
    peare_value_type type;
    union {
        int boolean_value;
        int64_t signed_value;
        uint64_t unsigned_value;
        struct {
            const uint8_t *data;
            size_t length;
        } buffer;
    } value;
} peare_value;

/* Colors use 0xRRGGBBAA byte order. */
typedef struct peare_font_render_options {
    uint32_t scale;
    uint32_t padding;
    uint32_t foreground_rgba;
    uint32_t background_rgba;
} peare_font_render_options;

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
    int32_t is_container;  /* nonzero: openable as a nested container */
} peare_resource_context;


#ifdef __cplusplus
}
#endif

#endif
