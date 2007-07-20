#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>
#include <lib/pool.h>

#include <kfs/kfsd.h>
#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/debug.h>
#include <kfs/revision.h>
#include <kfs/wb2_cache_bd.h>

/* try to flush every second */
#define FLUSH_PERIOD HZ

#define DEBUG_TIMING 0
#include <kfs/kernel_timing.h>
KERNEL_TIMING(wait);

/* useful for looking at chdesc graphs */
#define DELAY_FLUSH_UNTIL_EXIT 0

#define MAP_SIZE 32768

/* the all list is ordered by read/write usage, while the dirty list is ordered by write usage:
 * all.first -> most recently used -> next -> next -> least recently used <- all.last
 * dirty.first -> most recently written -> next -> next -> least recently written <- dirty.last */
typedef struct wb2_cache_bd {
	struct BD bd;
	
	BD_t *below_bd;
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
} wb2_cache_bd_t;

static inline bdesc_t * wb2_map_get_block(wb2_cache_bd_t * info, uint32_t number)
{
	bdesc_t * b = info->map[number & (info->map_capacity - 1)];
	while(b && b->b_number < number)
		b = b->block_hash.next;
	return (b && b->b_number == number) ? b : NULL;
}

static inline void wb2_map_put_block(wb2_cache_bd_t * info, bdesc_t * block)
{
	bdesc_t ** b = &info->map[block->b_number & (info->map_capacity - 1)];
	while(*b && (*b)->b_number < block->b_number)
		b = &(*b)->block_hash.next;
	block->block_hash.next = *b;
	if(*b)
		(*b)->block_hash.pprev = &block->block_hash.next;
	block->block_hash.pprev = b;
	*b = block;
}

static inline void wb2_map_remove_block(bdesc_t * block)
{
	if((*block->block_hash.pprev = block->block_hash.next))
		block->block_hash.next->block_hash.pprev = block->block_hash.pprev;
}

/* we are guaranteed that the block is not already in the list */
static void wb2_push_block(wb2_cache_bd_t * info, bdesc_t * block)
{
	block->lru_all.prev = NULL;
	block->lru_all.next = info->all.first;
	block->lru_dirty.prev = NULL;
	block->lru_dirty.next = NULL;
	
	assert(!wb2_map_get_block(info, block->b_number));
	wb2_map_put_block(info, block);
	
	info->all.first = block;
	if(block->lru_all.next)
		block->lru_all.next->lru_all.prev = block;
	else
		info->all.last = block;
	info->blocks++;
	
	bdesc_retain(block);
}

/* we are guaranteed that the block is not already in the list */
static void wb2_push_dirty(wb2_cache_bd_t * info, bdesc_t * block)
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

static void wb2_pop_slot(wb2_cache_bd_t * info, bdesc_t * block)
{
	assert(wb2_map_get_block(info, block->b_number) == block);
	
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

static void wb2_pop_slot_dirty(wb2_cache_bd_t * info, bdesc_t * block)
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

static void wb2_touch_block_read(wb2_cache_bd_t * info, bdesc_t * block)
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
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	revision_slice_t slice;
	int r;
	KFS_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_LOOKBLOCK, object, block);
	
	if(delay)
		*delay = 0;
	
	/* in flight? */
	if(block->ddesc->in_flight)
		return FLUSH_NONE;
	
	/* already flushed? */
	if(!block->ddesc->level_changes[object->level].head)
		return FLUSH_EMPTY;
	
	r = revision_slice_create(block, object, info->below_bd, &slice);
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
		r = CALL(info->below_bd, write_block, block, block->b_number);
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
			KFS_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_WRITEBLOCK, object, block, block->ddesc->flags);
		}
	}
	
	revision_slice_destroy(&slice);
	
	return r;
}

enum dshrink_strategy {
	CLIP,  /* just get below the soft limit */
	FLUSH, /* flush as much as possible */
	PREEN  /* flush but stop on any I/O delay */
};
/* reduce the number of dirty blocks in the cache, if possible, by writing
 * blocks out (using the specified strategy) */
static void wb2_shrink_dblocks(BD_t * object, enum dshrink_strategy strategy)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	bdesc_t * block = info->dirty.last;
	
#if DELAY_FLUSH_UNTIL_EXIT
	if(kfsd_is_running())
		return;
#endif
	
#ifdef __KERNEL__
	revision_tail_process_landing_requests();
