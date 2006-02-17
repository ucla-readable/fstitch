#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/types.h>
#include <lib/jiffies.h> // HZ
#include <lib/hash_map.h>
#include <lib/panic.h>
#include <lib/kdprintf.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/revision.h>
#include <kfs/wb_cache_bd.h>

/* try to flush every 10 seconds */
#define FLUSH_PERIOD (10 * HZ)

/* This file implements the first whack at our new WB cache. It's an LRU cache,
 * and it allows you to give it chdescs with unsatisfied dependencies. However,
 * it will fill up and deadlock if you give it too many. If this ever causes a
 * problem, an appropriate error message is displayed on the console and -E_BUSY
 * is returned. */

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
	BD_t * bd;
	uint32_t size;
	struct cache_slot * blocks;
	hash_map_t * block_map;
	uint16_t blocksize;
	uint16_t level;
};

static int wb_cache_bd_get_config(void * object, int level, char * string, size_t length)
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

static int wb_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	snprintf(string, length, "dirty: %d", wb_cache_dirty_count((BD_t *) object));
	return 0;
}

static uint32_t wb_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t wb_cache_bd_get_blocksize(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t wb_cache_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static uint32_t push_block(struct cache_info * info, bdesc_t * block)
{
	uint32_t index = info->blocks[0].free_index;
	
	assert(index && index <= info->size && !info->blocks[index].block);
	assert(!hash_map_find_val(info->block_map, (void *) block->number));
	
	if(hash_map_insert(info->block_map, (void *) block->number, (void *) index) < 0)
		return INVALID_BLOCK;
	
	info->blocks[index].block = block;
	
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

static void pop_block(struct cache_info * info, uint32_t number, uint32_t index)
{
	assert(info->blocks[index].block);
	assert(info->blocks[index].block->number == number);
	
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

static void touch_block(struct cache_info * info, uint32_t index)
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

static int flush_block(BD_t * object, struct cache_slot * slot)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	revision_slice_t * slice;
	chmetadesc_t * meta;
	int r;
	
	/* already flushed? */
	if(!slot->block->ddesc->changes)
		return FLUSH_EMPTY;
	for(meta = slot->block->ddesc->changes->dependencies; meta; meta = meta->next)
		if(meta->desc->owner == object)
			break;
	if(!meta)
		return FLUSH_EMPTY;
	
	/* honor external dependencies: 1 for the last parameter here */
	slice = revision_slice_create(slot->block, object, info->bd, 1);
	if(!slice)
	{
		kdprintf(STDERR_FILENO, "%s(): OOM and can't flush!\n", __FUNCTION__);
		return FLUSH_NONE;
	}
	
	if(!slice->ready_size)
	{
		/* otherwise we would have caught it above... */
		assert(slice->full_size);
		r = FLUSH_NONE;
	}
	else
	{
		revision_slice_push_down(slice);
		r = CALL(info->bd, write_block, slot->block);
		if(r < 0)
		{
			revision_slice_pull_up(slice);
			r = FLUSH_NONE;
		}
		else
			r = (slice->ready_size == slice->full_size) ? FLUSH_DONE : FLUSH_SOME;
	}
	
	revision_slice_destroy(slice);
	
	return r;
}

/* evict_block should evict exactly one block if it is successful */
static int evict_block(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	
	for(;;)
	{
		int r = FLUSH_EMPTY;
		struct cache_slot * slot;
		for(slot = info->blocks[0].lru; slot != &info->blocks[0]; slot = slot->prev)
		{
			int code = flush_block(object, slot);
			if(0 <= code)
			{
				pop_block(info, slot->block->number, (uint32_t) (slot - &info->blocks[0]));
				return 0;
			}
			r |= code;
		}
		if(r == FLUSH_NONE)
			return -E_BUSY;
	}
}

static bdesc_t * wb_cache_bd_read_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	bdesc_t * block;
	uint32_t index;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
	{
		/* in the cache, use it */
		touch_block(info, index);
		return info->blocks[index].block;
	}
	
	if(hash_map_size(info->block_map) == info->size)
		if(evict_block(object) < 0)
		{
			printf("HOLY MACKEREL! We can't read block %d, because the cache is full!\n", number);
			return NULL;
		}
	assert(hash_map_size(info->block_map) < info->size);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, read_block, number);
	if(!block)
		return NULL;
	
	index = push_block(info, block);
	if(index == INVALID_BLOCK)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static bdesc_t * wb_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	bdesc_t * block;
	uint32_t index;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
	{
		/* in the cache, use it */
		touch_block(info, index);
		*synthetic = 0;
		return info->blocks[index].block;
	}
	
	if(hash_map_size(info->block_map) == info->size)
		if(evict_block(object) < 0)
		{
			printf("HOLY MACKEREL! We can't synthetic read block %d, because the cache is full!\n", number);
			return NULL;
		}
	assert(hash_map_size(info->block_map) < info->size);
	
	/* not in the cache, need to read it */
	block = CALL(info->bd, synthetic_read_block, number, synthetic);
	if(!block)
		return NULL;
	
	index = push_block(info, block);
	if(index == INVALID_BLOCK)
	{
		/* kind of a waste of the read... but we have to do it */
		if(*synthetic)
			CALL(info->bd, cancel_block, number);
		return NULL;
	}
	
	return block;
}

