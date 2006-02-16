#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <inc/error.h>
#include <lib/jiffies.h>
#include <lib/panic.h>
#include <lib/stdio.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/revision.h>
#include <kfs/elevator_cache_bd.h>

#define ELEV_DEBUG 0

#if ELEV_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* incremental flush every second */
#define FLUSH_PERIOD HZ

/* This file is similar to wb_cache_bd.c, but it tries to evict blocks in
 * "elevator" order instead of LRU order. The expected configuration is that a
 * small elevator_cache will be placed under a larger wb_cache (and before a
 * persistent disk, so that the elevator_cache has level 1) to make "sliding
 * window" optimizations to the LRU write ordering. Note that the elevator_cache
 * will *not* hold onto blocks until all their external dependencies have been
 * satisfied (like the wb_cache does); it only optimizes the local ordering. */

/* Blocks in the cache are kept in a binary tree, so that we can look them up
 * quickly and find nearby blocks easily. */

struct elevator_slot {
	bdesc_t * block;
	struct elevator_slot * parent;
	struct elevator_slot ** pointer; /* parent's pointer to us */
	struct elevator_slot * smaller;
	struct elevator_slot * larger;
};

struct cache_info {
	BD_t * bd;
	uint32_t size, dirty;
	uint32_t head_pos;
	struct elevator_slot * blocks;
	uint16_t blocksize, level;
};

static int elevator_cache_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "blocksize: %d, size: %d, contention: x%d", info->blocksize, info->size, (CALL(info->bd, get_numblocks) + info->size - 1) / info->size);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d x %d", info->blocksize, info->size);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "blocksize: %d, size: %d", info->blocksize, info->size);
	}
	return 0;
}

static int elevator_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	snprintf(string, length, "dirty: %d", info->dirty);
	return 0;
}

static uint32_t elevator_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t elevator_cache_bd_get_blocksize(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t elevator_cache_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static struct elevator_slot * lookup_block_slot(struct cache_info * info, uint32_t number, struct elevator_slot ** parent)
{
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	struct elevator_slot * slot = info->blocks;
	if(parent)
		*parent = NULL;
	while(slot)
	{
		if(slot->block->number == number)
			break;
		if(parent)
			*parent = slot;
		if(slot->block->number < number)
			slot = slot->larger;
		else
			slot = slot->smaller;
	}
	return slot;
}

static bdesc_t * lookup_block_exact(struct cache_info * info, uint32_t number)
{
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	struct elevator_slot * slot = lookup_block_slot(info, number, NULL);
	return slot ? slot->block : NULL;
}

static bdesc_t * lookup_block_larger(struct cache_info * info, uint32_t number)
{
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	struct elevator_slot * parent;
	struct elevator_slot * slot = lookup_block_slot(info, number, &parent);
	if(slot)
		return slot->block;
	
	while(parent && parent->block->number < number)
	{
		number = parent->block->number;
		parent = parent->parent;
	}
	
	return parent ? parent->block : NULL;
}

static int insert_block(struct cache_info * info, bdesc_t * block)
{
	Dprintf("%s(%d)\n", __FUNCTION__, block->number);
	struct elevator_slot * parent = NULL;
	struct elevator_slot ** pointer = &info->blocks;
	struct elevator_slot * slot = info->blocks;
	while(slot)
	{
		if(slot->block->number == block->number)
			return (slot->block->ddesc == block->ddesc) ? 0 : -E_BUSY;
		if(slot->block->number < block->number)
		{
			parent = slot;
			pointer = &slot->larger;
			slot = slot->larger;
		}
		else
		{
			parent = slot;
			pointer = &slot->smaller;
			slot = slot->smaller;
		}
	}
	
	slot = malloc(sizeof(*slot));
	if(!slot)
		return -E_NO_MEM;
	
	slot->block = bdesc_retain(block);
	slot->parent = parent;
	slot->pointer = pointer;
	slot->smaller = NULL;
	slot->larger = NULL;
	*pointer = slot;
	
	return 0;
}

static int remove_slot(struct elevator_slot * slot)
{
	Dprintf("%s(%d)\n", __FUNCTION__, slot->block->number);
	bdesc_release(&slot->block);
	if(!slot->smaller && !slot->larger)
	{
		*slot->pointer = NULL;
		free(slot);
	}
	else if(!slot->smaller || !slot->larger)
	{
		struct elevator_slot * child = slot->smaller ? slot->smaller : slot->larger;
		child->parent = slot->parent;
		child->pointer = slot->pointer;
		*slot->pointer = child;
		free(slot);
	}
	else
	{
		bdesc_t * next_block;
		struct elevator_slot * next = slot->larger;
		while(next->smaller)
			next = next->smaller;
		/* must retain because it will be released in the recursive call */
		next_block = bdesc_retain(next->block);
		remove_slot(next);
		slot->block = next_block;
	}
	return 0;
}

static int remove_block(struct cache_info * info, bdesc_t * block)
{
	Dprintf("%s(%d)\n", __FUNCTION__, block->number);
	struct elevator_slot * slot = lookup_block_slot(info, block->number, NULL);
	if(slot)
	{
		if(slot->block->ddesc != block->ddesc)
			return -E_BUSY;
		remove_slot(slot);
	}
	return 0;
}

static void remove_block_number(struct cache_info * info, uint32_t number)
{
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	struct elevator_slot * slot = lookup_block_slot(info, number, NULL);
	if(slot)
		remove_slot(slot);
}

static bdesc_t * advance_head(struct cache_info * info)
{
	bdesc_t * block = lookup_block_larger(info, info->head_pos);
	if(!block)
		block = lookup_block_larger(info, 0);
	assert(block);
	Dprintf("%s() = %d\n", block->number);
	/* advance the head *past* this block, not to it */
	info->head_pos = block->number + 1;
	return block;
}

static int evict_block(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	Dprintf("%s()\n", __FUNCTION__);
	
	if(!info->dirty)
		return 0;
	
	for(;;)
	{
		bdesc_t * block = advance_head(info);
		revision_slice_t * slice = revision_slice_create(block, object, info->bd, 0);
		if(!slice)
			return -E_NO_MEM;
		if(slice->ready_size)
		{
			int r;
			
			revision_slice_push_down(slice);
			r = CALL(info->bd, write_block, block);
			if(r < 0)
				panic("%s(): write_block failed (%i), but code for recovery is not implemented", __FUNCTION__, r);
			
			if(slice->ready_size == slice->full_size)
			{
				revision_slice_destroy(slice);
				remove_block(info, block);
				info->dirty--;
				break;
			}
		}
		revision_slice_destroy(slice);
	}
	
	return 0;
}

static bdesc_t * elevator_cache_bd_read_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	bdesc_t * block;
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	block = lookup_block_exact(info, number);
	if(block)
		/* in the cache, use it */
		return block;
	
	if(info->dirty == info->size)
		if(evict_block(object) < 0)
		{
			printf("HOLY MACKEREL! We can't read a block, because the cache is full!\n");
			return NULL;
		}
	assert(info->dirty < info->size);
	
	/* not in the cache, need to read it */
	/* notice that we do not reset the head position here, even though
	 * technically the head has been moved - this is for fairness */
	return CALL(info->bd, read_block, number);
}

