#ifndef PEARE_NTFS_PWD_H
#define PEARE_NTFS_PWD_H

#include <stddef.h>
#include <sys/types.h>

struct passwd {
	char *pw_name;
	uid_t pw_uid;
	gid_t pw_gid;
};

static __inline struct passwd *getpwuid(uid_t uid)
{
	(void)uid;
	return NULL;
}

static __inline struct passwd *getpwnam(const char *name)
{
	(void)name;
	return NULL;
}

#endif
