#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/depman.h>
#include <kfs/modman.h>
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
};

static uint32_t block_resizer_bd_get_numblocks(BD_t * object)
{
	return ((struct resize_info *) object->instance)->block_count;
}

static uint16_t block_resizer_bd_get_blocksize(BD_t * object)
{
	return ((struct resize_info *) object->instance)->converted_size;
}

static uint16_t block_resizer_bd_get_atomicsize(BD_t * object)
{
	return ((struct resize_info *) object->instance)->atomic_size;
}

static bdesc_t * block_resizer_bd_read_block(BD_t * object, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) object->instance;
	bdesc_t * bdesc;
	uint32_t i;
	
	/* make sure it's a valid block */
	if(number >= info->block_count)
		return NULL;
	
	bdesc = bdesc_alloc(object, number, 0, info->converted_size);
	if(!bdesc)
		return NULL;
	
	number *= info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		bdesc_t * sub = CALL(info->bd, read_block, number + i);
		if(!sub)
		{
			bdesc_drop(&bdesc);
			return NULL;
		}
		memcpy(&bdesc->ddesc->data[i * info->original_size], sub->ddesc->data, info->original_size);
		bdesc_drop(&sub);
	}
	
	return bdesc;
}

static int block_resizer_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) object->instance;
	uint32_t i, number;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->converted_size)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->block_count)
		return -E_INVAL;
	
	number = block->number * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		/* Why do we read here, instead of just synthesizing a bdesc to
		 * write? Two reasons:
		 * 1) To avoid confusion, bdescs that claim to originate from a
		 *    BD should have been generated by it.
		 * 2) In case there is an intelligent cache below us, with
		 *    outstanding chdescs for this block, this will ease the
		 *    addition of new chdescs.
		 * It may be worth changing this in the future to be more
		 * efficient. So long as there are caches above and below us, we
		 * should be able to avoid most of the inefficiency. */
		bdesc_t * sub = CALL(info->bd, read_block, number + i);
		/* maybe we ran out of memory? */
		if(!sub)
			return -E_NO_MEM;
		bdesc_touch(sub);
		memcpy(sub->ddesc->data, &block->ddesc->data[i * info->original_size], info->original_size);
		/* explicitly forward change descriptors */
		depman_translate_chdesc(block, sub, i * info->original_size, info->original_size);
		CALL(info->bd, write_block, sub);
	}
	
	bdesc_drop(&block);
	
	return 0;
}

static int block_resizer_bd_sync(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) object->instance;
	uint32_t i, number;
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->converted_size)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->block_count)
		return -E_INVAL;
	
	number = block->number * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		bdesc_t * sub = CALL(info->bd, read_block, number + i);
		/* ran out of memory? */
		if(!sub)
			return -E_INVAL;
		/* FIXME check return value? */
		CALL(info->bd, sync, sub);
	}
	
	bdesc_drop(&block);
	
	return 0;
}

static int block_resizer_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct resize_info *) bd->instance)->bd);
	
	free(bd->instance);
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
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	bd->instance = info;
	
	ASSIGN(bd, block_resizer_bd, get_numblocks);
	ASSIGN(bd, block_resizer_bd, get_blocksize);
	ASSIGN(bd, block_resizer_bd, get_atomicsize);
	ASSIGN(bd, block_resizer_bd, read_block);
	ASSIGN(bd, block_resizer_bd, write_block);
	ASSIGN(bd, block_resizer_bd, sync);
	ASSIGN_DESTROY(bd, block_resizer_bd, destroy);
	
	info->bd = disk;
	info->original_size = original_size;
	info->converted_size = blocksize;
	info->merge_count = blocksize / original_size;
	info->atomic_size = CALL(disk, get_atomicsize);
	info->block_count = CALL(disk, get_numblocks) / info->merge_count;
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	modman_inc_bd(disk);
	
	return bd;
}
