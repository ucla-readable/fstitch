#ifndef __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H
#define __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H

#include <kfs/cfs.h>

CFS_t * table_classifier_cfs(void);
int     table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs);
CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char *path);

// Mount path_cfs at path relative to the single table_classifier_cfs instance
int singleton_table_classifier_cfs_add(const char * path, CFS_t * path_cfs);
#define kfsd_add_mount(p, c) singleton_table_classifier_cfs_add(p, c)

#endif // not __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H
