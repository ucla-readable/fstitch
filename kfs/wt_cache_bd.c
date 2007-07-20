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
	BD_t bd;
	
	BD_t *below_bd;
	uint32_t size;
	struct cache_slot * blocks; // blocks[0] holds BD mru and lru
	hash_map_t * block_map; // block_number -> struct cache_slot *
};

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

static int wt_push_block(struct cache_info * info, bdesc_t * block, uint32_t number)
{
	struct cache_slot * slot = info->blocks[0].lru;
	int r;
	
	assert(block);
	assert(!hash_map_find_val(info->block_map, (void *) number));
	assert(!slot->block);
	
	if((r = hash_map_insert(info->block_map, (void *) number, slot)) < 0)
		return r;
	slot->block = block;
	block->cache_number = number;
	
	bdesc_retain(block);
	wt_touch_block(info, slot);
	return 0;
}

static void wt_pop_block(struct cache_info * info, struct cache_slot * slot)
{
	struct cache_slot * erased_slot;
	
	assert(slot);
	assert(slot->block);
	
	erased_slot = hash_map_erase(info->block_map, (void *) slot->block->cache_number);
	assert(slot == erased_slot);
	bdesc_release(&slot->block);
	
	wt_list_remove(slot);
	wt_list_insert(slot, &info->blocks[0]);
}

static bdesc_t * wt_cache_bd_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct cache_info * info = (struct cache_info *) object;
	struct cache_slot * slot;
	bdesc_t * block;
	
	slot = hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		assert(slot->block->ddesc->length == nbytes);
		wt_touch_block(info, slot);
		if(!slot->block->ddesc->synthetic)
			return slot->block;
	}
	else
	{
		/* make sure it's a valid block */
		assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
		
		if(info->blocks[0].lru->block)
			wt_pop_block(info, info->blocks[0].lru);
	}
	
	block = CALL(info->below_bd, read_block, number, nbytes);
	if(!block)
		return NULL;
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else if(wt_push_block(info, block, number) < 0)
		return NULL;
	
	return block;
}

static bdesc_t * wt_cache_bd_synthetic_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct cache_info * info = (struct cache_info *) object;
	struct cache_slot * slot;
	bdesc_t * block;
	int r;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	slot = hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		assert(slot->block->ddesc->length == nbytes);
		wt_touch_block(info, slot);
		return slot->block;
	}
	
	if(info->blocks[0].lru->block)
		wt_pop_block(info, info->blocks[0].lru);
	
	block = CALL(info->below_bd, synthetic_read_block, number, nbytes);
	if(!block)
		return NULL;
	
	r = wt_push_block(info, block, number);
	if(r < 0)
		/* kind of a waste of a read... but we have to do it */
		return NULL;
	
	return block;
}

static int wt_cache_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object;
	struct cache_slot * slot;
	int r;
	
	/* make sure it's a valid block */
	assert(number + block->ddesc->length / object->blocksize <= object->numblocks);
	
	slot = hash_map_find_val(info->block_map, (void *) number);
	if(slot)
	{
		assert(slot->block->ddesc == block->ddesc);
		wt_touch_block(info, slot);
	}
	else
	{
		if(info->blocks[0].lru->block)
			wt_pop_block(info, info->blocks[0].lru);

		r = wt_push_block(info, block, number);
		if(r < 0)
			return r;
	}
	
	/* write it */
	return CALL(info->below_bd, write_block, block, number);
}

static int wt_cache_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** wt_cache_bd_get_write_head(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return CALL(info->below_bd, get_write_head);
}

static int32_t wt_cache_bd_get_block_space(BD_t * object)
{
	struct cache_info * info = (struct cache_info *) object;
	return CALL(info->below_bd, get_block_space);
}

static int wt_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->below_bd, bd);
	
	while(info->blocks[0].mru->block)
		wt_pop_block(info, info->blocks[0].mru);
	
	sfree(info->blocks, (info->size + 1) * sizeof(info->blocks[0]));
	
	hash_map_destroy(info->block_map);
	
	free_memset(info, sizeof(*info));
	free(info);
	
	return 0;
}

BD_t * wt_cache_bd(BD_t * disk, uint32_t blocks)
{
	struct cache_info * info = malloc(sizeof(*info));
	BD_t * bd;
	uint32_t i;

	if(!info)
		return NULL;
	bd = &info->bd;
	
	info->blocks = smalloc((blocks + 1) * sizeof(info->blocks[0]));
	if(!info->blocks)
	{
		free(info);
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

	BD_INIT(bd, wt_cache_bd);
	
	info->below_bd = disk;
	info->size = blocks;
	bd->blocksize = disk->blocksize;
	bd->numblocks = disk->numblocks;
	bd->atomicsize = disk->atomicsize;
	
	bd->level = disk->level;

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
