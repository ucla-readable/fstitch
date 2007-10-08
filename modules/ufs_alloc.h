/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_UFS_ALLOC_H
#define __FSTITCH_MODULES_UFS_ALLOC_H

#include <fscore/oo.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>
#include <fscore/fdesc.h>
#include <fscore/feature.h>
#include <fscore/dirent.h>
#include <fscore/inode.h>
#include <fscore/lfs.h>

struct UFSmod_alloc;
typedef struct UFSmod_alloc UFSmod_alloc_t;

struct UFSmod_alloc {
	OBJECT(UFSmod_alloc_t);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_block, fdesc_t * file, int purpose);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_frag, fdesc_t * file, int purpose);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_inode, fdesc_t * file, int purpose);
};

#define UFS_ALLOC_INIT(ufs, module) { \
	OBJ_INIT(ufs, module); \
	ASSIGN(ufs, module, find_free_block); \
	ASSIGN(ufs, module, find_free_frag); \
	ASSIGN(ufs, module, find_free_inode); \
}

#endif /* __FSTITCH_MODULES_UFS_ALLOC_H */
