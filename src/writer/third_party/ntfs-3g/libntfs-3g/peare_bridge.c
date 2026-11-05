#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "peare_bridge.h"

#include "cache.h"
#include "dir.h"
#include "device.h"
#include "layout.h"
#include "logfile.h"
#include "logging.h"
#include "volume.h"
#include "attrib.h"
#include "layout.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define PEARE_NTFS_LAYOUT_ASSERT(name, expression) \
    typedef char peare_ntfs_layout_assert_##name[(expression) ? 1 : -1]

PEARE_NTFS_LAYOUT_ASSERT(mft_flags_size, sizeof(MFT_RECORD_FLAGS) == 2);
PEARE_NTFS_LAYOUT_ASSERT(mft_bytes_allocated_offset,
                         offsetof(MFT_RECORD, bytes_allocated) == 28);
PEARE_NTFS_LAYOUT_ASSERT(mft_record_size, sizeof(MFT_RECORD) == 48);
PEARE_NTFS_LAYOUT_ASSERT(attribute_flags_size, sizeof(ATTR_FLAGS) == 2);
PEARE_NTFS_LAYOUT_ASSERT(resident_flags_size, sizeof(RESIDENT_ATTR_FLAGS) == 1);
PEARE_NTFS_LAYOUT_ASSERT(file_flags_size, sizeof(FILE_ATTR_FLAGS) == 4);
PEARE_NTFS_LAYOUT_ASSERT(file_name_flags_size, sizeof(FILE_NAME_TYPE_FLAGS) == 1);
PEARE_NTFS_LAYOUT_ASSERT(ace_type_size, sizeof(ACE_TYPES) == 1);
PEARE_NTFS_LAYOUT_ASSERT(ace_flags_size, sizeof(ACE_FLAGS) == 1);
PEARE_NTFS_LAYOUT_ASSERT(security_control_size,
                         sizeof(SECURITY_DESCRIPTOR_CONTROL) == 2);
PEARE_NTFS_LAYOUT_ASSERT(volume_flags_size, sizeof(VOLUME_FLAGS) == 2);
PEARE_NTFS_LAYOUT_ASSERT(index_header_flags_size,
                         sizeof(INDEX_HEADER_FLAGS) == 1);
PEARE_NTFS_LAYOUT_ASSERT(index_entry_flags_size,
                         sizeof(INDEX_ENTRY_FLAGS) == 2);
PEARE_NTFS_LAYOUT_ASSERT(ea_flags_size, sizeof(EA_FLAGS) == 1);
PEARE_NTFS_LAYOUT_ASSERT(log_record_flags_size, sizeof(LOG_RECORD_FLAGS) == 2);

struct peare_ntfs_handle {
    peare_ntfs_io io;
    struct ntfs_device *device;
    ntfs_volume *volume;
    int64_t position;
    int poisoned;
};

typedef struct peare_name_node {
    char *name;
    struct peare_name_node *next;
} peare_name_node;

static int peare_device_open(struct ntfs_device *device, int flags)
{
    (void)device;
    (void)flags;
    return 0;
}

static int peare_device_close(struct ntfs_device *device)
{
    (void)device;
    return 0;
}

static s64 peare_device_seek(struct ntfs_device *device, s64 offset, int whence)
{
    peare_ntfs_handle *handle = device
        ? (peare_ntfs_handle *)device->d_private : NULL;
    int64_t base;
    int64_t position;
    if (!handle) {
        errno = EINVAL;
        return -1;
    }
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = handle->position;
    else if (whence == SEEK_END) base = handle->io.size;
    else {
        errno = EINVAL;
        return -1;
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && offset < -base)) {
        errno = EINVAL;
        return -1;
    }
    position = base + offset;
    if (position < 0 || position > handle->io.size) {
        errno = EINVAL;
        return -1;
    }
    handle->position = position;
    return position;
}

static s64 peare_device_read(struct ntfs_device *device, void *buffer, s64 size)
{
    peare_ntfs_handle *handle = device
        ? (peare_ntfs_handle *)device->d_private : NULL;
    int64_t available;
    if (!handle || size < 0 || (!buffer && size)) {
        errno = EINVAL;
        return -1;
    }
    if (!size || handle->position == handle->io.size) return 0;
    available = handle->io.size - handle->position;
    if (size > available) size = available;
    if (!handle->io.read(handle->io.context, handle->position, buffer, size)) {
        errno = EIO;
        return -1;
    }
    handle->position += size;
    return size;
}

