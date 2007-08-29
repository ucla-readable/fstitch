/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_BD_H
#define __FSTITCH_FSCORE_BD_H

#include <fscore/oo.h>
#include <fscore/types.h>

/* maximum number of BD levels */
#define NBDLEVEL 2
/* maximum number of BD graph indices */
#define NBDINDEX 8
/* this value represents no level */
#define BDLEVEL_NONE ((uint16_t) -1)

#define FLUSH_DEVICE 0xFFFFFFFF
#define INVALID_BLOCK 0xFFFFFFFF

/* flush() should return:
 * FLUSH_EMPTY if no flush was necessary
 * FLUSH_DONE if a flush was completed
 * FLUSH_SOME if some progress was made
 * FLUSH_NONE if no progress was made */
#define FLUSH_EMPTY ((int) 0)
#define FLUSH_DONE ((int) 1)
/* notice that FLUSH_SOME and FLUSH_NONE are negative */
#define FLUSH_SOME ((int) -2)
#define FLUSH_NONE ((int) 1 << (8 * sizeof(int) - 1))

struct BD {
	OBJECT(BD_t);
	uint16_t level, graph_index;
	uint32_t numblocks;
	uint16_t blocksize;
	uint16_t atomicsize;
	DECLARE(BD_t, bdesc_t *, read_block, uint32_t number, uint16_t count, page_t * page);
	/* This function is used to avoid unnecessary reads. If the block is
	 * already in memory, it is returned. If not, it is not read in from
	 * disk: rather, it is synthesized and its synthetic bit is set. Note
	 * that this behavior is only actually necessary at the terminal BD,
	 * because this is where it really hurts to do unnecessary reads. */
	DECLARE(BD_t, bdesc_t *, synthetic_read_block, uint32_t number, uint16_t count, page_t * page);
	DECLARE(BD_t, int, write_block, bdesc_t * block, uint32_t number);
	DECLARE(BD_t, int, flush, uint32_t block, patch_t * ch);
	DECLARE(BD_t, patch_t **, get_write_head);
	/* This function returns the number of dirtyable cache blocks in the
	 * earliest cache. It returns negative numbers to indicate that a cache
	 * already holds more dirty blocks than it wants. */
	DECLARE(BD_t, int32_t, get_block_space);
};

#define BD_INIT(bd, module) { \
	OBJ_INIT(bd, module); \
	(bd)->level = -1; (bd)->graph_index = -1;			\
	(bd)->blocksize = 0; (bd)->atomicsize = 0; (bd)->numblocks = 0;	\
	ASSIGN(bd, module, read_block); \
	ASSIGN(bd, module, synthetic_read_block); \
	ASSIGN(bd, module, write_block); \
	ASSIGN(bd, module, flush); \
	ASSIGN(bd, module, get_write_head); \
	ASSIGN(bd, module, get_block_space); \
}

#endif /* __FSTITCH_FSCORE_BD_H */
