#ifndef __KUDOS_KFS_JOSFS_CFS_H
#define __KUDOS_KFS_JOSFS_CFS_H

#include <kfs/cfs.h>

/* The number of open files per josfs_cfs module. */
#define JOSFS_CFS_MAX_OPEN 256

CFS_t * josfs_cfs();

#endif // not __KUDOS_KFS_JOSFS_CFS_H
