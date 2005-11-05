#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <inc/error.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/blockman.h>
#include <kfs/block_resizer_bd.h>

/* This simple size converter can only convert up in size (i.e. aggregate blocks
 * together on read, split them on write). It should not be too ineffieicent, so
 * long as there is a cache above it. */

struct resize_info {
	BD_t * bd;
	uint16_t original_size;
	uint16_t converted_size;
	uint16_t merge_count;
	uint16_t atomic_size;
	uint32_t block_count;
	/* preallocate this array... */
	partial_forward_t * forward_buffer;
	blockman_t * blockman;
	uint16_t level;
};

static int block_resizer_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct resize_info * info = (struct resize_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "original: %d, converted: %d, count: %d, atomic: %d", info->original_size, info->converted_size, info->block_count, info->atomic_size);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d to %d", info->original_size, info->converted_size);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "original: %d, converted: %d, count: %d", info->original_size, info->converted_size, info->block_count);
	}
	return 0;
}

static int block_resizer_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t block_resizer_bd_get_numblocks(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->block_count;
}

static uint16_t block_resizer_bd_get_blocksize(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->converted_size;
}

static uint16_t block_resizer_bd_get_atomicsize(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->atomic_size;
}

static bdesc_t * block_resizer_bd_read_block(BD_t * object, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t i;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
		return bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->block_count)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->converted_size);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	number *= info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		bdesc_t * sub = CALL(info->bd, read_block, number + i);
		if(!sub)
			return NULL;
		memcpy(&bdesc->ddesc->data[i * info->original_size], sub->ddesc->data, info->original_size);
	}
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return bdesc;
}

/* note that because we are combining multiple results, we can't
 * easily just pass multiple synthetic reads down below... */
static bdesc_t * block_resizer_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(number >= info->block_count)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->converted_size);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int block_resizer_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int block_resizer_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	uint32_t i, number;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->converted_size)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->block_count)
		return -E_INVAL;
	
	number = block->number * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		info->forward_buffer[i].target = info->bd;
		info->forward_buffer[i].number = number + i;
		info->forward_buffer[i].offset = i * info->original_size;
		info->forward_buffer[i].size = info->original_size;
	}
	
	return barrier_partial_forward(info->forward_buffer, info->merge_count, object, block);
}

static int block_resizer_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	uint32_t i, number;
	
	if(block == SYNC_FULL_DEVICE)
		return CALL(info->bd, sync, SYNC_FULL_DEVICE, NULL);
	
	/* make sure it's a valid block */
	if(block >= info->block_count)
		return -E_INVAL;
	
	number = block * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
		CALL(info->bd, sync, i, ch);
	
	return 0;
}

static uint16_t block_resizer_bd_get_devlevel(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->level;
}

static int block_resizer_bd_destroy(BD_t * bd)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct resize_info *) OBJLOCAL(bd))->bd, bd);
	
	free(info->forward_buffer);
	blockman_destroy(&info->blockman);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * block_resizer_bd(BD_t * disk, uint16_t blocksize)
{
	struct resize_info * info;
	uint32_t original_size;
	BD_t * bd;
	
	original_size = CALL(disk, get_blocksize);
	/* make sure it's an even multiple of the block size */
	if(blocksize % original_size)
		return NULL;
	/* block resizer not needed */
	if(blocksize == original_size)
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
	
	BD_INIT(bd, block_resizer_bd, info);
	
	info->bd = disk;
	info->original_size = original_size;
	info->converted_size = blocksize;
	info->merge_count = blocksize / original_size;
	info->atomic_size = CALL(disk, get_atomicsize);
	info->block_count = CALL(disk, get_numblocks) / info->merge_count;
	info->level = CALL(disk, get_devlevel);
	
	info->forward_buffer = malloc(info->merge_count * sizeof(*info->forward_buffer));
	if(!info->forward_buffer)
	{
		free(info);
		free(bd);
		return NULL;
	}

	info->blockman = blockman_create();
	if(!info->blockman)
	{
		free(info->forward_buffer);
		free(info);
		free(bd);
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
