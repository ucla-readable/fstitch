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

struct UFS_Alloc;
typedef struct UFS_Alloc UFS_Alloc_t;

struct UFS_Alloc {
	OBJECT(UFS_Alloc_t);
	DECLARE(UFS_Alloc_t, uint32_t, find_free_block, fdesc_t * file, int purpose);
	DECLARE(UFS_Alloc_t, uint32_t, find_free_frag, fdesc_t * file, int purpose);
	DECLARE(UFS_Alloc_t, uint32_t, find_free_inode, fdesc_t * file);
};

#define UFS_ALLOC_INIT(ufs, module, info) { \
	OBJ_INIT(ufs, module, info); \
	ASSIGN(ufs, module, find_free_block); \
	ASSIGN(ufs, module, find_free_frag); \
	ASSIGN(ufs, module, find_free_inode); \
}

#endif /* __KUDOS_KFS_UFS_ALLOC_H */
