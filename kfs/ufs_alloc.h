#ifndef __KUDOS_KFS_UFS_ALLOC_H
#define __KUDOS_KFS_UFS_ALLOC_H

#include <lib/types.h>

#include <kfs/oo.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/fdesc.h>
#include <kfs/feature.h>
#include <kfs/inode.h>
#include <kfs/lfs.h>
#include <lib/dirent.h>

struct UFSmod_alloc;
typedef struct UFSmod_alloc UFSmod_alloc_t;

struct UFSmod_alloc {
	OBJECT(UFSmod_alloc_t);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_block, fdesc_t * file, int purpose);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_frag, fdesc_t * file, int purpose);
	DECLARE(UFSmod_alloc_t, uint32_t, find_free_inode, fdesc_t * file, int purpose);
};

#define UFS_ALLOC_INIT(ufs, module, info) { \
	OBJ_INIT(ufs, module, info); \
	ASSIGN(ufs, module, find_free_block); \
	ASSIGN(ufs, module, find_free_frag); \
	ASSIGN(ufs, module, find_free_inode); \
}

#endif /* __KUDOS_KFS_UFS_ALLOC_H */
