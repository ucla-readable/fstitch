#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/wt_cache_bd.h>

struct cache_info {
	BD_t * bd;
	uint32_t size;
	bdesc_t ** blocks;
};

static uint32_t wt_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) object->instance)->bd, get_numblocks);
}

static uint32_t wt_cache_bd_get_blocksize(BD_t * object)
{
	return CALL(((struct cache_info *) object->instance)->bd, get_blocksize);
}

static bdesc_t * wt_cache_bd_read_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t index;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	index = number % info->size;
	if(info->blocks[index])
	{
		/* in the cache, use it */
		if(info->blocks[index]->number == number)
			return info->blocks[index];
		
		/* need to replace this cache entry */
		bdesc_release(&info->blocks[index]);
	}
	
	/* not in the cache, need to read it */
	info->blocks[index] = CALL(info->bd, read_block, number);
	
	if(!info->blocks)
		return NULL;
	
	/* FIXME bdesc_alter() and bdesc_retain() can fail */
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&info->blocks[index]);
	
	/* adjust the block descriptor to match the cache */
	info->blocks[index]->bd = object;
	
	/* increase reference count */
	bdesc_retain(&info->blocks[index]);
	
	return info->blocks[index];
}

static int wt_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t index;
	int value;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -1;
	
	/* FIXME bdesc_retain() can fail */
	/* increase reference count - do this before release, in case they are the same block */
	bdesc_retain(&block);
	
	index = block->number % info->size;
	/* need to replace this cache entry? */
	if(info->blocks[index])
		bdesc_release(&info->blocks[index]);
	info->blocks[index] = block;
	
	if(block->translated)
		printf("%s(): (%s:%d): block already translated!\n", __FUNCTION__, __FILE__, __LINE__);
	block->translated = 1;
	block->bd = info->bd;
	
	/* write it */
	value = CALL(block->bd, write_block, block);
	
	block->bd = object;
	block->translated = 0;
	
	return value;
}

static int wt_cache_bd_sync(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int value;
	
	/* since this is a write-through cache, syncing is a no-op */
	/* ...but we still have to pass the sync on correctly */
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -1;
	
	block->translated++;
	block->bd = info->bd;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	block->bd = object;
	block->translated--;
	
	return value;
}

static int wt_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd->instance;
	uint32_t block;
	
	for(block = 0; block != info->size; block++)
		if(info->blocks[block])
			bdesc_release(&info->blocks[block]);
	free(info->blocks);
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * wt_cache_bd(BD_t * disk, uint32_t blocks)
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
	bd->instance = info;
	
	info->blocks = malloc(blocks * sizeof(*info->blocks));
	if(!info->blocks)
	{
		free(info);
		free(bd);
		return NULL;
	}
	memset(info->blocks, 0, blocks * sizeof(*info->blocks));
	
	ASSIGN(bd, wt_cache_bd, get_numblocks);
	ASSIGN(bd, wt_cache_bd, get_blocksize);
	ASSIGN(bd, wt_cache_bd, read_block);
	ASSIGN(bd, wt_cache_bd, write_block);
	ASSIGN(bd, wt_cache_bd, sync);
	ASSIGN_DESTROY(bd, wt_cache_bd, destroy);
	
	info->bd = disk;
	info->size = blocks;
	
	return bd;
}