static s64 peare_device_write(struct ntfs_device *device, const void *buffer,
                              s64 size)
{
    peare_ntfs_handle *handle = device
        ? (peare_ntfs_handle *)device->d_private : NULL;
    if (!handle || size < 0 || (!buffer && size) ||
        handle->position > handle->io.size ||
        size > handle->io.size - handle->position) {
        errno = EINVAL;
        return -1;
    }
    if (size && !handle->io.write(handle->io.context, handle->position,
                                  buffer, size)) {
        errno = EIO;
        return -1;
    }
    handle->position += size;
    return size;
}

static s64 peare_device_pread(struct ntfs_device *device, void *buffer,
                              s64 size, s64 offset)
{
    peare_ntfs_handle *handle = (peare_ntfs_handle *)device->d_private;
    int read_ok = offset >= 0 && size >= 0 && offset <= handle->io.size &&
        size <= handle->io.size - offset &&
        handle->io.read(handle->io.context, offset, buffer, size);
    if (!read_ok) {
        errno = EIO;
        return -1;
    }
    return size;
}

static s64 peare_device_pwrite(struct ntfs_device *device, const void *buffer,
                               s64 size, s64 offset)
{
    peare_ntfs_handle *handle = (peare_ntfs_handle *)device->d_private;
    if (offset < 0 || size < 0 || offset > handle->io.size ||
        size > handle->io.size - offset ||
        !handle->io.write(handle->io.context, offset, buffer, size)) {
        errno = EIO;
        return -1;
    }
    return size;
}

static int peare_device_sync(struct ntfs_device *device)
{
    peare_ntfs_handle *handle = (peare_ntfs_handle *)device->d_private;
    return handle->io.flush(handle->io.context) ? 0 : -1;
}

static int peare_device_stat(struct ntfs_device *device, struct stat *status)
{
    peare_ntfs_handle *handle = (peare_ntfs_handle *)device->d_private;
    memset(status, 0, sizeof(*status));
    status->st_size = (off_t)handle->io.size;
    return 0;
}

static int peare_device_ioctl(struct ntfs_device *device,
                              unsigned long request, void *argument)
{
    peare_ntfs_handle *handle = device
        ? (peare_ntfs_handle *)device->d_private : NULL;
    if (!handle || !argument) {
        errno = EINVAL;
        return -1;
    }
#ifdef BLKGETSIZE64
    if (request == BLKGETSIZE64) {
        *(u64 *)argument = (u64)handle->io.size;
        return 0;
    }
#endif
#ifdef BLKGETSIZE
    if (request == BLKGETSIZE) {
        const u64 sectors = (u64)handle->io.size / 512;
        if (sectors > ULONG_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        *(unsigned long *)argument = (unsigned long)sectors;
        return 0;
    }
#endif
#ifdef BLKSSZGET
    if (request == BLKSSZGET) {
        unsigned char boot[512];
        u16 bytes_per_sector;
        if (!handle->io.read(handle->io.context, 0, boot, sizeof(boot))) {
            errno = EIO;
            return -1;
        }
        memcpy(&bytes_per_sector, boot + 11, sizeof(bytes_per_sector));
        bytes_per_sector = le16_to_cpu(bytes_per_sector);
        if (bytes_per_sector < 256 || bytes_per_sector > 4096 ||
            (bytes_per_sector & (bytes_per_sector - 1))) {
            errno = EINVAL;
            return -1;
        }
        *(int *)argument = bytes_per_sector;
        return 0;
    }
#endif
#ifdef BLKBSZSET
    if (request == BLKBSZSET) return 0;
#endif
    errno = EOPNOTSUPP;
    return -1;
}

static struct ntfs_device_operations peare_device_operations = {
    peare_device_open,
    peare_device_close,
    peare_device_seek,
    peare_device_read,
    peare_device_write,
    peare_device_pread,
    peare_device_pwrite,
    peare_device_sync,
    peare_device_stat,
    peare_device_ioctl
};

static int peare_utf8_next(const unsigned char **cursor, u32 *codepoint)
{
    const unsigned char *p = *cursor;
    u32 value;
    unsigned int count;
    if (*p < 0x80) {
        value = *p;
        count = 1;
    } else if (*p >= 0xc2 && *p <= 0xdf) {
        value = *p & 0x1f;
        count = 2;
    } else if (*p >= 0xe0 && *p <= 0xef) {
        value = *p & 0x0f;
        count = 3;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
        value = *p & 0x07;
        count = 4;
    } else {
        return -1;
    }
    for (unsigned int i = 1; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80) return -1;
        if (i == 1 && ((p[0] == 0xe0 && p[i] < 0xa0) ||
                       (p[0] == 0xed && p[i] >= 0xa0) ||
                       (p[0] == 0xf0 && p[i] < 0x90) ||
                       (p[0] == 0xf4 && p[i] >= 0x90))) return -1;
        value = (value << 6) | (p[i] & 0x3f);
    }
    *cursor = p + count;
    *codepoint = value;
    return 0;
}

