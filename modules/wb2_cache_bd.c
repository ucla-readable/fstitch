/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>
#include <lib/pool.h>

#include <fscore/fstitchd.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/debug.h>
#include <fscore/revision.h>

#include <modules/wb2_cache_bd.h>

/* try to flush every second */
#define FLUSH_PERIOD HZ

#define DEBUG_TIMING 0
#include <fscore/kernel_timing.h>
KERNEL_TIMING(wait);

/* useful for looking at patch graphs */
#define DELAY_FLUSH_UNTIL_EXIT 0

#define MAP_SIZE 32768

/* the all list is ordered by read/write usage, while the dirty list is ordered by write usage:
 * all.first -> most recently used -> next -> next -> least recently used <- all.last
 * dirty.first -> most recently written -> next -> next -> least recently written <- dirty.last */
struct cache_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint32_t soft_blocks, blocks;
	uint32_t soft_dblocks, dblocks;
	uint32_t soft_dblocks_low, soft_dblocks_high;
	struct {
		bdesc_t * first;
		bdesc_t * last;
	} all, dirty;
	
	/* map from block number to bdesc */
	uint32_t map_capacity;
	bdesc_t ** map;
};

static inline bdesc_t * wb2_map_get_block(struct cache_info * info, uint32_t number)
{
	bdesc_t * b = info->map[number & (info->map_capacity - 1)];
	while(b && b->cache_number < number)
		b = b->block_hash.next;
	return (b && b->cache_number == number) ? b : NULL;
}

static inline void wb2_map_put_block(struct cache_info * info, bdesc_t * block, uint32_t number)
{
	bdesc_t ** b = &info->map[number & (info->map_capacity - 1)];
	while(*b && (*b)->cache_number < number)
		b = &(*b)->block_hash.next;
	block->block_hash.next = *b;
	if(*b)
		(*b)->block_hash.pprev = &block->block_hash.next;
	block->block_hash.pprev = b;
	block->cache_number = number;
	*b = block;
}

static inline void wb2_map_remove_block(bdesc_t * block)
{
	if((*block->block_hash.pprev = block->block_hash.next))
		block->block_hash.next->block_hash.pprev = block->block_hash.pprev;
}

/* we are guaranteed that the block is not already in the list */
static void wb2_push_block(struct cache_info * info, bdesc_t * block, uint32_t number)
{
#if DIRTY_QUEUE_REORDERING
	block->pass = 0;
	block->block_after_number = INVALID_BLOCK;
	block->block_after_pass = 0;
#endif
	block->lru_all.prev = NULL;
	block->lru_all.next = info->all.first;
	block->lru_dirty.prev = NULL;
	block->lru_dirty.next = NULL;
	
	assert(!wb2_map_get_block(info, number));
	wb2_map_put_block(info, block, number);
	
	info->all.first = block;
	if(block->lru_all.next)
		block->lru_all.next->lru_all.prev = block;
	else
		info->all.last = block;
	info->blocks++;
	
	bdesc_retain(block);
}

/* we are guaranteed that the block is not already in the list */
static void wb2_push_dirty(struct cache_info * info, bdesc_t * block)
{
	block->lru_dirty.prev = NULL;
	block->lru_dirty.next = info->dirty.first;
	
	info->dirty.first = block;
	if(block->lru_dirty.next)
		block->lru_dirty.next->lru_dirty.prev = block;
	else
		info->dirty.last = block;
	/* if we go above the high mark, set the current mark low */
	if(++info->dblocks > info->soft_dblocks_high)
		info->soft_dblocks = info->soft_dblocks_low;
}

#define wb2_dirty_slot(info, block) ((info)->dirty.first == (block) || (block)->lru_dirty.prev)

static void wb2_pop_slot(struct cache_info * info, bdesc_t * block)
{
	assert(wb2_map_get_block(info, block->cache_number) == block);
	
	if(block->lru_all.prev)
		block->lru_all.prev->lru_all.next = block->lru_all.next;
	else
		info->all.first = block->lru_all.next;
	if(block->lru_all.next)
		block->lru_all.next->lru_all.prev = block->lru_all.prev;
	else
		info->all.last = block->lru_all.prev;
	if(wb2_dirty_slot(info, block))
	{
		if(block->lru_dirty.prev)
			block->lru_dirty.prev->lru_dirty.next = block->lru_dirty.next;
		else
			info->dirty.first = block->lru_dirty.next;
		if(block->lru_dirty.next)
			block->lru_dirty.next->lru_dirty.prev = block->lru_dirty.prev;
		else
			info->dirty.last = block->lru_dirty.prev;
	}
	
	wb2_map_remove_block(block);
	bdesc_release(&block);
}

