#ifndef __KUDOS_KFS_BD_H
#define __KUDOS_KFS_BD_H

#include <inc/types.h>

#include <kfs/oo.h>

struct BD;
typedef struct BD BD_t;

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

#define SYNC_FULL_DEVICE 0xFFFFFFFF

struct BD {
	OBJECT(BD_t);
	DECLARE(BD_t, uint16_t, get_devlevel);
	DECLARE(BD_t, uint32_t, get_numblocks);
	DECLARE(BD_t, uint16_t, get_blocksize);
	DECLARE(BD_t, uint16_t, get_atomicsize);
	DECLARE(BD_t, bdesc_t *, read_block, uint32_t number);
	/* This function is used between barriers. If the block is already in
	 * memory, it is returned and *synthetic is set to 0. If not, it is not
	 * read in from disk: rather, it is synthesized and *synthetic is set to
	 * 1. Note that this behavior is only actually necessary at the terminal
	 * BD, because this is where it really hurts to do unnecessary reads. */
	DECLARE(BD_t, bdesc_t *, synthetic_read_block, uint32_t number, bool * synthetic);
	/* This function cancels a synthesized block, so that if for some reason
	 * the write which is required to follow a synthetic read cannot be
	 * completed, the synthesized block will not be returned as a normal
	 * block by a subsequent read_block call. */
	DECLARE(BD_t, int, cancel_block, uint32_t number);
	DECLARE(BD_t, int, write_block, bdesc_t * block);
	DECLARE(BD_t, int, sync, uint32_t block, chdesc_t * ch);
};

#define BD_INIT(bd, module, info) { \
	OBJ_INIT(bd, module, info); \
	ASSIGN(bd, module, get_numblocks); \
	ASSIGN(bd, module, get_devlevel); \
	ASSIGN(bd, module, get_blocksize); \
	ASSIGN(bd, module, get_atomicsize); \
	ASSIGN(bd, module, read_block); \
	ASSIGN(bd, module, synthetic_read_block); \
	ASSIGN(bd, module, cancel_block); \
	ASSIGN(bd, module, write_block); \
	ASSIGN(bd, module, sync); \
}

#endif /* __KUDOS_KFS_BD_H */
