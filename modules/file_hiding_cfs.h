#ifndef __FSTITCH_FSCORE_FILE_HIDING_CFS_H
#define __FSTITCH_FSCORE_FILE_HIDING_CFS_H

#include <fscore/cfs.h>

CFS_t * file_hiding_cfs(CFS_t * frontend_cfs);
int     file_hiding_cfs_hide(CFS_t * cfs, inode_t ino);
int     file_hiding_cfs_unhide(CFS_t * cfs, inode_t ino);

#endif // not __FSTITCH_FSCORE_FILE_HIDING_CFS_H
