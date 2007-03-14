#ifndef __KFS_BLOCK_ALLOC_H
#define __KFS_BLOCK_ALLOC_H

#include <lib/hash_map.h>
#include <kfs/chdesc.h>

struct block_alloc_head {
	/* block number -> chdesc that clears all pointers to it */
	hash_map_t * map;
};
typedef struct block_alloc_head block_alloc_head_t;

/* Add a block to the block alloc head, along with a chdesc which clears all
 * pointers to it. Later data written to the block during a subsequent
 * allocation need only depend on that chdesc, and not on the actual allocation. */
int block_alloc_set_freed(block_alloc_head_t * alloc, uint32_t block, chdesc_t * clear);

/* Convert the provided head into one which depends not only on the input head
 * but also on the chdesc which clears all pointers to it registered above. */
int block_alloc_get_freed(block_alloc_head_t * alloc, uint32_t block, chdesc_t ** head);

/* Inform the block alloc head that the given block has now been allocated, and
 * that tracking it is no longer necessary. */
int block_alloc_notify_alloc(block_alloc_head_t * alloc, uint32_t block);

int block_alloc_head_init(block_alloc_head_t * alloc);
void block_alloc_head_destroy(block_alloc_head_t * alloc);

#endif /* __KFS_BLOCK_ALLOC_H */
