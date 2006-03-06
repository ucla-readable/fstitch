#ifndef __KUDOS_KFS_BLOCKMAN_H
#define __KUDOS_KFS_BLOCKMAN_H

#include <lib/hash_map.h>

struct blockman {
	uint16_t length;
	hash_map_t * map;
};
typedef struct blockman blockman_t;

#include <kfs/bdesc.h>

blockman_t * blockman_create(uint16_t length);
void blockman_destroy(blockman_t ** blockman);

int blockman_add(blockman_t * blockman, uint32_t number, datadesc_t * ddesc);
int blockman_remove(datadesc_t * ddesc);

datadesc_t * blockman_lookup(blockman_t * blockman, uint32_t number);

int blockman_managed_add(blockman_t * blockman, bdesc_t * bdesc);
bdesc_t * blockman_managed_lookup(blockman_t * blockman, uint32_t number);

#endif /* __KUDOS_KFS_BLOCKMAN_H */
