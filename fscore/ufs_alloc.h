#ifndef __FSTITCH_FSCORE_UFS_ALLOC_H
#define __FSTITCH_FSCORE_UFS_ALLOC_H

#include <fscore/oo.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>
#include <fscore/fdesc.h>
#include <fscore/feature.h>
#include <fscore/inode.h>
#include <fscore/lfs.h>
#include <lib/dirent.h>

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

#endif /* __FSTITCH_FSCORE_UFS_ALLOC_H */
