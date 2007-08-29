/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/hash_map.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/blockman.h>

#define BLOCKMAN_DEBUG 0
#define DISABLE_ORPHAN_WARNING 0

#if BLOCKMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define BLOCKMAN_CAPACITY 16384

int blockman_init(blockman_t *man)
{
	man->capacity = BLOCKMAN_CAPACITY;
	if (!(man->map = (bdesc_t **) malloc(man->capacity * sizeof(bdesc_t *))))
		return -1;
	memset(man->map, 0, man->capacity * sizeof(bdesc_t *));
	return 0;
}

void blockman_destroy(blockman_t *man)
{
	bdesc_t **bptr;
	bdesc_t **bendptr = (man->map ? man->map + man->capacity : man->map);
	for (bptr = man->map; bptr != bendptr; ++bptr)
		while (*bptr) {
			(*bptr)->disk_hash.pprev = NULL;
			*bptr = (*bptr)->disk_hash.next;
		}
	free(man->map);
}
