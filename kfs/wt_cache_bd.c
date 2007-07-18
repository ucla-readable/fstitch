#include <lib/platform.h>
#include <lib/hash_map.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/wt_cache_bd.h>

struct cache_slot {
	bdesc_t * block;
	union {
		struct cache_slot * lru; /* block[0] */
		struct cache_slot * more_recent; /* blocks other than block[0] */
	};
	union {
		struct cache_slot * mru; /* block[0] */
		struct cache_slot * less_recent; /* blocks other than block[0] */
	};
};

struct cache_info {
	BD_t * bd;
	uint32_t size;
	struct cache_slot * blocks; // blocks[0] holds BD mru and lru
	hash_map_t * block_map; // block_number -> struct cache_slot *
};

static int wt_cache_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "blocksize: %d, size: %d, contention: x%d", bd->blocksize, info->size, (bd->numblocks + info->size - 1) / info->size);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d x %d", bd->blocksize, info->size);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "blocksize: %d, size: %d", bd->blocksize, info->size);
	}
	return 0;
}

static int wt_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}

/* remove 'slot' from its list position */
static void wt_list_remove(struct cache_slot * slot)
{
	slot->less_recent->more_recent = slot->more_recent;
	slot->more_recent->less_recent = slot->less_recent;
}

/* insert 'slot' into the list position following 'less_recent' */
static void wt_list_insert(struct cache_slot * slot, struct cache_slot * less_recent)
{
	slot->more_recent = less_recent->more_recent;
	slot->more_recent->less_recent = slot;
	less_recent->more_recent = slot;
	slot->less_recent = less_recent;
}

static void wt_touch_block(struct cache_info * info, struct cache_slot * slot)
{
	if(info->blocks[0].mru != slot)
	{
		wt_list_remove(slot);
		wt_list_insert(slot, info->blocks[0].mru);
	}
}

static int wt_push_block(struct cache_info * info, bdesc_t * block)
{
	struct cache_slot * slot = info->blocks[0].lru;
	int r;
	
	assert(block);
	assert(!hash_map_find_val(info->block_map, (void *) block->number));
	assert(!slot->block);
	
	if((r = hash_map_insert(info->block_map, (void *) block->number, slot)) < 0)
		return r;
	slot->block = block;
	
	bdesc_retain(block);
	wt_touch_block(info, slot);
	return 0;
}

static void wt_pop_block(struct cache_info * info, struct cache_slot * slot)
{
	struct cache_slot * erased_slot;
	
	assert(slot);
	assert(slot->block);
	
	erased_slot = hash_map_erase(info->block_map, (void *) slot->block->number);
	assert(slot == erased_slot);
	bdesc_release(&slot->block);
	
	wt_list_remove(slot);
	wt_list_insert(slot, &info->blocks[0]);
}

static bdesc_t * wt_cache_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct cache_slot * slot;
	bdesc_t * block;
	
	slot = hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		assert(slot->block->count == count);
		wt_touch_block(info, slot);
		if(!slot->block->ddesc->synthetic)
			return slot->block;
	}
	else
	{
		/* make sure it's a valid block */
		if(!count || number + count > info->bd->numblocks)
			return NULL;
		
		if(info->blocks[0].lru->block)
			wt_pop_block(info, info->blocks[0].lru);
	}
	
	block = CALL(info->bd, read_block, number, count);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else if(wt_push_block(info, block) < 0)
		return NULL;
	
	return block;
}

static bdesc_t * wt_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct cache_slot * slot;
	bdesc_t * block;
	int r;
	
	/* make sure it's a valid block */
	if(!count || number + count > info->bd->numblocks)
		return NULL;
	
	slot = hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		assert(slot->block->count == count);
		wt_touch_block(info, slot);
		return slot->block;
	}
	
	if(info->blocks[0].lru->block)
		wt_pop_block(info, info->blocks[0].lru);
	
	block = CALL(info->bd, synthetic_read_block, number, count);
	if(!block)
		return NULL;
	
	r = wt_push_block(info, block);
	if(r < 0)
		/* kind of a waste of a read... but we have to do it */
		return NULL;
	
	return block;
}

static int wt_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	struct cache_slot * slot;
	int r;
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->bd->numblocks)
		return -EINVAL;
	
	slot = hash_map_find_val(info->block_map, (void *) block->number);
	if(slot)
	{
		assert(slot->block->ddesc == block->ddesc);
		assert(slot->block->count == block->count);
		wt_touch_block(info, slot);
	}
	else
	{
		if(info->blocks[0].lru->block)
			wt_pop_block(info, info->blocks[0].lru);

		r = wt_push_block(info, block);
		if(r < 0)
			return r;
	}
	
	/* this should never fail */
	r = chdesc_push_down(object, block, info->bd, block);
	if(r < 0)
		return r;
	
	/* write it */
	return CALL(info->bd, write_block, block);
}

static int wt_cache_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** wt_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	return CALL(info->bd, get_write_head);
}

static int32_t wt_cache_bd_get_block_space(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	return CALL(info->bd, get_block_space);
}

static int wt_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	while(info->blocks[0].mru->block)
		wt_pop_block(info, info->blocks[0].mru);
	
	sfree(info->blocks, (info->size + 1) * sizeof(info->blocks[0]));
	
	hash_map_destroy(info->block_map);
	
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * wt_cache_bd(BD_t * disk, uint32_t blocks)
{
	struct cache_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	uint32_t i;

	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->blocks = smalloc((blocks + 1) * sizeof(info->blocks[0]));
	if(!info->blocks)
	{
		free(info);
		free(bd);
		return NULL;
	}

	info->blocks[0].block = NULL;
	info->blocks[0].lru = &info->blocks[1];
	info->blocks[0].mru = &info->blocks[blocks];
	for(i = 1; i < blocks + 1; i++)
	{
		info->blocks[i].block = NULL;
		info->blocks[i].less_recent = &info->blocks[i - 1];
		info->blocks[i].more_recent = &info->blocks[(i + 1) % (blocks + 1)];
	}

	info->block_map = hash_map_create_size(blocks, 0);

	BD_INIT(bd, wt_cache_bd, info);
	
	info->bd = disk;
	info->size = blocks;
	bd->blocksize = disk->blocksize;
	bd->numblocks = disk->numblocks;
	bd->atomicsize = disk->atomicsize;
	
	bd->level = disk->level;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
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
