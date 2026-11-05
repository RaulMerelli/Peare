#ifndef PEARE_NTFS_GRP_H
#define PEARE_NTFS_GRP_H

#include <stddef.h>
#include <sys/types.h>

struct group {
	char *gr_name;
	gid_t gr_gid;
	char **gr_mem;
};

static __inline struct group *getgrgid(gid_t gid)
{
	(void)gid;
	return NULL;
}

static __inline struct group *getgrnam(const char *name)
{
	(void)name;
	return NULL;
}

#endif
