#ifndef PEARE_DECODER_H
#define PEARE_DECODER_H
#include <peare/peare_types.h>
#if defined(_WIN32) && defined(PEARE_DECODER_BUILD)
# define PEARE_DECODER_API __declspec(dllexport)
#elif defined(_WIN32)
# define PEARE_DECODER_API __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PEARE_DECODER_API __attribute__((visibility("default")))
#else
# define PEARE_DECODER_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* platform_utf8 and resource_type_utf8 are optional UTF-8 hints.
 * Examples: "OS/2", "Windows", "RT_BITMAP", "RT_FONT".
 * Pass NULL or an empty string to request payload-only detection. */
PEARE_DECODER_API peare_status peare_decode_images(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
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

PEARE_DECODER_API void peare_blob_array_free(peare_blob_array *array);
PEARE_DECODER_API void peare_value_free(peare_value *value);

#ifdef __cplusplus
}
#endif
#endif
