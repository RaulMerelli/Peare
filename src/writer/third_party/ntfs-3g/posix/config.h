#ifndef PEARE_NTFS_POSIX_CONFIG_H
#define PEARE_NTFS_POSIX_CONFIG_H

#define HAVE_CONFIG_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDARG_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_TIME_H 1
#define HAVE_CTYPE_H 1
#define HAVE_WCHAR_H 1
#define HAVE_UNISTD_H 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define POSIXACLS 0

#ifndef S_IREAD
#define S_IREAD S_IRUSR
#endif
#ifndef S_IWRITE
#define S_IWRITE S_IWUSR
#endif
#ifndef S_IEXEC
#define S_IEXEC S_IXUSR
#endif

#if defined(__linux__) || defined(__ANDROID__)
#define HAVE_SYS_SYSMACROS_H 1
#include <sys/sysmacros.h>
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define WORDS_LITTLEENDIAN 1
#endif

#endif