static bdesc_t * elevator_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	bdesc_t * block;
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	block = lookup_block_exact(info, number);
	if(block)
	{
		/* in the cache, use it */
		*synthetic = 0;
		return block;
	}
	
	if(info->dirty == info->size)
		if(evict_block(object) < 0)
		{
			printf("HOLY MACKEREL! We can't read a block, because the cache is full!\n");
			return NULL;
		}
	assert(info->dirty < info->size);
	
	/* not in the cache, need to read it */
	/* notice that we do not reset the head position here, even though
	 * technically the head may have been moved - this is for fairness */
	return CALL(info->bd, synthetic_read_block, number, synthetic);
}

static int elevator_cache_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	Dprintf("%s(%d)\n", __FUNCTION__, number);
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	remove_block_number(info, number);
	
	return CALL(info->bd, cancel_block, number);
}

static int elevator_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct elevator_slot * slot;
	Dprintf("%s(%d)\n", __FUNCTION__, block->number);
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	slot = lookup_block_slot(info, block->number, NULL);
	if(!slot)
	{
		chmetadesc_t * scan = block->ddesc->changes->dependencies;
		for(; scan; scan = scan->next)
			if(scan->desc->owner == object)
				break;
		if(!scan)
			/* the block is clean... no need to write it */
			return 0;
		
		if(info->dirty == info->size)
		{
			int r = evict_block(object);
			if(r < 0)
				return r;
		}
		assert(info->dirty < info->size);
		
		/* could be more efficient if we used parent pointer from
		 * lookup_block_slot() above... */
		if(insert_block(info, block) < 0)
			return -E_NO_MEM;
		
		info->dirty++;
	}
	return 0;
}

static int elevator_cache_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	int start_dirty = info->dirty;

	if(!info->dirty)
		return FLUSH_EMPTY;

	while(info->dirty)
	{
		int r = evict_block(object);
		/* this really should never happen to the elevator cache... */
		if(r < 0)
			return (info->dirty == start_dirty) ? FLUSH_NONE : FLUSH_SOME;
	}

	return FLUSH_DONE;
}

static uint16_t elevator_cache_bd_get_devlevel(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->level;
}

static void elevator_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	Dprintf("%s()\n", __FUNCTION__);
	int r = evict_block(object);
	if(r < 0)
		panic("%s(): eviction failed! (%i)\n", __FUNCTION__, r);
}

static int elevator_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	int r;
	
	if(info->dirty)
	{
		r = CALL(bd, flush, FLUSH_DEVICE, NULL);
		if(r < 0)
			return -E_BUSY;
	}
	assert(!info->dirty);
	assert(!info->blocks);
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	sched_unregister(elevator_cache_bd_callback, bd);
	
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * elevator_cache_bd(BD_t * disk, uint32_t blocks)
{
	struct cache_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, elevator_cache_bd, info);
	OBJMAGIC(bd) = ELEVATOR_CACHE_MAGIC;
	
	info->bd = disk;
	info->size = blocks;
	info->dirty = 0;
	info->head_pos = 0;
	info->blocks = NULL;
	info->blocksize = CALL(disk, get_blocksize);
	
	/* we generally delay blocks, so our level goes up */
	info->level = CALL(disk, get_devlevel) + 1;
	
	/* set up the callback */
	if(sched_register(elevator_cache_bd_callback, bd, FLUSH_PERIOD) < 0)
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
