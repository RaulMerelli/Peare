#ifndef PEARE_FS_H
#define PEARE_FS_H

// C ABI for the DiscUtils-compatible file-system stack. These signatures hide
// the mechanism behind them completely: whether a file's bytes come from a flat
// array or from a stack of lazy layers (window / concat / decompress / cache),
// the caller only ever sees a positioned source it can size and read. New
// readers (ISO now, WIM next, others later) plug in behind the same ABI.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(PEARE_OPENER_BUILD)
#    define PEARE_FS_API __declspec(dllexport)
#  else
#    define PEARE_FS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define PEARE_FS_API __attribute__((visibility("default")))
#else
#  define PEARE_FS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct peare_fs peare_fs;
typedef struct peare_source peare_source;

typedef struct peare_fs_entry {
    const char* name;      /* UTF-8 leaf name, owned by the returned list */
    int32_t is_directory;  /* 1 for a directory, 0 for a file */
    int64_t length;        /* content length in bytes (0 for directories) */
} peare_fs_entry;

/* Detect and open a supported file system over an in-memory image. The bytes
 * are copied and kept alive by the handle. Returns NULL when no supported file
 * system is recognised. */
PEARE_FS_API peare_fs* peare_fs_open_memory(const uint8_t* data, size_t len);

/* Non-zero when the image parsed cleanly. */
PEARE_FS_API int32_t peare_fs_valid(const peare_fs* fs);

/* Friendly name of the detected file system (valid until peare_fs_close). */
PEARE_FS_API const char* peare_fs_name(const peare_fs* fs);

PEARE_FS_API void peare_fs_close(peare_fs* fs);

/* List the immediate children of a directory ("" or "/" is the root). On success
 * returns the child count and sets *out_list to a heap array of that many
 * entries (names owned by the array); release it with peare_fs_free_list.
 * Returns a negative value on error. */
PEARE_FS_API int32_t peare_fs_list(const peare_fs* fs, const char* dir_utf8,
                                   peare_fs_entry** out_list);

PEARE_FS_API void peare_fs_free_list(peare_fs_entry* list, int32_t count);

/* Open a file's content as a positioned source that reads on demand from the
 * image (no eager materialisation). Returns NULL if the path is absent or is a
 * directory. */
PEARE_FS_API peare_source* peare_fs_open_file(const peare_fs* fs, const char* path_utf8);

/* Total number of readable bytes in the source. */
PEARE_FS_API int64_t peare_source_size(const peare_source* source);

/* Read up to count bytes at pos into dst; returns the number of bytes read
 * (0 at or past end). */
PEARE_FS_API int32_t peare_source_read(const peare_source* source, int64_t pos,
                                       uint8_t* dst, int32_t count);

PEARE_FS_API void peare_source_close(peare_source* source);

#ifdef __cplusplus
}
#endif

#endif /* PEARE_FS_H */
