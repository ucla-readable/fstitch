#ifndef _KFS_IPC_CLIENT_H_
#define _KFS_IPC_CLIENT_H_

#include <inc/types.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

CFS_t * create_cfs(uint32_t id);
LFS_t * create_lfs(uint32_t id);
BD_t *  create_bd (uint32_t id);

int kfs_sync(const char * name);

#endif // not _KFS_IPC_CLIENT_H_
