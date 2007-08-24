#ifndef __KUDOS_KFS_FILE_HIDING_CFS_H
#define __KUDOS_KFS_FILE_HIDING_CFS_H

#include <kfs/cfs.h>

CFS_t * file_hiding_cfs(CFS_t * frontend_cfs);
int     file_hiding_cfs_hide(CFS_t * cfs, inode_t ino);
int     file_hiding_cfs_unhide(CFS_t * cfs, inode_t ino);

#endif // not __KUDOS_KFS_FILE_HIDING_CFS_H