static void wb2_pop_slot_dirty(struct cache_info * info, bdesc_t * block)
{
	assert(wb2_dirty_slot(info, block));
	if(block->lru_dirty.prev)
		block->lru_dirty.prev->lru_dirty.next = block->lru_dirty.next;
	else
		info->dirty.first = block->lru_dirty.next;
	if(block->lru_dirty.next)
		block->lru_dirty.next->lru_dirty.prev = block->lru_dirty.prev;
	else
		info->dirty.last = block->lru_dirty.prev;
	block->lru_dirty.prev = NULL;
	block->lru_dirty.next = NULL;
	/* if we make it below the low mark, set the current mark high */
	if(--info->dblocks <= info->soft_dblocks_low)
		info->soft_dblocks = info->soft_dblocks_high;
}

static void wb2_touch_block_read(struct cache_info * info, bdesc_t * block)
{
	/* already the first? */
	if(info->all.first == block)
		return;
	
	/* must have a prev, so detach it */
	block->lru_all.prev->lru_all.next = block->lru_all.next;
	if(block->lru_all.next)
		block->lru_all.next->lru_all.prev = block->lru_all.prev;
	else
		info->all.last = block->lru_all.prev;
	
	/* now re-add it */
	block->lru_all.prev = NULL;
	block->lru_all.next = info->all.first;
	info->all.first = block;
	if(block->lru_all.next)
		block->lru_all.next->lru_all.prev = block;
	else
		info->all.last = block;
}

static int wb2_flush_block(BD_t * object, bdesc_t * block, int * delay)
{
	struct cache_info * info = (struct cache_info *) object;
	revision_slice_t slice;
	int r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_CACHE, FDB_CACHE_LOOKBLOCK, object, block);
	
	if(delay)
		*delay = 0;
	
	/* in flight? */
	if(block->in_flight)
		return FLUSH_NONE;
	
	/* already flushed? */
	if(!block->index_patches[object->graph_index].head)
		return FLUSH_EMPTY;
	
	r = revision_slice_create(block, object, info->bd, &slice);
	if(r < 0)
	{
		printf("%s() returned %i; can't flush!\n", __FUNCTION__, r);
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
		int start = delay ? jiffy_time() : 0;
		r = CALL(info->bd, write_block, block, block->cache_number);
		if(r < 0)
		{
			revision_slice_pull_up(&slice);
			r = FLUSH_NONE;
		}
		else
		{
			if(delay)
				*delay = jiffy_time() - start;
			r = (slice.all_ready ? FLUSH_DONE : FLUSH_SOME);
			FSTITCH_DEBUG_SEND(FDB_MODULE_CACHE, FDB_CACHE_WRITEBLOCK, object, block, block->flags);
		}
	}
	
	revision_slice_destroy(&slice);
	
	return r;
}

#if DIRTY_QUEUE_REORDERING
static bdesc_t * wb2_find_block_before(BD_t * object, patch_t * patch, bdesc_t * start_block)
{
	patchdep_t * dep = patch->befores;
	for(; dep; dep = dep->before.next)
	{
		patch_t * before = dep->before.desc;
		if(before->owner != object)
			continue;
		if(!before->block)
		{
			bdesc_t * block = wb2_find_block_before(object, before, start_block);
			if(block)
				return block;
		}
		else if(before->block->ddesc != start_block->ddesc)
			return before->block;
	}
	return NULL;
}

/* move "slot" to before "before" */
static void wb2_bounce_block_write(struct cache_info * info, bdesc_t * block, bdesc_t * before)
{
	wb2_pop_slot_dirty(info, block);
	block->lru_dirty.next = before;
	block->lru_dirty.prev = before->lru_dirty.prev;
	before->lru_dirty.prev = block;
	if(block->lru_dirty.prev)
		block->lru_dirty.prev->lru_dirty.next = block;
	else
		info->dirty.first = block;
	/* there is no way we could be putting this block at the end
	 * of the queue, since it's going before some other block */
	/* if we go above the high mark, set the current mark low */
	if(++info->dblocks > info->soft_dblocks_high)
		info->soft_dblocks = info->soft_dblocks_low;
}
#endif

enum dshrink_strategy {
	CLIP,  /* just get below the soft limit */
	FLUSH, /* flush as much as possible */
	PREEN  /* flush but stop on any I/O delay */
};
/* reduce the number of dirty blocks in the cache, if possible, by writing
 * blocks out (using the specified strategy) */
static void wb2_shrink_dblocks(BD_t * object, enum dshrink_strategy strategy)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * block = info->dirty.last;
	
#if DIRTY_QUEUE_REORDERING
	bdesc_t * stop = NULL;
#define STOP stop
	static uint32_t pass = 0;
	if(!++pass)
		pass = 1;
#else
#define STOP NULL
#endif
	
#if DELAY_FLUSH_UNTIL_EXIT
	if(fstitchd_is_running())
		return;
#endif
	
#ifdef __KERNEL__
	revision_tail_process_landing_requests();
