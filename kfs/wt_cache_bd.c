#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/wt_cache_bd.h>

struct cache_info {
	BD_t * bd;
	uint32_t size;
	bdesc_t ** blocks;
	uint16_t blocksize;
	uint16_t level;
};

static int wt_cache_bd_get_config(void * object, int level, char * string, size_t length)
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

static int wt_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t wt_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t wt_cache_bd_get_blocksize(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t wt_cache_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct cache_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static bdesc_t * wt_cache_bd_read_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
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
	
	if(!info->blocks[index])
		return NULL;
	
	/* increase reference count */
	return bdesc_retain(info->blocks[index]);
}

static int wt_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	uint32_t index;
	int value;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	bdesc_retain(block);
	
	index = block->number % info->size;
	/* need to replace this cache entry? */
	if(info->blocks[index])
		bdesc_release(&info->blocks[index]);
	info->blocks[index] = block;
	
	/* this should never fail */
	value = chdesc_push_down(object, block, info->bd, block);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd, write_block, block);
}

static int wt_cache_bd_sync(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(object);
	
	/* since this is a write-through cache, syncing is a no-op */
	/* ...but we still have to pass the sync on correctly */
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	/* sync it */
	return CALL(info->bd, sync, block);
}

static uint16_t wt_cache_bd_get_devlevel(BD_t * object)
{
	return ((struct cache_info *) OBJLOCAL(object))->level;
}

static int wt_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) OBJLOCAL(bd);
	uint32_t block;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
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
	OBJLOCAL(bd) = info;
	
	info->blocks = malloc(blocks * sizeof(*info->blocks));
	if(!info->blocks)
	{
		free(info);
		free(bd);
		return NULL;
	}
	memset(info->blocks, 0, blocks * sizeof(*info->blocks));
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, wt_cache_bd, get_config);
	OBJASSIGN(bd, wt_cache_bd, get_status);
	ASSIGN(bd, wt_cache_bd, get_numblocks);
	ASSIGN(bd, wt_cache_bd, get_blocksize);
	ASSIGN(bd, wt_cache_bd, get_atomicsize);
	ASSIGN(bd, wt_cache_bd, get_devlevel);
	ASSIGN(bd, wt_cache_bd, read_block);
	ASSIGN(bd, wt_cache_bd, write_block);
	ASSIGN(bd, wt_cache_bd, sync);
	DESTRUCTOR(bd, wt_cache_bd, destroy);
	
	info->bd = disk;
	info->size = blocks;
	info->blocksize = CALL(disk, get_blocksize);
	
	info->level = CALL(disk, get_devlevel);

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
