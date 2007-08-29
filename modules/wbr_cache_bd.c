#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>
#include <lib/vector.h>
#include <lib/pool.h>

#include <fscore/fstitchd.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/debug.h>
#include <fscore/revision.h>

#include <modules/wbr_cache_bd.h>

/* This is the random write cache. It is based on the wb2 cache, and can be used
 * interchangably. It maintains a list of the dirty blocks, and tries to write
 * them in random order (except for linear scans for adjacent dirty blocks, just
 * like the wb2 cache). Surprisingly it actually does pretty well. */

/* The random order comes from a pair of 20-bit LFSRs. One is initialized in the
 * constructor and keeps its state for the lifetime of the cache, while the
 * other is reinitialized each time wbr_shrink_dblocks() is called. The first is
 * stepped once each time that function is called, and its value is xored into
 * the value of the second LFSR to get a permutation of the fixed LFSR order. It
 * is the subsequence of the resulting permuted LFSR sequence less than the size
 * of the dirty block list which determines the order blocks will be examined. */

/* try to flush every second */
#define FLUSH_PERIOD HZ

#define DEBUG_TIMING 0
#include <fscore/kernel_timing.h>
KERNEL_TIMING(wait);

/* useful for looking at patch graphs */
#define DELAY_FLUSH_UNTIL_EXIT 0

struct rand_slot {
	bdesc_t * block;
	/* LRU block list */
	struct rand_slot * prev;
	struct rand_slot * next;
	/* index in dirty list */
	size_t index;
};

/* the block list is ordered by read/write usage:
 * first -> most recently used -> next -> next -> least recently used <- last */
struct cache_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint32_t soft_blocks, blocks;
	uint32_t soft_dblocks, dblocks;
	uint32_t soft_dblocks_low, soft_dblocks_high;
	/* map from (void *) number -> rand_slot * */
	hash_map_t * block_map;
	/* list of all blocks, in LRU order */
	struct rand_slot * first;
	struct rand_slot * last;
	/* list of all dirty blocks, in random order (rand_slot *) */
	vector_t * dirty_list;
	size_t dirty_state;
};

DECLARE_POOL(rand_slot, struct rand_slot);
static int n_wbr_instances;

/* we are guaranteed that the block is not already in the list */
static struct rand_slot * wbr_push_block(struct cache_info * info, bdesc_t * block, uint32_t number)
{
	struct rand_slot * slot = rand_slot_alloc();
	if(!slot)
		return NULL;
	
	slot->block = block;
	slot->prev = NULL;
	slot->next = info->first;
	slot->index = -1;
	
	assert(!hash_map_find_val(info->block_map, (void *) number));
	if(hash_map_insert(info->block_map, (void *) number, slot) < 0)
	{
		rand_slot_free(slot);
		return NULL;
	}
	block->cache_number = number;
	
	info->first = slot;
	if(slot->next)
		slot->next->prev = slot;
	else
		info->last = slot;
	info->blocks++;
	
	bdesc_retain(block);
	
	return slot;
}

/* we are guaranteed that the block is not already in the list */
static int wbr_push_slot_dirty(struct cache_info * info, struct rand_slot * slot)
{
	int r = vector_push_back(info->dirty_list, slot);
	if(r < 0)
		return r;
	slot->index = vector_size(info->dirty_list) - 1;
	
	/* if we go above the high mark, set the current mark low */
	if(++info->dblocks > info->soft_dblocks_high)
		info->soft_dblocks = info->soft_dblocks_low;
	
	return 0;
}

static void wbr_pop_slot(struct cache_info * info, struct rand_slot * slot)
{
	uint32_t number = slot->block->cache_number;
	assert(hash_map_find_val(info->block_map, (void *) number) == slot);
	
	bdesc_release(&slot->block);
	
	if(slot->prev)
		slot->prev->next = slot->next;
	else
		info->first = slot->next;
	if(slot->next)
		slot->next->prev = slot->prev;
	else
		info->last = slot->prev;
	if(slot->index != -1)
	{
		struct rand_slot * last = vector_elt_end(info->dirty_list);
		vector_elt_set(info->dirty_list, last->index = slot->index, last);
		vector_pop_back(info->dirty_list);
	}
	
	hash_map_erase(info->block_map, (void *) number);
	rand_slot_free(slot);
}

static void wbr_pop_slot_dirty(struct cache_info * info, struct rand_slot * slot)
{
	assert(slot->index != -1);
	
	struct rand_slot * last = vector_elt_end(info->dirty_list);
	vector_elt_set(info->dirty_list, last->index = slot->index, last);
	vector_pop_back(info->dirty_list);
	slot->index = -1;
	
	/* if we make it below the low mark, set the current mark high */
	if(--info->dblocks <= info->soft_dblocks_low)
		info->soft_dblocks = info->soft_dblocks_high;
}