#endif
	FSTITCH_DEBUG_SEND(FDB_MODULE_CACHE, FDB_CACHE_FINDBLOCK, object);
	
	/* in clip mode, stop as soon as we are below the soft limit */
	while((info->dblocks > info->soft_dblocks || strategy != CLIP) && block != STOP)
	{
		int status, delay = 0;
#if DIRTY_QUEUE_REORDERING
		if(block->pass == pass)
		{
			block = block->lru_dirty.prev;
			assert(block || !stop);
			continue;
		}
		block->pass = pass;
#endif
		status = wb2_flush_block(object, block, &delay);
		/* still dirty? */
		if(status < 0)
		{
#if DIRTY_QUEUE_REORDERING
			this_is_not_updated_yet();
			/* pick somewhere later in the queue to move it */
			bdesc_t * scan_block = NULL;
			if(!block->in_flight)
			{
				patch_t * scan = block->index_patches[object->graph_index].head;
				for(; !scan_block && scan; scan = scan->ddesc_index_next)
					scan_block = wb2_find_block_before(object, scan, slot->block);
			}
			if(scan_block)
			{
				struct lru_slot * before_slot = NULL;
				before_slot = (struct lru_slot *) hash_map_find_val(info->block_map, (void *) scan_block->number);
				assert(slot != before_slot);
				if(before_slot)
				{
					struct lru_slot * prev = slot->dirty.prev;
					/* it had better be in the dirty list */
					assert(before_slot->dirty.prev || info->dirty.first == before_slot);
					if(before_slot->block_after_pass == pass)
					{
						struct lru_slot * try;
						try = (struct lru_slot *) hash_map_find_val(info->block_map, (void *) before_slot->block_after_number);
						if(try)
						{
							assert(slot != try);
							assert(try->dirty.prev || info->dirty.first == try);
							before_slot->block_after_number = slot->block->number;
							before_slot = try;
						}
					}
					else
					{
						before_slot->block_after_number = slot->block->number;
						before_slot->block_after_pass = pass;
					}
					wb2_bounce_block_write(info, slot, before_slot);
					if(info->dirty.first == slot && !stop && prev)
						stop = slot;
					slot = prev;
					assert(slot || !stop);
				}
				else
				{
					slot = slot->dirty.prev;
					assert(slot || !stop);
				}
			}
			else
#endif
			{
				block = block->lru_dirty.prev;
				assert(block || !STOP);
			}
		}
		else
		{
			uint32_t number = block->cache_number;
			bdesc_t * prev = block->lru_dirty.prev;
			wb2_pop_slot_dirty(info, block);
			/* now try and find sequential blocks to write */
			while((block = wb2_map_get_block(info, ++number)))
			{
				if(!wb2_dirty_slot(info, block))
					break;
				/* if we were about to examine this block, don't */
				if(block == prev)
					prev = prev->lru_dirty.prev;
				/* assume it will be merged, so don't ask for delay */
				status = wb2_flush_block(object, block, NULL);
				/* clean slot now? */
				if(status >= 0)
					wb2_pop_slot_dirty(info, block);
				/* if we didn't actually write it, stop looking */
				if(status == FLUSH_EMPTY || status == FLUSH_NONE)
					break;
			}
			block = prev;
			assert(block || !STOP);
		}
		/* if we're just preening, then stop if there was I/O delay */
		if(strategy == PREEN && delay > 1)
			break;
	}
	/* After making it through the list of all dirty blocks, we will have to
	 * wait for some in-flight blocks to land before any of the still-dirty
	 * blocks will be writable. We assume that this will take a while, so we
	 * don't wait explicitly for it here - rather, the caller should wait
	 * (if appropriate) and call shrink_dblocks() again. */
}

/* reduce the number of blocks in the cache to below the soft limit, if
 * possible, by evicting clean blocks in LRU order */
static void wb2_shrink_blocks(struct cache_info * info)
{
	bdesc_t * block = info->all.last;
	/* while there are more blocks than the soft limit, and there are clean blocks */
	while(info->blocks >= info->soft_blocks && info->blocks > info->dblocks)
	{
		assert(block);
		/* skip dirty blocks */
		if(wb2_dirty_slot(info, block))
			block = block->lru_all.prev;
		else
		{
			bdesc_t * prev = block->lru_all.prev;
			wb2_pop_slot(info, block);
			info->blocks--;
			block = prev;
		}
	}
}

static bdesc_t * wb2_cache_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	block = wb2_map_get_block(info, number);
	if(block)
	{
		/* in the cache, use it */
		assert(block->length == count * object->blocksize);
		wb2_touch_block_read(info, block);
		if(!block->synthetic)
		{
			bdesc_ensure_linked_page(block, page);
			return block;
		}
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			wb2_shrink_dblocks(object, CLIP);
		if(info->blocks >= info->soft_blocks)
			wb2_shrink_blocks(info);
	}
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, read_block, number, count, page);
	if(!block)
		return NULL;
	
	if(block->synthetic)
		block->synthetic = 0;
	else
		wb2_push_block(info, block, number);
	
	return block;
}