static int peare_utf8_name(const char *text, ntfschar **name, u8 *length)
{
    const unsigned char *cursor;
    u32 codepoint;
    unsigned int units = 0;
    ntfschar *result;
    unsigned int index = 0;
    if (!text || !*text || !name || !length) return -1;
    cursor = (const unsigned char *)text;
    while (*cursor) {
        if (peare_utf8_next(&cursor, &codepoint)) return -1;
        units += codepoint > 0xffff ? 2 : 1;
        if (units > 255) return -1;
    }
    result = (ntfschar *)malloc(units * sizeof(ntfschar));
    if (!result) return -1;
    cursor = (const unsigned char *)text;
    while (*cursor) {
        if (peare_utf8_next(&cursor, &codepoint)) {
            free(result);
            return -1;
        }
        if (codepoint <= 0xffff) {
            result[index++] = cpu_to_le16((u16)codepoint);
        } else {
            codepoint -= 0x10000;
            result[index++] = cpu_to_le16((u16)(0xd800 | (codepoint >> 10)));
            result[index++] = cpu_to_le16((u16)(0xdc00 | (codepoint & 0x3ff)));
        }
    }
    *length = (u8)units;
    *name = result;
    return 0;
}

static int peare_name_utf8(const ntfschar *name, int length, char *output,
                           size_t capacity)
{
    size_t used = 0;
    for (int i = 0; i < length; ++i) {
        u32 codepoint = le16_to_cpu(name[i]);
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            u32 low;
            if (++i >= length) return -1;
            low = le16_to_cpu(name[i]);
            if (low < 0xdc00 || low > 0xdfff) return -1;
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            return -1;
        }
        if (codepoint < 0x80) {
            if (used + 1 >= capacity) return -1;
            output[used++] = (char)codepoint;
        } else if (codepoint < 0x800) {
            if (used + 2 >= capacity) return -1;
            output[used++] = (char)(0xc0 | (codepoint >> 6));
            output[used++] = (char)(0x80 | (codepoint & 0x3f));
        } else if (codepoint < 0x10000) {
            if (used + 3 >= capacity) return -1;
            output[used++] = (char)(0xe0 | (codepoint >> 12));
            output[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            output[used++] = (char)(0x80 | (codepoint & 0x3f));
        } else {
            if (used + 4 >= capacity) return -1;
            output[used++] = (char)(0xf0 | (codepoint >> 18));
            output[used++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
            output[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            output[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    }
    output[used] = '\0';
    return (int)used;
}

static char *peare_string_duplicate(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static int peare_next_component(char **cursor, char **component)
{
    char *start = *cursor;
    char *end;
    while (*start == '/') ++start;
    if (!*start) return 0;
    end = start;
    while (*end && *end != '/') ++end;
    if (*end) {
        *end = '\0';
        *cursor = end + 1;
        while (**cursor == '/') ++*cursor;
    } else {
        *cursor = end;
    }
    if (!strcmp(start, ".") || !strcmp(start, "..") || strchr(start, ':'))
        return -1;
    *component = start;
    return 1;
}

static int peare_parent_and_name(peare_ntfs_handle *handle, const char *path,
                                 ntfs_inode **parent, ntfs_inode **entry,
                                 ntfschar **leaf, u8 *leaf_length)
{
    char *copy, *cursor, *component, *next;
    ntfs_inode *directory, *child;
    ntfschar *unicode;
    u8 unicode_length;
    int state;
    if (!path || !*path || path[0] == '/' || strchr(path, '\\')) return -1;
    copy = peare_string_duplicate(path);
    if (!copy) return -1;
    cursor = copy;
    directory = ntfs_inode_open(handle->volume, FILE_root);
    if (!directory) {
        free(copy);
        return -1;
    }
    for (;;) {
        state = peare_next_component(&cursor, &component);
        if (state <= 0) {
            ntfs_inode_close(directory);
            free(copy);
            return -1;
        }
        next = cursor;
        while (*next == '/') ++next;
        if (!*next) break;
        if (peare_utf8_name(component, &unicode, &unicode_length)) {
            ntfs_inode_close(directory);
            free(copy);
            return -1;
        }
        {
            u64 reference = ntfs_inode_lookup_by_name(directory, unicode,
                                                       unicode_length);
            child = reference == (u64)-1 ? NULL :
                    ntfs_inode_open(handle->volume, reference);
        }
        free(unicode);
        if (!child || !(child->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
            if (child) ntfs_inode_close(child);
            ntfs_inode_close(directory);
            free(copy);
            return -1;
        }
        ntfs_inode_close(directory);
        directory = child;
    }
    if (peare_utf8_name(component, leaf, leaf_length)) {
        ntfs_inode_close(directory);
        free(copy);
        return -1;
    }
    free(copy);
    *parent = directory;
    *entry = NULL;
    {
        u64 reference = ntfs_inode_lookup_by_name(directory, *leaf,
                                                   *leaf_length);
        if (reference != (u64)-1)
            *entry = ntfs_inode_open(handle->volume, reference);
    }
    return 0;
}

static int peare_ntfs_open_mode(const peare_ntfs_io *io,
                                peare_ntfs_handle **result,
                                ntfs_mount_flags flags)
{
    peare_ntfs_handle *handle;
    if (!io || !result || io->size <= 0 || !io->read || !io->write ||
        !io->flush) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    handle = (peare_ntfs_handle *)calloc(1, sizeof(*handle));
    if (!handle) {
        errno = ENOMEM;
        return -1;
    }
    handle->io = *io;
    handle->device = ntfs_device_alloc("peare-staged-image", 0,
                                      &peare_device_operations, handle);
    if (!handle->device) {
        free(handle);
        errno = ENOMEM;
        return -1;
    }
    handle->volume = ntfs_device_mount(handle->device, flags);
    if (!handle->volume) {
        int saved_errno = errno ? errno : EIO;
        ntfs_device_free(handle->device);
        free(handle);
        errno = saved_errno;
        return -1;
    }
    ntfs_create_lru_caches(handle->volume);
    *result = handle;
    return 0;
}

PEARE_NTFS_API int peare_ntfs_open(const peare_ntfs_io *io,
                                   peare_ntfs_handle **result)
{
    return peare_ntfs_open_mode(io, result, NTFS_MNT_NONE);
}

PEARE_NTFS_API int peare_ntfs_probe(const peare_ntfs_io *io)
{
    peare_ntfs_handle *handle = NULL;
    int result;
    if (peare_ntfs_open_mode(io, &handle, NTFS_MNT_RDONLY))
        result = errno ? errno : EIO;
    else if (peare_ntfs_close(handle))
        result = errno ? errno : EIO;
    else
        result = 0;
    return result;
}

PEARE_NTFS_API int peare_ntfs_close(peare_ntfs_handle *handle)
{
    int result;
    if (!handle) return -1;
    result = ntfs_umount(handle->volume, FALSE);
    if (!result) free(handle);
    return result;
}

PEARE_NTFS_API int peare_ntfs_write_file(peare_ntfs_handle *handle,
                                         const char *path,
                                         const peare_ntfs_source *source,
                                         int replace)
{
    ntfs_inode *parent = NULL, *entry = NULL;
    ntfs_attr *data = NULL;
    ntfschar *leaf = NULL;
    u8 leaf_length = 0;
    char buffer[65536];
    int64_t remaining;
    int result = -1;
    if (!handle || !source || source->size < 0 || !source->read)
        return -1;
    if (handle->poisoned) return -1;
    if (peare_parent_and_name(handle, path, &parent, &entry, &leaf,
                              &leaf_length)) return -1;
    if (entry) {
        if (!replace || (entry->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
            errno = EEXIST;
            goto done;
        }
    } else {
        if (replace) {
            errno = ENOENT;
            goto done;
        }
        entry = ntfs_create(parent, 0, leaf, leaf_length, S_IFREG);
        if (!entry) goto done;
    }
    data = ntfs_attr_open(entry, AT_DATA, AT_UNNAMED, 0);
    if (!data) goto done;
    if (ntfs_attr_truncate(data, source->size)) goto done;
    remaining = source->size;
    while (remaining > 0) {
        s64 count = remaining < (s64)sizeof(buffer) ? remaining :
                    (s64)sizeof(buffer);
        s64 read_count = source->read(source->context, buffer, count);
        if (read_count != count) goto done;
        if (ntfs_attr_pwrite(data, source->size - remaining, count, buffer) != count)
            goto done;
        remaining -= count;
    }
    result = 0;
done:
    if (data) ntfs_attr_close(data);
    if (entry && ntfs_inode_close_in_dir(entry, parent)) {
        result = -1;
    }
    if (parent && ntfs_inode_close(parent)) {
        result = -1;
    }
    free(leaf);
    if (result) handle->poisoned = 1;
    return result;
}

PEARE_NTFS_API int peare_ntfs_create_directory(peare_ntfs_handle *handle,
                                              const char *path)
{
    ntfs_inode *parent = NULL, *entry = NULL;
    ntfschar *leaf = NULL;
    u8 leaf_length = 0;
    int result = -1;
    if (!handle || handle->poisoned || peare_parent_and_name(
            handle, path, &parent, &entry, &leaf, &leaf_length)) return -1;
    if (entry) {
        if (entry->mrec->flags & MFT_RECORD_IS_DIRECTORY) result = 0;
    } else {
        entry = ntfs_create(parent, 0, leaf, leaf_length, S_IFDIR);
        if (entry) result = 0;
    }
    if (entry && ntfs_inode_close_in_dir(entry, parent)) result = -1;
    if (parent && ntfs_inode_close(parent)) result = -1;
    free(leaf);
    if (result) handle->poisoned = 1;
    return result;
}

PEARE_NTFS_API int peare_ntfs_rename(peare_ntfs_handle *handle,
                                     const char *old_path,
                                     const char *new_path)
{
    ntfs_inode *old_parent = NULL, *entry = NULL;
    ntfs_inode *new_parent = NULL, *existing = NULL;
    ntfschar *old_leaf = NULL, *new_leaf = NULL;
    u8 old_length = 0, new_length = 0;
    u64 entry_mft = 0;
    int linked = 0;
    int result = -1;
    if (!handle || handle->poisoned || !old_path || !new_path) goto done;
    if (!strcmp(old_path, new_path)) return 0;
    if (peare_parent_and_name(handle, old_path, &old_parent, &entry,
                              &old_leaf, &old_length) || !entry) goto done;
    if (peare_parent_and_name(handle, new_path, &new_parent, &existing,
                              &new_leaf, &new_length)) goto done;
    if (existing) goto done;
    entry_mft = entry->mft_no;
    if (ntfs_link(entry, new_parent, new_leaf, new_length)) goto done;
    linked = 1;
    result = 0;
    if (ntfs_inode_close(new_parent)) result = -1;
    new_parent = NULL;
    if (old_parent && ntfs_inode_close(old_parent)) result = -1;
    old_parent = NULL;
    if (ntfs_inode_close(entry)) result = -1;
    entry = NULL;
    if (result) goto done;

    free(old_leaf);
    old_leaf = NULL;
    if (peare_parent_and_name(handle, old_path, &old_parent, &entry,
                              &old_leaf, &old_length) || !entry ||
        entry->mft_no != entry_mft) goto done;
    result = ntfs_delete(handle->volume, old_path, entry, old_parent,
                         old_leaf, old_length);
    entry = NULL;
    old_parent = NULL;
done:
    if (existing) ntfs_inode_close(existing);
    if (linked) {
        if (new_parent && ntfs_inode_close(new_parent)) result = -1;
        if (old_parent && ntfs_inode_close(old_parent)) result = -1;
        if (entry && ntfs_inode_close(entry)) result = -1;
    } else {
        if (entry && ntfs_inode_close_in_dir(entry, old_parent)) result = -1;
        if (old_parent && ntfs_inode_close(old_parent)) result = -1;
        if (new_parent && ntfs_inode_close(new_parent)) result = -1;
    }
    free(old_leaf);
    free(new_leaf);
    if (result) handle->poisoned = 1;
    return result;
}

static int peare_collect_name(void *opaque, const ntfschar *name,
                              const int length, const int name_type,
                              const s64 position, const MFT_REF reference,
                              const unsigned type)
{
    peare_name_node **head = (peare_name_node **)opaque;
    peare_name_node *node;
    int bytes;
    (void)position;
    (void)reference;
    if (name_type == FILE_NAME_DOS || (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    bytes = length * 4;
    node = (peare_name_node *)calloc(1, sizeof(*node));
    if (!node) return -1;
    node->name = (char *)malloc((size_t)bytes + 1);
    if (!node->name || peare_name_utf8(name, length, node->name,
                                       (size_t)bytes + 1) < 0) {
        free(node->name);
        free(node);
        return -1;
    }
    node->name[bytes] = '\0';
    node->next = *head;
    *head = node;
    (void)type;
    return 0;
}

static void peare_free_names(peare_name_node *node)
{
    peare_name_node *next;
    while (node) {
        next = node->next;
        free(node->name);
        free(node);
        node = next;
    }
}

static int peare_delete_path(peare_ntfs_handle *handle, const char *path,
                             int recursive)
{
    ntfs_inode *parent = NULL, *entry = NULL;
    ntfschar *leaf = NULL;
    u8 leaf_length = 0;
    MFT_REF entry_reference = 0;
    int result = -1;
    if (peare_parent_and_name(handle, path, &parent, &entry, &leaf,
                              &leaf_length) || !entry) goto done;
    if (entry->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
        if (!recursive) {
            if (ntfs_check_empty_dir(entry)) goto done;
        } else {
            peare_name_node *names = NULL, *node, *next;
            s64 position = 0;
            int close_error = 0;
            entry_reference = MK_MREF(entry->mft_no,
                    le16_to_cpu(entry->mrec->sequence_number));
            if (ntfs_readdir(entry, &position, &names,
                             peare_collect_name) != 0) {
                peare_free_names(names);
                goto done;
            }
            if (parent && ntfs_inode_close(parent)) close_error = 1;
            parent = NULL;
            if (entry && ntfs_inode_close(entry)) close_error = 1;
            entry = NULL;
            if (close_error) {
                peare_free_names(names);
                handle->poisoned = 1;
                goto done;
            }
            for (node = names; node; node = node->next) {
                size_t size = strlen(path) + strlen(node->name) + 2;
                char *child_path = (char *)malloc(size);
                if (!child_path) {
                    handle->poisoned = 1;
                    break;
                }
                snprintf(child_path, size, "%s/%s", path, node->name);
                if (peare_delete_path(handle, child_path, 1)) {
                    handle->poisoned = 1;
                }
                free(child_path);
            }
            peare_free_names(names);
            if (handle->poisoned) goto done;
            if (peare_parent_and_name(handle, path, &parent, &entry,
                                      &leaf, &leaf_length) || !entry ||
                MK_MREF(entry->mft_no,
                    le16_to_cpu(entry->mrec->sequence_number)) !=
                    entry_reference) goto done;
        }
    }
    result = ntfs_delete(handle->volume, path, entry, parent, leaf,
                         leaf_length);
    entry = NULL;
    parent = NULL;
done:
    if (parent && ntfs_inode_close(parent)) handle->poisoned = 1;
    if (entry && ntfs_inode_close(entry)) handle->poisoned = 1;
    free(leaf);
    if (result) handle->poisoned = 1;
    return result;
}

PEARE_NTFS_API int peare_ntfs_delete(peare_ntfs_handle *handle,
                                    const char *path,
                                    int recursive)
{
    if (!handle || handle->poisoned || !path || !*path) return -1;
    return peare_delete_path(handle, path, recursive != 0);
}
