/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_FILE_HIDING_CFS_H
#define __FSTITCH_FSCORE_FILE_HIDING_CFS_H

#include <fscore/cfs.h>

CFS_t * file_hiding_cfs(CFS_t * frontend_cfs);
int     file_hiding_cfs_hide(CFS_t * cfs, inode_t ino);
int     file_hiding_cfs_unhide(CFS_t * cfs, inode_t ino);

#endif // not __FSTITCH_FSCORE_FILE_HIDING_CFS_H
