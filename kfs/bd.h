#ifndef __KUDOS_KFS_BD_H
#define __KUDOS_KFS_BD_H

#include <inc/types.h>

#include <kfs/oo.h>

struct BD;
typedef struct BD BD_t;

#include <kfs/bdesc.h>

struct BD {
	OBJECT(BD_t);
	DECLARE(BD_t, uint16_t, get_devlevel);
	DECLARE(BD_t, uint32_t, get_numblocks);
	DECLARE(BD_t, uint16_t, get_blocksize);
	DECLARE(BD_t, uint16_t, get_atomicsize);
	DECLARE(BD_t, bdesc_t *, read_block, uint32_t number);
	DECLARE(BD_t, int, write_block, bdesc_t * block);
	DECLARE(BD_t, int, sync, bdesc_t * block);
};

#define BD_INIT(bd, module, info) { \
	OBJ_INIT(bd, module, info); \
	ASSIGN(bd, module, get_numblocks); \
	ASSIGN(bd, module, get_devlevel); \
	ASSIGN(bd, module, get_blocksize); \
	ASSIGN(bd, module, get_atomicsize); \
	ASSIGN(bd, module, read_block); \
	ASSIGN(bd, module, write_block); \
	ASSIGN(bd, module, sync); \
}

#endif /* __KUDOS_KFS_BD_H */
