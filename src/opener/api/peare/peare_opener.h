#ifndef PEARE_OPENER_H
#define PEARE_OPENER_H
#include <peare/peare_types.h>
#if defined(_WIN32) && defined(PEARE_OPENER_BUILD)
# define PEARE_OPENER_API __declspec(dllexport)
#elif defined(_WIN32)
# define PEARE_OPENER_API __declspec(dllimport)
#elif defined(__OS2__) && defined(PEARE_OPENER_BUILD)
# define PEARE_OPENER_API __declspec(dllexport)
#elif defined(__OS2__)
# define PEARE_OPENER_API __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PEARE_OPENER_API __attribute__((visibility("default")))
#else
# define PEARE_OPENER_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

PEARE_OPENER_API peare_status peare_opener_create(peare_opener_handle *out_opener);
PEARE_OPENER_API void peare_opener_destroy(peare_opener_handle opener);
/* ---- Unified open ------------------------------------------------------- *
 * There is a single way to open anything: build a source, then open it. A
 * source comes either from a file path or from a resource inside an already
 * open opener; both are opened by the same peare_opener_open. This is what makes
 * nesting recursive and uniform — a resource's content is just another source.
 */
PEARE_OPENER_API peare_status peare_resource_get_source(peare_resource_handle resource,
                                                        peare_source_handle *out_source);
PEARE_OPENER_API peare_status peare_opener_open(peare_opener_handle opener,
                                                peare_source_handle source);
PEARE_OPENER_API void peare_source_destroy(peare_source_handle source);

/* Convenience shim over the unified path: open a file by path. */
PEARE_OPENER_API peare_status peare_opener_open_file(peare_opener_handle opener, const char *path_utf8);
PEARE_OPENER_API peare_status peare_opener_close(peare_opener_handle opener);
PEARE_OPENER_API peare_status peare_opener_get_container_format(
    peare_opener_handle opener, peare_container_format *out_format);
PEARE_OPENER_API peare_status peare_opener_get_description(
    peare_opener_handle opener, peare_blob *out_description_utf8);
PEARE_OPENER_API peare_status peare_opener_get_folder_count(peare_opener_handle opener, size_t *out_count);
PEARE_OPENER_API peare_status peare_opener_get_folder_type(peare_opener_handle opener,
                                                    size_t folder_index,
                                                    peare_blob *out_type_utf8);
PEARE_OPENER_API peare_status peare_opener_get_resource_count(peare_opener_handle opener,
                                                       size_t folder_index,
                                                       size_t *out_count);
PEARE_OPENER_API peare_status peare_opener_open_resource_at(peare_opener_handle opener,
                                                     size_t folder_index,
                                                     size_t resource_index,
                                                     peare_resource_handle *out_resource);
PEARE_OPENER_API peare_status peare_opener_find_resource(peare_opener_handle opener,
                                                  const char *type_utf8,
                                                  const char *identifier_utf8,
                                                  const char *preferred_language_utf8,
                                                  peare_resource_handle *out_resource);
PEARE_OPENER_API void peare_resource_destroy(peare_resource_handle resource);
PEARE_OPENER_API peare_status peare_resource_get_payload(peare_resource_handle resource, peare_blob *out_payload);
PEARE_OPENER_API peare_status peare_resource_get_context(peare_resource_handle resource,
                                                  peare_resource_context *out_context);
PEARE_OPENER_API peare_status peare_resource_get_converted_extensions(
    peare_resource_handle resource, peare_blob_array *out_extensions_utf8);
PEARE_OPENER_API peare_status peare_resource_convert(
    peare_resource_handle resource, const char *extension_utf8, peare_blob_array *out_files);
PEARE_OPENER_API void peare_resource_conversion_array_free(peare_blob_array *array);
PEARE_OPENER_API void peare_resource_context_free(peare_resource_context *context);
PEARE_OPENER_API const char *peare_status_message(peare_status status);

#ifdef __cplusplus
}
#endif
#endif
