#ifndef __KUDOS_KFS_EXT2_SUPER_H
#define __KUDOS_KFS_EXT2_SUPER_H

#include <lib/types.h>
#include <kfs/oo.h>
#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/ext2_base.h>

typedef struct EXT2mod_super EXT2mod_super_t;
struct EXT2mod_super {
	OBJECT(EXT2mod_super_t);
	DECLARE(EXT2mod_super_t, struct EXT2_Super *, read);
	DECLARE(EXT2mod_super_t, struct EXT2_group_desc *, read_gdescs);
	DECLARE(EXT2mod_super_t, int, write_gdesc, uint32_t, int32_t, int32_t, int32_t);
	DECLARE(EXT2mod_super_t, int, inodes, int32_t ninodes);
	DECLARE(EXT2mod_super_t, int, blocks, int32_t blocks);
	DECLARE(EXT2mod_super_t, int, wtime, int32_t wtime);
	DECLARE(EXT2mod_super_t, int, mount_time, int32_t mount_time);
	DECLARE(EXT2mod_super_t, int, sync, chdesc_t ** head);
};

#define EXT2_SUPER_INIT(ext2, module, info) { \
	OBJ_INIT(ext2, module, info); \
	ASSIGN(ext2, module, read); \
	ASSIGN(ext2, module, read_gdescs); \
	ASSIGN(ext2, module, write_gdesc); \
	ASSIGN(ext2, module, inodes); \
	ASSIGN(ext2, module, blocks); \
	ASSIGN(ext2, module, wtime); \
	ASSIGN(ext2, module, mount_time); \
	ASSIGN(ext2, module, sync); \
}

#endif /* __KUDOS_KFS_EXT2_SUPER_H */
