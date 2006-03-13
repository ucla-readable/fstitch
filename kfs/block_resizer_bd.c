#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/block_resizer_bd.h>

#if !defined(__KERNEL__)
#include <assert.h>
#else
#warning Add assert.h support
#define assert(x) do { } while(0)
#endif

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
	if (length >= 1)
		string[0] = 0;
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

static bdesc_t * block_resizer_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > info->block_count)
		return NULL;
	
	bdesc = CALL(info->bd, read_block, number * info->merge_count, count * info->merge_count);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_wrap(bdesc->ddesc, number, bdesc->ddesc->length / info->converted_size);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static bdesc_t * block_resizer_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, bool * synthetic)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > info->block_count)
		return NULL;
	
	bdesc = CALL(info->bd, synthetic_read_block, number * info->merge_count, count * info->merge_count, synthetic);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_wrap(bdesc->ddesc, number, bdesc->ddesc->length / info->converted_size);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static int block_resizer_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->block_count)
		return -E_INVAL;
	
	return CALL(info->bd, cancel_block, number * info->merge_count);
}

static int block_resizer_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * wblock;
	int value;
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->block_count)
		return -E_INVAL;
	
	wblock = bdesc_alloc_wrap(block->ddesc, block->number * info->merge_count, block->ddesc->length / info->original_size);
	if(!wblock)
		return -E_UNSPECIFIED;
	bdesc_autorelease(wblock);
	
	/* this should never fail */
	value = chdesc_push_down(object, block, info->bd, wblock);
	if(value < 0)
		return value;
	
	/* write it */
	value = CALL(info->bd, write_block, wblock);
	return value;
}

static int block_resizer_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
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