static void wbr_touch_block_read(struct cache_info * info, struct rand_slot * slot)
{
	/* already the first? */
	if(info->first == slot)
		return;
	
	/* must have a prev, so detach it */
	slot->prev->next = slot->next;
	if(slot->next)
		slot->next->prev = slot->prev;
	else
		info->last = slot->prev;
	
	/* now re-add it */
	slot->prev = NULL;
	slot->next = info->first;
	info->first = slot;
	if(slot->next)
		slot->next->prev = slot;
	else
		info->last = slot;
}

static int wbr_flush_block(BD_t * object, bdesc_t * block, int * delay)
{
	struct cache_info * info = (struct cache_info *) object;
	revision_slice_t slice;
	int r;
	FSTITCH_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_LOOKBLOCK, object, block);
	
	if(delay)
		*delay = 0;
	
	/* in flight? */
	if(block->ddesc->in_flight)
		return FLUSH_NONE;
	
	/* already flushed? */
	if(!block->ddesc->index_patches[object->graph_index].head)
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
			FSTITCH_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_WRITEBLOCK, object, block, block->ddesc->flags);
		}
	}
	
	revision_slice_destroy(&slice);
	
	return r;
}

#define next_state(state) ((state) = ((state) >> 1) | ((((state) & 1) ^ (((state) >> 3) & 1)) << 19))

enum dshrink_strategy {
	CLIP,  /* just get below the soft limit */
	FLUSH, /* flush as much as possible */
	PREEN  /* flush but stop on any I/O delay */
};
/* reduce the number of dirty blocks in the cache, if possible, by writing
 * blocks out (using the specified strategy) */
static void wbr_shrink_dblocks(BD_t * object, enum dshrink_strategy strategy)
{
	struct cache_info * info = (struct cache_info *) object;
	size_t left = vector_size(info->dirty_list), local_state = 1;
	
	next_state(info->dirty_state);
	
#if DELAY_FLUSH_UNTIL_EXIT
	if(fstitchd_is_running())
		return;
#endif
	
#ifdef __KERNEL__
	revision_tail_process_landing_requests();
#endif
	FSTITCH_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_FINDBLOCK, object);
	
	/* in clip mode, stop as soon as we are below the soft limit */
	while((info->dblocks > info->soft_dblocks || strategy != CLIP) && left)
	{
		size_t index;
		struct rand_slot * slot;
		int status, delay = 0;
		
		do {
			index = next_state(local_state) ^ info->dirty_state;
			index = (index ? index : info->dirty_state) - 1;
		} while(index >= vector_size(info->dirty_list));
		slot = vector_elt(info->dirty_list, index);
		left--;
		
		status = wbr_flush_block(object, slot->block, &delay);
		if(status >= 0)
		{
			uint32_t number = slot->block->cache_number;
			wbr_pop_slot_dirty(info, slot);
			/* now try and find sequential blocks to write */
			while((slot = hash_map_find_val(info->block_map, (void *) ++number)))
			{
				if(slot->index == -1)
					break;
				/* assume it will be merged, so don't ask for delay */
				status = wbr_flush_block(object, slot->block, NULL);
				/* clean slot now? */
				if(status >= 0)
					wbr_pop_slot_dirty(info, slot);
				/* if we didn't actually write it, stop looking */
				if(status == FLUSH_EMPTY || status == FLUSH_NONE)
					break;
			}
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
static void wbr_shrink_blocks(struct cache_info * info)
{
	struct rand_slot * slot = info->last;
	/* while there are more blocks than the soft limit, and there are clean blocks */
	while(info->blocks >= info->soft_blocks && info->blocks > info->dblocks)
	{
		assert(slot);
		/* skip dirty blocks */
		if(slot->index != -1)
			slot = slot->prev;
		else
		{
			struct rand_slot * prev = slot->prev;
			wbr_pop_slot(info, slot);
			info->blocks--;
			slot = prev;
		}
	}
}

static bdesc_t * wbr_cache_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	struct rand_slot * slot;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	slot = (struct rand_slot *) hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		/* in the cache, use it */
		block = slot->block;
		assert(block->ddesc->length == count * object->blocksize);
		wbr_touch_block_read(info, slot);
		bdesc_ensure_linked_page(block, page);
		if(!block->ddesc->synthetic)
			return block;
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			wbr_shrink_dblocks(object, CLIP);
		if(info->blocks >= info->soft_blocks)
			wbr_shrink_blocks(info);
	}
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, read_block, number, count, page);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else if(!wbr_push_block(info, block, number))
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static bdesc_t * wbr_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct cache_info * info = (struct cache_info *) object;
	struct rand_slot * slot;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	slot = (struct rand_slot *) hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		/* in the cache, use it */
		assert(slot->block->ddesc->length == count * object->blocksize);
		wbr_touch_block_read(info, slot);
		bdesc_ensure_linked_page(slot->block, page);
		return slot->block;
	}
	
	if(info->dblocks > info->soft_dblocks)
		wbr_shrink_dblocks(object, CLIP);
	if(info->blocks >= info->soft_blocks)
		wbr_shrink_blocks(info);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, synthetic_read_block, number, count, page);
	if(!block)
		return NULL;
	
	if(!wbr_push_block(info, block, number))
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static int wbr_cache_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object;
	struct rand_slot * slot;
	
	/* make sure it's a valid block */
	assert(block->ddesc->length && number + block->ddesc->length / object->blocksize <= object->numblocks);
	
	slot = (struct rand_slot *) hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		/* already have this block */
		assert(slot->block->ddesc == block->ddesc);
		wbr_touch_block_read(info, slot);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		if(slot->index == -1)
			wbr_push_slot_dirty(info, slot);
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			wbr_shrink_dblocks(object, CLIP);
#ifdef __KERNEL__
		else
			/* shrink_dblocks() calls revision_tail_process_landing_requests(),
			 * so only call it if we aren't calling shrink_dblocks() above */
			revision_tail_process_landing_requests();
