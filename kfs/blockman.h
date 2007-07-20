#ifndef __KUDOS_KFS_BLOCKMAN_H
#define __KUDOS_KFS_BLOCKMAN_H

#include <kfs/bdesc.h>
#include <lib/hash_map.h>

/* We can't include bd.h because it includes bdesc.h, which we can't include
 * until after we define blockman, because it includes blockman.h... sigh. */
struct BD;

typedef void (*destroy_notify_t)(struct BD * bd, uint32_t block, uint16_t length);

struct blockman {
	uint32_t capacity;
	bdesc_t **map;
};
typedef struct blockman blockman_t;

#include <kfs/bdesc.h>
#include <kfs/bd.h>

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

#endif /* __KUDOS_KFS_BLOCKMAN_H */
