#ifndef __KUDOS_KFS_BLOCKMAN_H
#define __KUDOS_KFS_BLOCKMAN_H

#include <inc/hash_map.h>

typedef hash_map_t blockman_t;

#include <kfs/bdesc.h>

blockman_t * blockman_create(void);
void blockman_destroy(blockman_t ** blockman);

int blockman_add(blockman_t * blockman, uint32_t number, datadesc_t * ddesc);
int blockman_remove(datadesc_t * ddesc);

datadesc_t * blockman_lookup(blockman_t * blockman, uint32_t number);

int blockman_managed_add(blockman_t * blockman, bdesc_t * bdesc);
bdesc_t * blockman_managed_lookup(blockman_t * blockman, uint32_t number);

#endif /* __KUDOS_KFS_BLOCKMAN_H */
