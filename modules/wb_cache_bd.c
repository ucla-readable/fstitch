/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h> // HZ
#include <lib/hash_map.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/revision.h>

#include <modules/wb_cache_bd.h>

/* try to flush every second */
#define FLUSH_PERIOD HZ

/* This file implements the first whack at our new WB cache. It's an LRU cache,
 * and it allows you to give it patches with unsatisfied dependencies. However,
 * it will fill up and deadlock if you give it too many. If this ever causes a
 * problem, an appropriate error message is displayed on the console and -EBUSY
 * is returned. */

#define DEBUG_TIMING 0
#include <fscore/kernel_timing.h>
KERNEL_TIMING(wait);

/* This structure is optimized for memory footprint with unions.
 * The items in each union are never used at the same time. */
struct cache_slot {
	union {
		bdesc_t * block;
		/* free_index is used in blocks[0] */
		uint32_t free_index;
	};
	union {
		struct cache_slot * prev;
		/* next_index is used in free blocks */
		uint32_t next_index;
		/* lru is used in blocks[0] */
		struct cache_slot * lru;
	};
	union {
		struct cache_slot * next;
		/* mru is used in blocks[0] */
		struct cache_slot * mru;
	};
};

struct cache_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint32_t size;
	struct cache_slot * blocks;
	hash_map_t * block_map;
};

static uint32_t wb_push_block(struct cache_info * info, bdesc_t * block, uint32_t number)
{
	uint32_t index = info->blocks[0].free_index;
	
	assert(index && index <= info->size && !info->blocks[index].block);
	assert(!hash_map_find_val(info->block_map, (void *) number));
	
	if(hash_map_insert(info->block_map, (void *) number, (void *) index) < 0)
		return INVALID_BLOCK;
	
	info->blocks[index].block = block;
	block->cache_number = number;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_BDESC_NUMBER, block, number, 1);
	
	/* not a free slot anymore */
	info->blocks[0].free_index = info->blocks[index].next_index;
	info->blocks[index].prev = &info->blocks[0];
	info->blocks[index].next = info->blocks[0].mru;
	
	/* this will set blocks[0].lru if this is the first block */
	info->blocks[0].mru->prev = &info->blocks[index];
	info->blocks[0].mru = &info->blocks[index];
	
	bdesc_retain(block);
	
	return index;
}

static void wb_pop_block(struct cache_info * info, uint32_t number, uint32_t index)
{
	assert(info->blocks[index].block);
	assert(info->blocks[index].block->cache_number == number);
	
	bdesc_release(&info->blocks[index].block);
	
	/* this will set blocks[0].lru/mru as necessary */
	info->blocks[index].prev->next = info->blocks[index].next;
	info->blocks[index].next->prev = info->blocks[index].prev;
	
	/* now it's a free slot */
	info->blocks[index].next_index = info->blocks[0].free_index;
	info->blocks[index].next = &info->blocks[info->blocks[0].free_index];
	info->blocks[0].free_index = index;
	
	hash_map_erase(info->block_map, (void *) number);
}

static void wb_touch_block(struct cache_info * info, uint32_t index)
{
	assert(info->blocks[index].block);
	
	if(info->blocks[0].mru != &info->blocks[index])
	{
		/* this will set blocks[0].lru/mru as necessary */
		info->blocks[index].prev->next = info->blocks[index].next;
		info->blocks[index].next->prev = info->blocks[index].prev;
		
		info->blocks[index].prev = &info->blocks[0];
		info->blocks[index].next = info->blocks[0].mru;
		
		/* this will set blocks[0].lru if this is the first block */
		info->blocks[0].mru->prev = &info->blocks[index];
		info->blocks[0].mru = &info->blocks[index];
	}
}

static int wb_flush_block(BD_t * object, struct cache_slot * slot)
{
	struct cache_info * info = (struct cache_info *) object;
	revision_slice_t slice;
	patch_t * patch;
	int r;
	
	/* in flight? */
	if(slot->block->ddesc->in_flight)
		return FLUSH_NONE;
	
	/* already flushed? */
	for(patch = slot->block->ddesc->all_patches; patch; patch = patch->ddesc_next)
		if(patch->owner == object)
			break;
	if(!patch)
		return FLUSH_EMPTY;
	if(!slot->block->ddesc->ready_patches[object->level].head)
		return FLUSH_NONE;
	
	r = revision_slice_create(slot->block, object, info->bd, &slice);
	if(r < 0)
	{
		fprintf(stderr, "%s() returned %i; can't flush!\n", __FUNCTION__, r);
		return FLUSH_NONE;
	}
	
	if(!slice.ready_size)
	{
		revision_slice_pull_up(&slice);
		/* otherwise we would have caught it above... */
		r = FLUSH_NONE;
	}
	else
	{
		r = CALL(info->bd, write_block, slot->block, slot->block->cache_number);
		if(r < 0)
		{
			revision_slice_pull_up(&slice);
			r = FLUSH_NONE;
		}
		else
			r = (slice.all_ready ? FLUSH_DONE : FLUSH_SOME);
	}
	
	revision_slice_destroy(&slice);
	
	return r;
}