static int wb_cache_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	uint32_t index;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) number);
	if(index)
		pop_block(info, number, index);
	
	return CALL(info->bd, cancel_block, number);
}

static int wb_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	uint32_t index;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	index = (uint32_t) hash_map_find_val(info->block_map, (void *) block->number);
	if(index)
	{
		/* already have this block */
		assert(info->blocks[index].block->ddesc == block->ddesc);
		touch_block(info, index);
		
		return 0;
	}
	else
	{
		if(hash_map_size(info->block_map) == info->size)
			if(evict_block(object) < 0)
			{
				printf("HOLY MACKEREL! We can't write a block, because the cache is full!\n");
				return -E_BUSY;
			}
		assert(hash_map_size(info->block_map) < info->size);
		
		index = push_block(info, block);
		if(index == INVALID_BLOCK)
			return -E_NO_MEM;
		
		return 0;
	}
}

static int wb_cache_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	int dirty, start_dirty = wb_cache_dirty_count(object);

	if(!start_dirty)
		return FLUSH_EMPTY;

	/* evict_block will evict exactly one block if it is successful */
	for(dirty = start_dirty; dirty; dirty--)
		if(evict_block(object) < 0)
		{
			assert(dirty == wb_cache_dirty_count(object));
			return (start_dirty == dirty) ? FLUSH_NONE : FLUSH_SOME;
		}
	
	assert(!wb_cache_dirty_count(object));
	
	return FLUSH_DONE;
}

static uint16_t wb_cache_bd_get_devlevel(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->level;
}

static void wb_cache_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct cache_slot * slot;
	
	/* FIXME: make this more efficient by only doing dirty blocks? */
	/* FIXME: try to come up with a good flush ordering, instead of waiting for the next callback? */
	for(slot = info->blocks[0].lru; slot != &info->blocks[0]; slot = slot->prev)
		flush_block(object, slot);
}

static int wb_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	uint32_t block;
	int r;
	
	if(wb_cache_dirty_count(bd) != 0)
	{
		r = CALL(bd, flush, FLUSH_DEVICE, NULL);
		if(r < 0)
			return -E_BUSY;
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
	
	free(info->blocks);
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks)
{
	uint32_t i;
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
	
	/* allocate an extra cache slot: hash maps return NULL on failure, so we
	 * can't have 0 be a valid index... besides, we need a pointers to the
	 * head and tail of the LRU block queue */
	info->blocks = malloc((blocks + 1) * sizeof(*info->blocks));
	if(!info->blocks)
	{
		free(info);
		free(bd);
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
		free(info->blocks);
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, wb_cache_bd, info);
	OBJMAGIC(bd) = WB_CACHE_MAGIC;
	
	info->bd = disk;
	info->size = blocks;
	info->blocksize = CALL(disk, get_blocksize);
	
	/* we generally delay blocks, so our level goes up */
	info->level = CALL(disk, get_devlevel) + 1;
	
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
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	uint32_t i, dirty = 0;
	
	if(OBJMAGIC(bd) != WB_CACHE_MAGIC)
		return INVALID_BLOCK;
	
	for(i = 1; i <= info->size; i++)
		if(info->blocks[i].block && info->blocks[i].block->ddesc->changes)
		{
			chmetadesc_t * meta = info->blocks[i].block->ddesc->changes->dependencies;
			while(meta)
			{
				if(meta->desc->owner == bd)
					break;
				meta = meta->next;
			}
			if(meta)
			{
				dirty++;
				continue;
			}
		}
	
	return dirty;
}
