#ifndef __KUDOS_KFS_LOOP_BD_H
#define __KUDOS_KFS_LOOP_BD_H

#include <lib/types.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

BD_t * loop_bd(LFS_t * lfs, inode_t inode);

#endif /* __KUDOS_KFS_LOOP_BD_H */
