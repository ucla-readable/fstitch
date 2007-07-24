#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/block_resizer_bd.h>

/* This simple size converter can only convert up in size (i.e. aggregate blocks
 * together on read, split them on write). It should not be too ineffieicent, so
 * long as there is a cache above it. */

struct resize_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint16_t original_size;
	uint16_t merge_count;
};

static bdesc_t * block_resizer_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct resize_info * info = (struct resize_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, read_block, number * info->merge_count, count * info->merge_count);
}

static bdesc_t * block_resizer_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct resize_info * info = (struct resize_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, synthetic_read_block, number * info->merge_count, count * info->merge_count);
}

static int block_resizer_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) object;
	int value;
	
	/* make sure it's a valid block */
	assert(block->length && number + block->length / object->blocksize <= object->numblocks);
	
	/* this should never fail */
	value = chdesc_push_down(block, object, info->bd);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd, write_block, block, number * info->merge_count);
}

static int block_resizer_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** block_resizer_bd_get_write_head(BD_t * object)
{
	struct resize_info * info = (struct resize_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t block_resizer_bd_get_block_space(BD_t * object)
{
	struct resize_info * info = (struct resize_info *) object;
	return CALL(info->bd, get_block_space) / info->merge_count;
}

static int block_resizer_bd_destroy(BD_t * bd)
{
	struct resize_info * info = (struct resize_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

BD_t * block_resizer_bd(BD_t * disk, uint16_t blocksize)
{
	struct resize_info * info;
	uint32_t original_size;
	BD_t * bd;
	
	original_size = disk->blocksize;
	/* make sure it's an even multiple of the block size */
	if(blocksize % original_size)
		return NULL;
	/* block resizer not needed */
	if(blocksize == original_size)
		return NULL;
	
	info = malloc(sizeof(struct resize_info));
	if(!info)
		return NULL;

	bd = &info->my_bd;
	BD_INIT(bd, block_resizer_bd);
	
	info->bd = disk;
	info->original_size = original_size;
	bd->blocksize = blocksize;
	info->merge_count = blocksize / original_size;
	bd->atomicsize = disk->atomicsize;
	bd->numblocks = disk->numblocks / info->merge_count;
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
