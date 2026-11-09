#ifndef PEARE_WRITER_H
#define PEARE_WRITER_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(PEARE_WRITER_BUILD)
# define PEARE_WRITER_API __declspec(dllexport)
#elif defined(_WIN32)
# define PEARE_WRITER_API __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PEARE_WRITER_API __attribute__((visibility("default")))
#else
# define PEARE_WRITER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct peare_writer_handle_s *peare_writer_handle;

typedef enum peare_writer_status {
    PEARE_WRITER_OK = 0,
    PEARE_WRITER_INVALID_ARGUMENT,
    PEARE_WRITER_INVALID_HANDLE,
    PEARE_WRITER_NOT_ACTIVE,
    PEARE_WRITER_IO_ERROR,
    PEARE_WRITER_UNSUPPORTED_LIMIT,
    PEARE_WRITER_INVALID_ENTRY_NAME,
    PEARE_WRITER_DUPLICATE_ENTRY,
    PEARE_WRITER_ALLOCATION_FAILED,
    PEARE_WRITER_INTERNAL_ERROR,
    PEARE_WRITER_UNSUPPORTED_OPERATION,
    PEARE_WRITER_ENTRY_NOT_FOUND
} peare_writer_status;

typedef uint32_t peare_writer_update_capabilities;

typedef enum peare_writer_update_capability {
    PEARE_WRITER_CAP_CREATE_FILE = 1u << 0,
    PEARE_WRITER_CAP_CREATE_EMPTY_FILE = 1u << 1,
    PEARE_WRITER_CAP_CREATE_DIRECTORY = 1u << 2,
    PEARE_WRITER_CAP_UPDATE_FILE = 1u << 3,
    PEARE_WRITER_CAP_RENAME_ENTRY = 1u << 4,
    PEARE_WRITER_CAP_DELETE_ENTRY = 1u << 5
} peare_writer_update_capability;

typedef struct peare_writer_format {
    const char *id;
    const char *display_name;
    const char *extension;
} peare_writer_format;

typedef struct peare_writer_variant {
    const char *id;
    const char *display_name;
} peare_writer_variant;

typedef struct peare_writer_input_file {
    const char *source_path_utf8;
    const char *entry_name_utf8;
} peare_writer_input_file;

typedef struct peare_writer_create_options {
    const char *format_id_utf8;
    const char *variant_id_utf8;
    const char *filesystem_id_utf8;
} peare_writer_create_options;

PEARE_WRITER_API peare_writer_status peare_writer_create(peare_writer_handle *out_writer);
PEARE_WRITER_API void peare_writer_destroy(peare_writer_handle writer);

/* Generic container creation and format discovery. Strings are static ABI data. */
PEARE_WRITER_API size_t peare_writer_create_format_count(void);
PEARE_WRITER_API peare_writer_status peare_writer_get_create_format(
    size_t index, peare_writer_format *out_format);
PEARE_WRITER_API size_t peare_writer_create_variant_count(
    const char *format_id_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_get_create_variant(
    const char *format_id_utf8, size_t index, peare_writer_variant *out_variant);
PEARE_WRITER_API peare_writer_status peare_writer_create_begin(
    peare_writer_handle writer, const char *format_id_utf8,
    const char *output_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_create_begin_variant(
    peare_writer_handle writer, const char *format_id_utf8,
    const char *variant_id_utf8, const char *output_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_add_files_to(
    peare_writer_handle writer, const char *target_path_utf8,
    const char *format_id_utf8, const char *variant_id_utf8,
    size_t target_index, const peare_writer_input_file *files,
    size_t file_count);
PEARE_WRITER_API size_t peare_writer_create_filesystem_count(
    const char *format_id_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_get_create_filesystem(
    const char *format_id_utf8, size_t index, peare_writer_variant *out_filesystem);
PEARE_WRITER_API peare_writer_status peare_writer_add_files_to_ex(
    peare_writer_handle writer, const char *target_path_utf8,
    const peare_writer_create_options *create_options, size_t target_index,
    const peare_writer_input_file *files, size_t file_count);
PEARE_WRITER_API peare_writer_status peare_writer_finish(
    peare_writer_handle writer);

/* Generic update operations. The writer selects a backend from the opened file. */
PEARE_WRITER_API int peare_writer_can_add_files(const char *container_path_utf8);
PEARE_WRITER_API size_t peare_writer_add_target_count(const char *container_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_get_add_target_name(
    const char *container_path_utf8, size_t target_index,
    char *name_buffer, size_t name_buffer_size);
PEARE_WRITER_API peare_writer_status peare_writer_get_update_capabilities(
    const char *container_path_utf8, size_t target_index,
    peare_writer_update_capabilities *out_capabilities);
PEARE_WRITER_API size_t peare_writer_update_target_count(
    const char *container_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_get_update_target_name(
    const char *container_path_utf8, size_t target_index,
    char *name_buffer, size_t name_buffer_size);
PEARE_WRITER_API peare_writer_status peare_writer_update_begin(
    peare_writer_handle writer, const char *container_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_update_begin_target(
    peare_writer_handle writer, const char *container_path_utf8,
    size_t target_index);
PEARE_WRITER_API peare_writer_status peare_writer_update_begin_target_with_journal(
    peare_writer_handle writer, const char *container_path_utf8,
    size_t target_index, int64_t journal_offset, int64_t journal_size);
PEARE_WRITER_API peare_writer_status peare_writer_add_files_begin(
    peare_writer_handle writer, const char *container_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_add_files_begin_target(
    peare_writer_handle writer, const char *container_path_utf8, size_t target_index);
PEARE_WRITER_API peare_writer_status peare_writer_add_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_create_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_create_empty_file(
    peare_writer_handle writer, const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_create_directory(
    peare_writer_handle writer, const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_replace_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_replace_entry_by_id(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8, int64_t entry_id);
PEARE_WRITER_API peare_writer_status peare_writer_enable_fsverity(
    peare_writer_handle writer, const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_rename_entry(
    peare_writer_handle writer, const char *entry_name_utf8,
    const char *new_entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_rename_entry_by_id(
    peare_writer_handle writer, const char *entry_name_utf8,
    const char *new_entry_name_utf8, int64_t entry_id);
PEARE_WRITER_API peare_writer_status peare_writer_delete_entry(
    peare_writer_handle writer, const char *entry_name_utf8,
    int recursive);
PEARE_WRITER_API peare_writer_status peare_writer_delete_entry_by_id(
    peare_writer_handle writer, const char *entry_name_utf8,
    int recursive, int64_t entry_id);
PEARE_WRITER_API peare_writer_status peare_writer_update_finish(
    peare_writer_handle writer);

/* Format-specific creation API for backends that support new containers. */
PEARE_WRITER_API peare_writer_status peare_writer_zip_begin(
    peare_writer_handle writer, const char *output_path_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_zip_add_directory(
    peare_writer_handle writer, const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_zip_add_buffer(
    peare_writer_handle writer, const char *entry_name_utf8,
    const uint8_t *bytes, size_t length);
PEARE_WRITER_API peare_writer_status peare_writer_zip_add_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
PEARE_WRITER_API peare_writer_status peare_writer_zip_finish(peare_writer_handle writer);
PEARE_WRITER_API void peare_writer_abort(peare_writer_handle writer);
PEARE_WRITER_API const char *peare_writer_last_error(peare_writer_handle writer);
PEARE_WRITER_API const char *peare_writer_status_message(peare_writer_status status);

#ifdef __cplusplus
}
#endif

#endif
