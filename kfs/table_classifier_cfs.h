#ifndef __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H
#define __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H

#include <kfs/cfs.h>

CFS_t * table_classifier_cfs(const char * paths[], CFS_t * cfses[], size_t num_entries);
int     table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs);
CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char *path);

#endif // not __KUDOS_KFS_TABLE_CLASSIFIER_CFS_H
