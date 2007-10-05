/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_EXT2FS_BASE_H
#define __FSTITCH_MODULES_EXT2FS_BASE_H

#include <fscore/bd.h>
#include <fscore/lfs.h>

LFS_t * ext2_lfs(BD_t * block_device);

#endif /* __FSTITCH_MODULES_EXT2FS_BASE_H */
