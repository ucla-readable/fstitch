#ifndef __KUDOS_KFS_BD_H
#define __KUDOS_KFS_BD_H

#include <inc/types.h>

#include <kfs/oo.h>
#include <kfs/bdesc.h>

/* struct bdesc needs BD, so we avoid the cycle */
struct bdesc;

struct BD;
typedef struct BD BD_t;

struct BD {
	DESTRUCTOR(BD_t);
	DECLARE(BD_t, uint32_t, get_numblocks);
	DECLARE(BD_t, uint16_t, get_blocksize);
	DECLARE(BD_t, uint16_t, get_atomicsize);
	DECLARE(BD_t, struct bdesc *, read_block, uint32_t number);
	DECLARE(BD_t, int, write_block, struct bdesc * block);
	DECLARE(BD_t, int, sync, struct bdesc * block);
	void * instance;
};

#endif /* __KUDOS_KFS_BD_H */
