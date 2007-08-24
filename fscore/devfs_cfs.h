#ifndef __KUDOS_KFS_DEVFS_CFS_H
#define __KUDOS_KFS_DEVFS_CFS_H

#include <kfs/cfs.h>
#include <kfs/bd.h>

CFS_t * devfs_cfs(const char * names[], BD_t * bds[], size_t num_entries);

int devfs_bd_add(CFS_t * cfs, const char * name, BD_t * bd);
BD_t * devfs_bd_remove(CFS_t * cfs, const char * name);

#endif // not __KUDOS_KFS_DEVFS_CFS_H
