#ifndef PEARE_DECODER_H
#define PEARE_DECODER_H
#include <peare/peare_types.h>
#if defined(_WIN32) && defined(PEARE_DECODER_BUILD)
# define PEARE_DECODER_API __declspec(dllexport)
#elif defined(_WIN32)
# define PEARE_DECODER_API __declspec(dllimport)
#elif defined(__OS2__) && defined(PEARE_DECODER_BUILD)
# define PEARE_DECODER_API __declspec(dllexport)
#elif defined(__OS2__)
# define PEARE_DECODER_API __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PEARE_DECODER_API __attribute__((visibility("default")))
#else
# define PEARE_DECODER_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef struct peare_font_codepoint_array {
    uint32_t *items;
    size_t count;
    int glyph_index_mode;
} peare_font_codepoint_array;

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

typedef struct peare_font_render_options {
    uint32_t scale;
    uint32_t padding;
    uint32_t foreground_rgba;
    uint32_t background_rgba;
} peare_font_render_options;

typedef peare_status (*peare_resource_resolver_fn)(void *user_data,
                                                   const char *resource_type_utf8,
                                                   size_t resource_type_length,
                                                   const char *identifier_utf8,
                                                   size_t identifier_length,
                                                   const uint8_t **out_payload,
                                                   size_t *out_payload_length);

/* platform_utf8 and resource_type_utf8 are optional UTF-8 hints.
 * Examples: "OS/2", "Windows", "RT_BITMAP", "RT_FONT".
 * Pass NULL or an empty string to request payload-only detection. */
PEARE_DECODER_API peare_status peare_decode_images(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_blob_array *out_pngs);

/* Decode images that require sibling resources. The resolver's returned bytes
 * must remain valid until this call returns. */
PEARE_DECODER_API peare_status peare_decode_images_with_resolver(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_resource_resolver_fn resolver,
    void *resolver_user_data,
    peare_blob_array *out_pngs);

PEARE_DECODER_API peare_status peare_decode_texts(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_blob_array *out_texts);

PEARE_DECODER_API peare_status peare_get_info(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_info_id info_id,
    peare_value *out_value);

PEARE_DECODER_API peare_status peare_get_item_info(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    size_t item_index,
    peare_info_id info_id,
    peare_value *out_value);

PEARE_DECODER_API peare_status peare_font_render(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    const char *text_utf8,
    const peare_font_render_options *options,
    peare_blob *out_png);

PEARE_DECODER_API peare_status peare_font_render_with_codepage(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t code_page,
    const char *text_utf8,
    const peare_font_render_options *options,
    peare_blob *out_png);

PEARE_DECODER_API peare_status peare_font_supports_text_with_codepage(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t code_page,
    const char *text_utf8,
    int *out_supported);

PEARE_DECODER_API peare_status peare_font_get_codepoints(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_font_codepoint_array *out_codepoints);

PEARE_DECODER_API peare_status peare_font_get_collection_faces(
    const uint8_t *payload,
    size_t payload_length,
    peare_blob_array *out_faces);

PEARE_DECODER_API peare_status peare_font_get_codepoints_with_codepage(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t code_page,
    peare_font_codepoint_array *out_codepoints);

PEARE_DECODER_API peare_status peare_font_render_glyph_index(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t glyph_index,
    const peare_font_render_options *options,
    peare_blob *out_png);

PEARE_DECODER_API peare_status peare_font_render_glyph_index_with_codepage(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t code_page,
    uint32_t glyph_index,
    const peare_font_render_options *options,
    peare_blob *out_png);

PEARE_DECODER_API peare_status peare_font_render_glyph_indexes_with_codepage(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    uint32_t code_page,
    const uint32_t *glyph_indexes,
    size_t glyph_count,
    const peare_font_render_options *options,
    peare_blob_array *out_pngs);

PEARE_DECODER_API void peare_blob_array_free(peare_blob_array *array);
PEARE_DECODER_API void peare_font_codepoint_array_free(peare_font_codepoint_array *array);
PEARE_DECODER_API void peare_value_free(peare_value *value);

#ifdef __cplusplus
}
#endif
#endif