static bdesc_t * wb2_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	block = wb2_map_get_block(info, number);
	if(block)
	{
		/* in the cache, use it */
		assert(block->length == count * object->blocksize);
		wb2_touch_block_read(info, block);
		bdesc_ensure_linked_page(block, page);
		return block;
	}
	
	if(info->dblocks > info->soft_dblocks)
		wb2_shrink_dblocks(object, CLIP);
	if(info->blocks >= info->soft_blocks)
		wb2_shrink_blocks(info);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, synthetic_read_block, number, count, page);
	if(!block)
		return NULL;
	
	wb2_push_block(info, block, number);
	return block;
}

static int wb2_cache_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object;
	bdesc_t * map_block;
	
	/* make sure it's a valid block */
	assert(block->length && number + block->length / object->blocksize <= object->numblocks);
	
	map_block = wb2_map_get_block(info, number);
	if(map_block)
	{
		/* already have this block */
		wb2_touch_block_read(info, map_block);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		if(!wb2_dirty_slot(info, map_block))
			wb2_push_dirty(info, map_block);
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			wb2_shrink_dblocks(object, CLIP);
#ifdef __KERNEL__
		else
			/* shrink_dblocks() calls revision_tail_process_landing_requests(),
			 * so only call it if we aren't calling shrink_dblocks() above */
			revision_tail_process_landing_requests();
#endif
		if(info->blocks >= info->soft_blocks)
			wb2_shrink_blocks(info);
		
		wb2_push_block(info, block, number);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		wb2_push_dirty(info, block);
	}
	
	return 0;
}

static int wb2_cache_bd_flush(BD_t * object, uint32_t blockno, patch_t * ch)
{
	struct cache_info * info = (struct cache_info *) object;
	uint32_t start_dirty = info->dblocks;

	if(!start_dirty)
		return FLUSH_EMPTY;

	for(;;)
	{
		uint32_t old_dirty = info->dblocks;
		wb2_shrink_dblocks(object, FLUSH);
		if(!info->dblocks)
			return FLUSH_DONE;
		if(info->dblocks == old_dirty)
		{
#ifdef __KERNEL__
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
				return (old_dirty == start_dirty) ? FLUSH_NONE : FLUSH_SOME;
		}
	}
}

static patch_t ** wb2_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t wb2_cache_bd_get_block_space(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return info->soft_dblocks - info->dblocks;
}

static void wb2_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	wb2_shrink_dblocks(object, PREEN);
#if DEBUG_TIMING
	struct cache_info * info = (struct cache_info *) object;
	printf("%s(): dirty %d/%d, limit %d/%d\n", __FUNCTION__, info->dblocks, info->blocks, info->soft_dblocks, info->soft_blocks);
#endif
}

static int wb2_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd;
	int r;
	
	if(info->dblocks)
	{
		r = CALL(bd, flush, FLUSH_DEVICE, NULL);
		if(r < 0)
			return -EBUSY;
	}
	assert(!info->dblocks);
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	sched_unregister(wb2_cache_bd_callback, bd);
	
	/* the blocks are all clean, because we checked above - just release them */
	while(info->all.first)
		wb2_pop_slot(info, info->all.first);
	
	free(info->map);
	memset(info, 0, sizeof(*info));
	free(info);
	
	TIMING_DUMP(wait, "wb2_cache wait", "waits");
	
	return 0;
}

BD_t * wb2_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks)
{
	struct cache_info * info;
	BD_t * bd;
	
	if(soft_dblocks > soft_blocks)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;

	info->map_capacity = MAP_SIZE;
	info->map = (bdesc_t **) malloc(info->map_capacity * sizeof(*info->map));
	if(!info->map)
	{
		free(info);
		return NULL;
	}
	memset(info->map, 0, info->map_capacity * sizeof(*info->map));
	
	BD_INIT(bd, wb2_cache_bd);
	
	info->bd = disk;
	info->soft_blocks = soft_blocks;
	info->blocks = 0;
	info->soft_dblocks_low = soft_dblocks * 9 / 10;
	info->soft_dblocks_high = soft_dblocks * 11 / 10;
	info->soft_dblocks = info->soft_dblocks_high;
	info->dblocks = 0;
	info->all.first = NULL;
	info->all.last = NULL;
	info->dirty.first = NULL;
	info->dirty.last = NULL;
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
	if(sched_register(wb2_cache_bd_callback, bd, FLUSH_PERIOD) < 0)
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
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_CACHE, FDB_CACHE_NOTIFY, bd);
	return bd;
}
