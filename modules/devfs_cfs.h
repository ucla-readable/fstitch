#ifndef __FSTITCH_FSCORE_DEVFS_CFS_H
#define __FSTITCH_FSCORE_DEVFS_CFS_H

#include <fscore/cfs.h>
#include <fscore/bd.h>

CFS_t * devfs_cfs(const char * names[], BD_t * bds[], size_t num_entries);

int devfs_bd_add(CFS_t * cfs, const char * name, BD_t * bd);
BD_t * devfs_bd_remove(CFS_t * cfs, const char * name);

#endif // not __FSTITCH_FSCORE_DEVFS_CFS_H
