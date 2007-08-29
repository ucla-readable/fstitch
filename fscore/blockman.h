/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_BLOCKMAN_H
#define __FSTITCH_FSCORE_BLOCKMAN_H

#include <fscore/bdesc.h>
#include <lib/hash_map.h>

struct blockman {
	uint32_t capacity;
	bdesc_t **map;
};

int blockman_init(blockman_t *blockman);
void blockman_destroy(blockman_t *blockman);

static inline void blockman_add(blockman_t *man, bdesc_t *bdesc, uint32_t number)
{
	bdesc_t **bptr;
	assert(!bdesc->disk_hash.pprev);

	bdesc->disk_number = number;
	bptr = &man->map[(number >> 3) & (man->capacity - 1)];
	while (*bptr && (*bptr)->disk_number < number)
		bptr = &(*bptr)->disk_hash.next;
	bdesc->disk_hash.pprev = bptr;
	bdesc->disk_hash.next = *bptr;
	*bptr = bdesc;
	if (bdesc->disk_hash.next)
		bdesc->disk_hash.next->disk_hash.pprev = &bdesc->disk_hash.next;
}

static inline void blockman_remove(bdesc_t *bdesc)
{
	if (bdesc->disk_hash.pprev) {
		*bdesc->disk_hash.pprev = bdesc->disk_hash.next;
		if (bdesc->disk_hash.next)
			bdesc->disk_hash.next->disk_hash.pprev = bdesc->disk_hash.pprev;
		bdesc->disk_hash.pprev = NULL;
	}
}

static inline bdesc_t *blockman_lookup(blockman_t *man, uint32_t number)
{
	bdesc_t *bdesc;
	bdesc = man->map[(number >> 3) & (man->capacity - 1)];
	while (bdesc && bdesc->disk_number < number)
		bdesc = bdesc->disk_hash.next;
	return (bdesc && bdesc->disk_number == number ? bdesc : 0);
}

#endif /* __FSTITCH_FSCORE_BLOCKMAN_H */
