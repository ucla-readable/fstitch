#ifndef __KUDOS_KFS_BLOCKMAN_H
#define __KUDOS_KFS_BLOCKMAN_H

#include <lib/hash_map.h>

/* We can't include bd.h because it includes bdesc.h, which we can't include
 * until after we define blockman, because it includes blockman.h... sigh. */
struct BD;

typedef void (*destroy_notify_t)(struct BD * bd, uint32_t block, uint16_t length);

struct blockman {
	uint16_t length;
	struct BD * owner;
	destroy_notify_t destroy_notify;
	hash_map_t * map;
};
typedef struct blockman blockman_t;

#include <kfs/bdesc.h>
#include <kfs/bd.h>

blockman_t * blockman_create(uint16_t length, BD_t * owner, destroy_notify_t destroy_notify);
void blockman_destroy(blockman_t ** blockman);

int blockman_add(blockman_t * blockman, uint32_t number, datadesc_t * ddesc);
int blockman_remove(datadesc_t * ddesc);

datadesc_t * blockman_lookup(blockman_t * blockman, uint32_t number);

int blockman_managed_add(blockman_t * blockman, bdesc_t *bdesc, uint32_t number);
bdesc_t * blockman_managed_lookup(blockman_t * blockman, uint32_t number);

#endif /* __KUDOS_KFS_BLOCKMAN_H */