/* evict_block should evict exactly one block if it is successful */
static int wb_evict_block(BD_t * object, bool only_dirty)
{
	struct cache_info * info = (struct cache_info *) object;
	
#ifdef __KERNEL__
	revision_tail_process_landing_requests();
#endif
	for(;;)
	{
		int r = FLUSH_EMPTY;
		struct cache_slot * slot;
		for(slot = info->blocks[0].lru; slot != &info->blocks[0]; slot = slot->prev)
		{
			int code = wb_flush_block(object, slot);
			if(code == FLUSH_DONE || (!only_dirty && code == FLUSH_EMPTY))
			{
				wb_pop_block(info, slot->block->cache_number, (uint32_t) (slot - &info->blocks[0]));
				return 0;
			}
			r |= code;
		}
#ifdef __KERNEL__
		/* For both FLUSH_NONE and FLUSH_SOME we must wait to make
		 * progress if there are any flights in progress. For FLUSH_NONE
		 * this is obvious; for FLUSH_SOME you must consider that the
		 * only way more blocks can be written is by waiting for the
		 * blocks that were just written to be completed, assuming that
		 * we do not have stacked caches. */
		if(revision_tail_flights_exist())
		{
			KERNEL_INTERVAL(wait);
			TIMING_START(wait);
			revision_tail_wait_for_landing_requests();
			revision_tail_process_landing_requests();
			TIMING_STOP(wait, wait);
		}
		else
#endif
		if(r == FLUSH_NONE)
			return -EBUSY;
	}
}

static bdesc_t * wb_cache_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * block;
	uint32_t index;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
	{
		/* in the cache, use it */
		block = info->blocks[index].block;
		assert(block->ddesc->length == count * object->blocksize);
		wb_touch_block(info, index);
		if(!block->ddesc->synthetic)
			return block;
	}
	else
	{
		if(hash_map_size(info->block_map) == info->size)
			if(wb_evict_block(object, 0) < 0)
				/* no room in cache, and can't evict anything... */
				return NULL;
		assert(hash_map_size(info->block_map) < info->size);
	}
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, read_block, number, count, page);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else if(wb_push_block(info, block, number) == INVALID_BLOCK)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static bdesc_t * wb_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * block;
	uint32_t index;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
	{
		/* in the cache, use it */
		assert(info->blocks[index].block->ddesc->length == count * object->blocksize);
		wb_touch_block(info, index);
		return info->blocks[index].block;
	}
	
	if(hash_map_size(info->block_map) == info->size)
		if(wb_evict_block(object, 0) < 0)
			/* no room in cache, and can't evict anything... */
			return NULL;
	assert(hash_map_size(info->block_map) < info->size);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, synthetic_read_block, number, count, page);
	if(!block)
		return NULL;
	
	index = wb_push_block(info, block, number);
	if(index == INVALID_BLOCK)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static int wb_cache_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object;
	uint32_t index;
	
	/* make sure it's a valid block */
	assert(block->ddesc->length && number + block->ddesc->length / object->blocksize <= object->numblocks);
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
	{
		/* already have this block */
		assert(info->blocks[index].block->ddesc == block->ddesc);
		wb_touch_block(info, index);
		
		return 0;
	}
	else
	{
		if(hash_map_size(info->block_map) == info->size)
			if(wb_evict_block(object, 0) < 0)
				/* no room in cache, and can't evict anything... */
				return -EBUSY;
		assert(hash_map_size(info->block_map) < info->size);
		
		index = wb_push_block(info, block, number);
		if(index == INVALID_BLOCK)
			return -ENOMEM;
		
		return 0;
	}
}

static int wb_cache_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
	int dirty, start_dirty = wb_cache_dirty_count(object);

	if(!start_dirty)
		return FLUSH_EMPTY;

	/* evict_block will evict exactly one block if it is successful */
	for(dirty = start_dirty; dirty; dirty--)
		if(wb_evict_block(object, 1) < 0)
		{
			assert(dirty == wb_cache_dirty_count(object));
			return (start_dirty == dirty) ? FLUSH_NONE : FLUSH_SOME;
		}
	
	assert(!wb_cache_dirty_count(object));
	
	return FLUSH_DONE;
}

