#ifndef __KUDOS_KFS_LOOP_BD_H
#define __KUDOS_KFS_LOOP_BD_H

#include <kfs/lfs.h>
#include <kfs/bd.h>

#ifdef KFSD
BD_t * loop_bd(LFS_t * lfs, inode_t inode);
#else
/* for use in KudOS userspace, where we don't allow direct use of inode numbers */
BD_t * loop_bd(LFS_t * lfs, const char * name);
#endif

#endif /* __KUDOS_KFS_LOOP_BD_H */
