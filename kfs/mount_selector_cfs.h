#ifndef __KUDOS_KFS_MOUNT_SELECTOR_CFS_H
#define __KUDOS_KFS_MOUNT_SELECTOR_CFS_H

#include <kfs/cfs.h>

CFS_t * mount_selector_cfs(void);
int     mount_selector_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs);
CFS_t * mount_selector_cfs_remove(CFS_t * cfs, const char *path);

// Mount path_cfs at path relative to the single mount_selector_cfs instance
int singleton_mount_selector_cfs_add(const char * path, CFS_t * path_cfs);
#define kfsd_add_mount(p, c) singleton_mount_selector_cfs_add(p, c)

void mount_selector_cfs_set(CFS_t * cfs);
#define kfsd_set_mount(c) mount_selector_cfs_set(c)

#endif // not __KUDOS_KFS_MOUNT_SELECTOR_CFS_H
