#ifndef PEARE_NTFS_MSVC_COMPAT_H
#define PEARE_NTFS_MSVC_COMPAT_H

#include "config.h"
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#define _CRT_RAND_S 1
#include <stdlib.h>
#include <time.h>
#include "compat.h"

#define stat _stat

#define __timespec_defined 1

#define HAVE_CONFIG_H 1
#define uid_t peare_uid_t
#define gid_t peare_gid_t
#define mode_t peare_mode_t
#define pid_t peare_pid_t
#define off_t peare_off_t

typedef uint32_t peare_uid_t;
typedef uint32_t peare_gid_t;
typedef uint32_t peare_mode_t;
typedef int32_t peare_pid_t;
typedef int64_t peare_off_t;
#ifndef _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

#define read _read
#define close _close
#define open _open
#define getuid peare_getuid
#define getgid peare_getgid

#define major(device) 0
#define minor(device) 0
#define makedev(device_major, device_minor) 0
#define random peare_random
#define srandom peare_srandom

#ifdef S_IFMT
#undef S_IFMT
#endif
#ifdef S_IFIFO
#undef S_IFIFO
#endif
#ifdef S_IFCHR
#undef S_IFCHR
#endif
#ifdef S_IFDIR
#undef S_IFDIR
#endif
#ifdef S_IFBLK
#undef S_IFBLK
#endif
#ifdef S_IFREG
#undef S_IFREG
#endif
#ifdef S_IFLNK
#undef S_IFLNK
#endif
#ifdef S_IFSOCK
#undef S_IFSOCK
#endif
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#ifdef S_ISUID
#undef S_ISUID
#endif
#ifdef S_ISGID
#undef S_ISGID
#endif
#ifdef S_ISVTX
#undef S_ISVTX
#endif
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)

#ifndef S_IRUSR
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001
#endif

static __inline peare_uid_t peare_getuid(void) { return 0; }
static __inline peare_gid_t peare_getgid(void) { return 0; }
static __inline long peare_random(void) {
	unsigned int value = 0;
	return rand_s(&value) == 0 ? (long)(value & 0x7fffffffU) : 0;
}
static __inline void peare_srandom(unsigned int seed) { (void)seed; }

#endif
