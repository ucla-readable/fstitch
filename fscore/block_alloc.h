/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_BLOCK_ALLOC_H
#define __FSTITCH_FSCORE_BLOCK_ALLOC_H

#include <lib/hash_map.h>
#include <fscore/patch.h>

#if PATCH_WEAKREF_CALLBACKS
#define BLOCK_ALLOC_DEPS 1
#else
/* This library uses patch callbacks. If we don't have them, disable it. */
#define BLOCK_ALLOC_DEPS 0
#endif

#if BLOCK_ALLOC_DEPS

/* note that this structure is copied by layered LFS modules,
 * so it should contain only pointers and read-only data */
struct block_alloc_head {
	/* block number -> patch that clears all pointers to it */
	hash_map_t * map;
};
typedef struct block_alloc_head block_alloc_head_t;
#define BLOCK_ALLOC_HEAD_INIT(head) do { (head)->map = NULL; } while(0)
#define BLOCK_ALLOC_HEAD_VALID(head) ((head)->map != NULL)

/* Add a block to the block alloc head, along with a patch which clears all
 * pointers to it. Later data written to the block during a subsequent
 * allocation need only depend on that patch, and not on the actual allocation. */
int block_alloc_set_freed(block_alloc_head_t * alloc, uint32_t block, patch_t * clear);

/* Convert the provided head into one which depends not only on the input head
 * but also on the patch which clears all pointers to it registered above. */
int block_alloc_get_freed(block_alloc_head_t * alloc, uint32_t block, patch_t ** head);

/* Inform the block alloc head that the given block has now been allocated, and
 * that tracking it is no longer necessary. */
int block_alloc_notify_alloc(block_alloc_head_t * alloc, uint32_t block);

int block_alloc_head_init(block_alloc_head_t * alloc);
void block_alloc_head_destroy(block_alloc_head_t * alloc);

#endif /* BLOCK_ALLOC_DEPS */

#endif /* __FSTITCH_FSCORE_BLOCK_ALLOC_H */
