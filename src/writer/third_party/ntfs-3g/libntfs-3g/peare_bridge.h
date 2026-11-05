#ifndef PEARE_NTFS_BRIDGE_H
#define PEARE_NTFS_BRIDGE_H

#include <stdint.h>

#if defined(_WIN32) && !defined(PEARE_NTFS_STATIC)
#define PEARE_NTFS_API __declspec(dllexport)
#else
#define PEARE_NTFS_API
#endif

#if defined(_MSC_VER)
#pragma pack(push, 8)
#endif
typedef struct peare_ntfs_io {
    void *context;
    int64_t size;
    int (*read)(void *context, int64_t offset, void *buffer, int64_t size);
    int (*write)(void *context, int64_t offset, const void *buffer, int64_t size);
    int (*flush)(void *context);
} peare_ntfs_io;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

typedef struct peare_ntfs_source {
    void *context;
    int64_t size;
    int64_t (*read)(void *context, void *buffer, int64_t size);
} peare_ntfs_source;

typedef struct peare_ntfs_handle peare_ntfs_handle;

#ifdef __cplusplus
extern "C" {
#endif

PEARE_NTFS_API int peare_ntfs_open(const peare_ntfs_io *io,
                                   peare_ntfs_handle **handle);
PEARE_NTFS_API int peare_ntfs_probe(const peare_ntfs_io *io);
PEARE_NTFS_API int peare_ntfs_format(const peare_ntfs_io *io,
                                    uint64_t partition_start_sector,
                                    uint32_t sector_size);
PEARE_NTFS_API int peare_ntfs_close(peare_ntfs_handle *handle);
PEARE_NTFS_API int peare_ntfs_write_file(peare_ntfs_handle *handle,
                                         const char *path,
                                         const peare_ntfs_source *source,
                                         int replace);
PEARE_NTFS_API int peare_ntfs_create_directory(peare_ntfs_handle *handle,
                                              const char *path);
PEARE_NTFS_API int peare_ntfs_rename(peare_ntfs_handle *handle,
                                     const char *old_path,
                                     const char *new_path);
PEARE_NTFS_API int peare_ntfs_delete(peare_ntfs_handle *handle,
                                    const char *path,
                                    int recursive);

#ifdef __cplusplus
}
#endif

#endif
