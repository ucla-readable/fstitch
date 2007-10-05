/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>

#include <fscore/bd.h>
#include <fscore/lfs.h>
#include <fscore/modman.h>
#include <fscore/debug.h>
#include <fscore/feature.h>

#include <modules/waffle.h>
#include <modules/waffle_lfs.h>

#define WAFFLE_LFS_DEBUG 0

#if WAFFLE_LFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* values for the "purpose" parameter */
#define PURPOSE_FILEDATA 0
#define PURPOSE_DIRDATA 1
#define PURPOSE_INDIRECT 2
#define PURPOSE_DINDIRECT 3

LFS_t * waffle_lfs(BD_t * block_device)
{
	return NULL;
}