#endif
	KFS_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_FINDBLOCK, object);
	
	/* in clip mode, stop as soon as we are below the soft limit */
	while((info->dblocks > info->soft_dblocks || strategy != CLIP) && block != NULL)
	{
		int status, delay = 0;
		status = wb2_flush_block(object, block, &delay);
		/* still dirty? */
		if(status < 0)
		{
			block = block->lru_dirty.prev;
		}
		else
		{
			uint32_t number = block->b_number;
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
static void wb2_shrink_blocks(wb2_cache_bd_t * info)
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

static bdesc_t * wb2_cache_bd_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	block = wb2_map_get_block(info, number);
	if(block)
	{
		/* in the cache, use it */
		assert(block->ddesc->length == nbytes);
		wb2_touch_block_read(info, block);
		if(!block->ddesc->synthetic)
			return block;
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			wb2_shrink_dblocks(object, CLIP);
		if(info->blocks >= info->soft_blocks)
			wb2_shrink_blocks(info);
	}
	
	/* not in the cache, need to read it */
	block = CALL(info->below_bd, read_block, number, nbytes);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else
		wb2_push_block(info, block);
	
	return block;
}

static bdesc_t * wb2_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	block = wb2_map_get_block(info, number);
	if(block)
	{
		/* in the cache, use it */
		assert(block->ddesc->length == nbytes);
		wb2_touch_block_read(info, block);
		return block;
	}
	
	if(info->dblocks > info->soft_dblocks)
		wb2_shrink_dblocks(object, CLIP);
	if(info->blocks >= info->soft_blocks)
		wb2_shrink_blocks(info);
	
	/* not in the cache, need to read it */
	block = CALL(info->below_bd, synthetic_read_block, number, nbytes);
	if(!block)
		return NULL;
	
	wb2_push_block(info, block);
	return block;
}

static int wb2_cache_bd_write_block(BD_t * object, bdesc_t *block, uint32_t number)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	
	/* make sure it's a valid block */
	assert(number + block->ddesc->length / object->blocksize <= object->numblocks);
	
	block = wb2_map_get_block(info, number);
	if(block)
	{
		/* already have this block */
		wb2_touch_block_read(info, block);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		if(!wb2_dirty_slot(info, block))
			wb2_push_dirty(info, block);
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
		
		wb2_push_block(info, block);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		wb2_push_dirty(info, block);
	}
	
	return 0;
}

static int wb2_cache_bd_flush(BD_t * object, uint32_t blockno, chdesc_t * ch)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
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

static chdesc_t ** wb2_cache_bd_get_write_head(BD_t * object)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	return CALL(info->below_bd, get_write_head);
}

static int32_t wb2_cache_bd_get_block_space(BD_t * object)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	return info->soft_dblocks - info->dblocks;
}

static void wb2_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	wb2_shrink_dblocks(object, PREEN);
#if DEBUG_TIMING
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) object;
	printf("%s(): dirty %d/%d, limit %d/%d\n", __FUNCTION__, info->dblocks, info->blocks, info->soft_dblocks, info->soft_blocks);
#endif
}

static int wb2_cache_bd_destroy(BD_t * bd)
{
	wb2_cache_bd_t * info = (wb2_cache_bd_t *) bd;
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
	modman_dec_bd(info->below_bd, bd);
	
	sched_unregister(wb2_cache_bd_callback, bd);
	
	/* the blocks are all clean, because we checked above - just release them */
	while(info->all.first)
		wb2_pop_slot(info, info->all.first);
	
	free(info->map);
	memset(bd, 0, sizeof(*bd));
	free(info);
	
	TIMING_DUMP(wait, "wb2_cache wait", "waits");
	
	return 0;
}

BD_t * wb2_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks)
{
	wb2_cache_bd_t * info;
	
	if(soft_dblocks > soft_blocks)
		return NULL;
	
	info = malloc(sizeof(wb2_cache_bd_t));
	if(!info)
		return NULL;

	info->map_capacity = MAP_SIZE;
	info->map = (bdesc_t **) malloc(info->map_capacity * sizeof(*info->map));
	if(!info->map)
	{
		free(info);
		return NULL;
	}
	memset(info->map, 0, info->map_capacity * sizeof(*info->map));
	
	BD_INIT(&info->bd, wb2_cache_bd);
	OBJMAGIC(&info->bd) = WB_CACHE_MAGIC;
	
	info->below_bd = disk;
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
	info->bd.numblocks = disk->numblocks;
	info->bd.blocksize = disk->blocksize;
	info->bd.atomicsize = disk->atomicsize;
	
	/* we generally delay blocks, so our level goes up */
	info->bd.level = disk->level + 1;
	
	/* set up the callback */
	if(sched_register(wb2_cache_bd_callback, &info->bd, FLUSH_PERIOD) < 0)
	{
		DESTROY(&info->bd);
		return NULL;
	}
	
	if(modman_add_anon_bd(&info->bd, __FUNCTION__))
	{
		DESTROY(&info->bd);
		return NULL;
	}
	if(modman_inc_bd(disk, &info->bd, NULL) < 0)
	{
		modman_rem_bd(&info->bd);
		DESTROY(&info->bd);
		return NULL;
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CACHE, KDB_CACHE_NOTIFY, &info->bd);
	return &info->bd;
}