#endif
		if(info->blocks >= info->soft_blocks)
			wbr_shrink_blocks(info);
		
		slot = wbr_push_block(info, block, number);
		if(!slot)
			return -ENOMEM;
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		wbr_push_slot_dirty(info, slot);
	}
	
	return 0;
}

static int wbr_cache_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
	struct cache_info * info = (struct cache_info *) object;
	uint32_t start_dirty = info->dblocks;

	if(!start_dirty)
		return FLUSH_EMPTY;

	for(;;)
	{
		uint32_t old_dirty = info->dblocks;
		wbr_shrink_dblocks(object, FLUSH);
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

static patch_t ** wbr_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t wbr_cache_bd_get_block_space(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return info->soft_dblocks - info->dblocks;
}

static void wbr_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	wbr_shrink_dblocks(object, PREEN);
#if DEBUG_TIMING
	struct cache_info * info = (struct cache_info *) object;
	printf("%s(): dirty %d/%d, limit %d/%d\n", __FUNCTION__, info->dblocks, info->blocks, info->soft_dblocks, info->soft_blocks);
#endif
}

static int wbr_cache_bd_destroy(BD_t * bd)
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
	
	sched_unregister(wbr_cache_bd_callback, bd);
	
	/* the blocks are all clean, because we checked above - just release them */
	while(info->first)
		wbr_pop_slot(info, info->first);
	
	n_wbr_instances--;
	if(!n_wbr_instances)
		rand_slot_free_all();
	
	vector_destroy(info->dirty_list);
	hash_map_destroy(info->block_map);
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	TIMING_DUMP(wait, "wbr_cache wait", "waits");
	
	return 0;
}

BD_t * wbr_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks)
{
	struct cache_info * info;
	BD_t * bd;
	
	if(soft_dblocks > soft_blocks)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	info->block_map = hash_map_create();
	if(!info->block_map)
	{
		free(info);
		return NULL;
	}
	
	info->dirty_list = vector_create();
	if(!info->dirty_list)
	{
		hash_map_destroy(info->block_map);
		free(info);
		return NULL;
	}
	
	BD_INIT(bd, wbr_cache_bd);
	
	info->bd = disk;
	info->soft_blocks = soft_blocks;
	info->blocks = 0;
	info->soft_dblocks_low = soft_dblocks * 9 / 10;
	info->soft_dblocks_high = soft_dblocks * 11 / 10;
	info->soft_dblocks = info->soft_dblocks_high;
	info->dblocks = 0;
	info->first = NULL;
	info->last = NULL;
	bd->numblocks = disk->numblocks;
	bd->blocksize = disk->blocksize;
	bd->atomicsize = disk->atomicsize;
	info->dirty_state = 1;
	
	/* we generally delay blocks, so our level goes up */
	bd->level = disk->level + 1;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
	/* set up the callback */
	if(sched_register(wbr_cache_bd_callback, bd, FLUSH_PERIOD) < 0)
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
	
	n_wbr_instances++;
	
	FSTITCH_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_NOTIFY, bd);
	return bd;
}
