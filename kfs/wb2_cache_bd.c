#include <lib/error.h>
#include <lib/assert.h>
#include <lib/types.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/revision.h>
#include <kfs/wb2_cache_bd.h>

/* try to flush every second */
#define FLUSH_PERIOD HZ

#define DEBUG_TIMING 0
#include <kfs/kernel_timing.h>
KERNEL_TIMING(wait);

/* each block in the cache has an lru_slot, and the dirty blocks are
 * hooked up using the dirty links in addition to the all links */
struct lru_slot {
	bdesc_t * block;
	struct {
		struct lru_slot * prev;
		struct lru_slot * next;
	} all, dirty;
};

/* the all list is ordered by read/write usage, while the dirty list is ordered by write usage:
 * all.first -> most recently used -> next -> next -> least recently used <- all.last
 * dirty.first -> most recently written -> next -> next -> least recently written <- dirty.last */
struct cache_info {
	BD_t * bd;
	uint32_t soft_blocks, blocks;
	uint32_t soft_dblocks, dblocks;
	struct {
		struct lru_slot * first;
		struct lru_slot * last;
	} all, dirty;
	/* map from (void *) number -> lru_slot * */
	hash_map_t * block_map;
	uint16_t blocksize;
};

static int wb2_cache_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "blocksize: %d, soft dirty: %d, soft blocks: %d", info->blocksize, info->soft_dblocks, info->soft_blocks);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d x %d", info->blocksize, info->soft_blocks);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "blocksize: %d, soft blocks: %d", info->blocksize, info->soft_blocks);
	}
	return 0;
}

static int wb2_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "dirty: %d, blocks: %d", info->dblocks, info->blocks);
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "%d", info->blocks);
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "blocks: %d",  info->blocks);
	}
	return 0;
}

