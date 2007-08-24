#ifndef __FSTITCH_FSCORE_LOOP_BD_H
#define __FSTITCH_FSCORE_LOOP_BD_H

#include <fscore/lfs.h>
#include <fscore/bd.h>

#ifdef FSTITCHD
BD_t * loop_bd(LFS_t * lfs, inode_t inode);
#else
/* for use in KudOS userspace, where we don't allow direct use of inode numbers */
BD_t * loop_bd(LFS_t * lfs, const char * name);
#endif

#endif /* __FSTITCH_FSCORE_LOOP_BD_H */
