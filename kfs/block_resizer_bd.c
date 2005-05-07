#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
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
	
	return bdesc;
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
		/* synthesize a new bdesc to avoid having to read it */
		bdesc_t * sub = bdesc_alloc(number + i, info->original_size);
		/* maybe we ran out of memory? */
		if(!sub)
			return -E_NO_MEM;
		bdesc_autorelease(sub);
		memcpy(sub->ddesc->data, &block->ddesc->data[i * info->original_size], info->original_size);
		/* explicitly forward change descriptors */
		depman_translate_chdesc(block, sub, i * info->original_size, info->original_size);
		CALL(info->bd, write_block, sub);
	}
	
	return 0;
}

static int block_resizer_bd_sync(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	uint32_t i, number;
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->converted_size)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->block_count)
		return -E_INVAL;
	
	number = block->number * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		/* synthesize a new bdesc to avoid having to read it */
		bdesc_t * sub = bdesc_alloc(number + i, info->original_size);
		/* maybe we ran out of memory? */
		if(!sub)
			return -E_NO_MEM;
		bdesc_autorelease(sub);
		/* FIXME check return value? */
		CALL(info->bd, sync, sub);
	}
	
	return 0;
}

static int block_resizer_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct resize_info *) OBJLOCAL(bd))->bd, bd);
	
	free(OBJLOCAL(bd));
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
	OBJLOCAL(bd) = info;
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, block_resizer_bd, get_config);
	OBJASSIGN(bd, block_resizer_bd, get_status);
	ASSIGN(bd, block_resizer_bd, get_numblocks);
	ASSIGN(bd, block_resizer_bd, get_blocksize);
	ASSIGN(bd, block_resizer_bd, get_atomicsize);
	ASSIGN(bd, block_resizer_bd, read_block);
	ASSIGN(bd, block_resizer_bd, write_block);
	ASSIGN(bd, block_resizer_bd, sync);
	DESTRUCTOR(bd, block_resizer_bd, destroy);
	
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
	if(modman_inc_bd(disk, bd, NULL) < 0)
	{
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