static uint32_t wb2_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t wb2_cache_bd_get_blocksize(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t wb2_cache_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

/* we are guaranteed that the block is not already in the list */
static struct lru_slot * push_block(struct cache_info * info, bdesc_t * block)
{
	struct lru_slot * slot = malloc(sizeof(*slot));
	if(!slot)
		return NULL;
	
	slot->block = block;
	slot->all.prev = NULL;
	slot->all.next = info->all.first;
	slot->dirty.prev = NULL;
	slot->dirty.next = NULL;
	
	assert(!hash_map_find_val(info->block_map, (void *) block->number));
	if(hash_map_insert(info->block_map, (void *) block->number, slot) < 0)
	{
		free(slot);
		return NULL;
	}
	
	info->all.first = slot;
	if(slot->all.next)
		slot->all.next->all.prev = slot;
	else
		info->all.last = slot;
	info->blocks++;
	
	bdesc_retain(block);
	
	return slot;
}

/* we are guaranteed that the block is not already in the list */
static void push_slot_dirty(struct cache_info * info, struct lru_slot * slot)
{
	slot->dirty.prev = NULL;
	slot->dirty.next = info->dirty.first;
	
	info->dirty.first = slot;
	if(slot->dirty.next)
		slot->dirty.next->dirty.prev = slot;
	else
		info->dirty.last = slot;
	info->dblocks++;
}

#define dirty_slot(info, slot) ((info)->dirty.first == (slot) || (slot)->dirty.prev)

static void pop_slot(struct cache_info * info, struct lru_slot * slot)
{
	uint32_t number = slot->block->number;
	assert(hash_map_find_val(info->block_map, (void *) number) == slot);
	
	bdesc_release(&slot->block);
	
	if(slot->all.prev)
		slot->all.prev->all.next = slot->all.next;
	else
		info->all.first = slot->all.next;
	if(slot->all.next)
		slot->all.next->all.prev = slot->all.prev;
	else
		info->all.last = slot->all.prev;
	if(dirty_slot(info, slot))
	{
		if(slot->dirty.prev)
			slot->dirty.prev->dirty.next = slot->dirty.next;
		else
			info->dirty.first = slot->dirty.next;
		if(slot->dirty.next)
			slot->dirty.next->dirty.prev = slot->dirty.prev;
		else
			info->dirty.last = slot->dirty.prev;
	}
	
	hash_map_erase(info->block_map, (void *) number);
}

static void pop_slot_dirty(struct cache_info * info, struct lru_slot * slot)
{
	assert(dirty_slot(info, slot));
	if(slot->dirty.prev)
		slot->dirty.prev->dirty.next = slot->dirty.next;
	else
		info->dirty.first = slot->dirty.next;
	if(slot->dirty.next)
		slot->dirty.next->dirty.prev = slot->dirty.prev;
	else
		info->dirty.last = slot->dirty.prev;
	slot->dirty.prev = NULL;
	slot->dirty.next = NULL;
	info->dblocks--;
}

static void touch_block_read(struct cache_info * info, struct lru_slot * slot)
{
	/* already the first? */
	if(info->all.first == slot)
		return;
	
	/* must have a prev, so detach it */
	slot->all.prev->all.next = slot->all.next;
	if(slot->all.next)
		slot->all.next->all.prev = slot->all.prev;
	else
		info->all.last = slot->all.prev;
	
	/* now re-add it */
	slot->all.prev = NULL;
	slot->all.next = info->all.first;
	info->all.first = slot;
	if(slot->all.next)
		slot->all.next->all.prev = slot;
	else
		info->all.last = slot;
}

static int flush_block(BD_t * object, bdesc_t * block, int * delay)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	revision_slice_t slice;
	chdesc_t * chdesc;
	int r;
	
	if(delay)
		*delay = 0;
	
	/* in flight? */
	if(block->ddesc->in_flight)
		return FLUSH_NONE;
	
	/* already flushed? */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc->ddesc_next)
		if(chdesc->owner == object)
			break;
	if(!chdesc)
		return FLUSH_EMPTY;
	
	r = revision_slice_create(block, object, info->bd, &slice);
	if(r < 0)
	{
		printk(KERN_ERR "%s() returned %i; can't flush!\n", __FUNCTION__, r);
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
		r = CALL(info->bd, write_block, block);
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
static void shrink_dblocks(BD_t * object, enum dshrink_strategy strategy)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct lru_slot * slot = info->dirty.last;
	
	revision_tail_process_landing_requests();
	
	/* in clip mode, stop as soon as we are below the soft limit */
	while((info->dblocks > info->soft_dblocks || strategy != CLIP) && slot)
	{
		int delay = 0;
		int status = flush_block(object, slot->block, &delay);
		/* still dirty? */
		if(status < 0)
			slot = slot->dirty.prev;
		else
		{
			uint32_t number = slot->block->number;
			struct lru_slot * prev = slot->dirty.prev;
			pop_slot_dirty(info, slot);
			/* now try and find sequential blocks to write */
			while((slot = hash_map_find_val(info->block_map, (void *) ++number)))
			{
				if(!dirty_slot(info, slot))
					break;
				/* if we were about to examine this block, don't */
				if(slot == prev)
					prev = prev->dirty.prev;
				/* assume it will be merged, so don't ask for delay */
				status = flush_block(object, slot->block, NULL);
				/* clean slot now? */
				if(status >= 0)
					pop_slot_dirty(info, slot);
				/* if we didn't actually write it, stop looking */
				if(status == FLUSH_EMPTY || status == FLUSH_NONE)
					break;
			}
			slot = prev;
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
static void shrink_blocks(struct cache_info * info)
{
	struct lru_slot * slot = info->all.last;
	/* while there are more blocks than the soft limit, and there are clean blocks */
	while(info->blocks >= info->soft_blocks && info->blocks > info->dblocks)
	{
		assert(slot);
		/* skip dirty blocks */
		if(dirty_slot(info, slot))
			slot = slot->all.prev;
		else
		{
			struct lru_slot * prev = slot->all.prev;
			pop_slot(info, slot);
			info->blocks--;
			slot = prev;
		}
	}
}

static bdesc_t * wb2_cache_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct lru_slot * slot;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	if(!count || number + count > CALL(info->bd, get_numblocks))
		return NULL;
	
	slot = (struct lru_slot *) hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		/* in the cache, use it */
		block = slot->block;
		assert(block->count == count);
		touch_block_read(info, slot);
		if(!block->ddesc->synthetic)
			return block;
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			shrink_dblocks(object, CLIP);
		if(info->blocks >= info->soft_blocks)
			shrink_blocks(info);
	}
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, read_block, number, count);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else if(!push_block(info, block))
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static bdesc_t * wb2_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct lru_slot * slot;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	if(!count || number + count > CALL(info->bd, get_numblocks))
		return NULL;
	
	slot = (struct lru_slot *) hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		/* in the cache, use it */
		assert(slot->block->count == count);
		touch_block_read(info, slot);
		return slot->block;
	}
	
	if(info->dblocks > info->soft_dblocks)
		shrink_dblocks(object, CLIP);
	if(info->blocks >= info->soft_blocks)
		shrink_blocks(info);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, synthetic_read_block, number, count);
	if(!block)
		return NULL;
	
	if(!push_block(info, block))
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static int wb2_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct lru_slot * slot;
	
	/* make sure it's a valid block */
	if(block->number + block->count > CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	slot = (struct lru_slot *) hash_map_find_val(info->block_map, (void *) block->number);
	if(slot)
	{
		/* already have this block */
		assert(slot->block->ddesc == block->ddesc);
		assert(slot->block->count == block->count);
		touch_block_read(info, slot);
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		if(!dirty_slot(info, slot))
			push_slot_dirty(info, slot);
	}
	else
	{
		if(info->dblocks > info->soft_dblocks)
			shrink_dblocks(object, CLIP);
		else
			/* shrink_dblocks() calls revision_tail_process_landing_requests(),
			 * so only call it if we aren't calling shrink_dblocks() above */
			revision_tail_process_landing_requests();
		if(info->blocks >= info->soft_blocks)
			shrink_blocks(info);
		
		slot = push_block(info, block);
		if(!slot)
			return -E_NO_MEM;
		/* assume it's dirty, even if it's not: we'll discover
		 * it later when a revision slice has zero size */
		push_slot_dirty(info, slot);
	}
	
	return 0;
}

