#ifndef __KUDOS_KFS_UHFS_H
#define __KUDOS_KFS_UHFS_H

#include <kfs/lfs.h>
#include <kfs/cfs.h>

/* The number of open files per UHFS module. */
#define UHFS_MAX_OPEN 256

/* This is the range used by UHFS for mapping client Fd pages. (shared across all UHFS) */
#define UHFS_FD_MAP ((void *) 0xB0000000)
#define UHFS_FD_END ((void *) 0xC0000000)

CFS_t * uhfs(LFS_t * lfs);

#endif // not __KUDOS_KFS_UHFS_H