static patch_t ** wb_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t wb_cache_bd_get_block_space(BD_t * object)
{
	/* the classic wb_cache does not support get_block_space, so it returns 0 */
	return 0;
}

static void wb_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	struct cache_info * info = (struct cache_info *) object;
	struct cache_slot * slot;
	
	/* FIXME: make this more efficient by only doing dirty blocks? */
	/* FIXME: try to come up with a good flush ordering, instead of waiting for the next callback? */
	for(slot = info->blocks[0].lru; slot != &info->blocks[0]; slot = slot->prev)
	{
#ifdef __KERNEL__
		revision_tail_process_landing_requests();
#endif
		wb_flush_block(object, slot);
	}
}

static int wb_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd;
	uint32_t block;
	int r;
	
	if(wb_cache_dirty_count(bd) != 0)
	{
		r = CALL(bd, flush, FLUSH_DEVICE, NULL);
		if(r < 0)
			return -EBUSY;
	}
	assert(!wb_cache_dirty_count(bd));
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	sched_unregister(wb_cache_bd_callback, bd);
	
	hash_map_destroy(info->block_map);
	
	/* the blocks are all clean, because we checked above - just release them */
	for(block = 1; block <= info->size; block++)
		if(info->blocks[block].block)
			bdesc_release(&info->blocks[block].block);
	
	sfree(info->blocks, (info->size + 1) * sizeof(*info->blocks));
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	TIMING_DUMP(wait, "wb_cache wait", "waits");
	
	return 0;
}

BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks)
{
	uint32_t i;
	BD_t *bd;
	struct cache_info * info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	/* allocate an extra cache slot: hash maps return NULL on failure, so we
	 * can't have 0 be a valid index... besides, we need pointers to the
	 * head and tail of the LRU block queue */
	info->blocks = smalloc((blocks + 1) * sizeof(*info->blocks));
	if(!info->blocks)
	{
		free(info);
		return NULL;
	}
	/* set up the block cache pointers... this could all be in
	 * the loop, but it is unwound a bit for clarity here */
	info->blocks[0].free_index = 1;
	info->blocks[0].lru = &info->blocks[0];
	info->blocks[0].mru = &info->blocks[0];
	info->blocks[1].block = NULL;
	if(blocks > 1)
	{
		info->blocks[1].next_index = 2;
		info->blocks[1].next = &info->blocks[2];
		info->blocks[blocks].block = NULL;
		info->blocks[blocks].next_index = 0;
		info->blocks[blocks].next = NULL;
	}
	else
	{
		info->blocks[1].next_index = 0;
		info->blocks[1].next = NULL;
	}
	for(i = 2; i < blocks; i++)
	{
		info->blocks[i].block = NULL;
		info->blocks[i].next_index = i + 1;
		info->blocks[i].next = &info->blocks[i + 1];
	}
	
	info->block_map = hash_map_create();
	if(!info->block_map)
	{
		sfree(info->blocks, (blocks + 1) * sizeof(*info->blocks));
		free(info);
		return NULL;
	}
	
	BD_INIT(bd, wb_cache_bd);
	OBJMAGIC(bd) = WB_CACHE_MAGIC;
	
	info->bd = disk;
	info->size = blocks;
	bd->numblocks = disk->numblocks;
	bd->blocksize = disk->blocksize;
	bd->atomicsize = disk->atomicsize;
	
	/* we generally delay blocks, so our level goes up */
	bd->level = disk->level + 1;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
	/* set up the callback */
	if(sched_register(wb_cache_bd_callback, bd, FLUSH_PERIOD) < 0)
	{
		DESTROY(bd);
		return NULL;
	}
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	if(modman_inc_bd(disk, bd, NULL) < 0)
	{
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}

uint32_t wb_cache_dirty_count(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd;
	uint32_t i, dirty = 0;
	
	if(OBJMAGIC(bd) != WB_CACHE_MAGIC)
		return INVALID_BLOCK;
	
	for(i = 1; i <= info->size; i++)
		if(info->blocks[i].block && info->blocks[i].block->ddesc->all_patches)
		{
			patch_t * scan = info->blocks[i].block->ddesc->all_patches;
			while(scan)
			{
				if(scan->owner == bd)
					break;
				scan = scan->ddesc_next;
			}
			if(scan)
			{
				dirty++;
				continue;
			}
		}
	
	return dirty;
}