static int wb2_cache_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	uint32_t start_dirty = info->dblocks;

	if(!start_dirty)
		return FLUSH_EMPTY;

	for(;;)
	{
		uint32_t old_dirty = info->dblocks;
		shrink_dblocks(object, FLUSH);
		if(!info->dblocks)
			return FLUSH_DONE;
		if(info->dblocks == old_dirty)
		{
			if(revision_tail_flights_exist())
			{
				KERNEL_INTERVAL(wait);
				TIMING_START(wait);
				revision_tail_wait_for_landing_requests();
				revision_tail_process_landing_requests();
				TIMING_STOP(wait, wait);
			}
			else
				return (old_dirty == start_dirty) ? FLUSH_NONE : FLUSH_SOME;
		}
	}
}

static chdesc_t * wb2_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	return CALL(info->bd, get_write_head);
}

static void wb2_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	shrink_dblocks(object, PREEN);
#if DEBUG_TIMING
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	printk(KERN_ERR "%s(): dirty %d/%d, limit %d/%d\n", __FUNCTION__, info->dblocks, info->blocks, info->soft_dblocks, info->soft_blocks);
#endif
}

static int wb2_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	int r;
	
	if(info->dblocks)
	{
		r = CALL(bd, flush, FLUSH_DEVICE, NULL);
		if(r < 0)
			return -E_BUSY;
	}
	assert(!info->dblocks);
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	sched_unregister(wb2_cache_bd_callback, bd);
	
	/* the blocks are all clean, because we checked above - just release them */
	while(info->all.first)
		pop_slot(info, info->all.first);
	
	hash_map_destroy(info->block_map);
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	TIMING_DUMP(wait, "wb2_cache wait", "waits");
	
	return 0;
}

BD_t * wb2_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks)
{
	struct cache_info * info;
	BD_t * bd;
	
	if(soft_dblocks > soft_blocks)
		return NULL;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->block_map = hash_map_create();
	if(!info->block_map)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, wb2_cache_bd, info);
	OBJMAGIC(bd) = WB_CACHE_MAGIC;
	
	info->bd = disk;
	info->soft_blocks = soft_blocks;
	info->blocks = 0;
	info->soft_dblocks = soft_dblocks;
	info->dblocks = 0;
	info->all.first = NULL;
	info->all.last = NULL;
	info->dirty.first = NULL;
	info->dirty.last = NULL;
	info->blocksize = CALL(disk, get_blocksize);
	
	/* we generally delay blocks, so our level goes up */
	bd->level = disk->level + 1;
	
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
	
	return bd;
}
