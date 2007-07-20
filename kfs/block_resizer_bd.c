#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/block_resizer_bd.h>

/* This simple size converter can only convert up in size (i.e. aggregate blocks
 * together on read, split them on write). It should not be too ineffieicent, so
 * long as there is a cache above it. */

struct resize_info {
	BD_t bd;
	
	BD_t * below_bd;
	uint16_t original_size;
	uint16_t merge_count;
};

#if 0
static int block_resizer_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct resize_info * info = (struct resize_info *) object;
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "original: %d, converted: %d, count: %d, atomic: %d", info->original_size, bd->blocksize, bd->numblocks, bd->atomicsize);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d to %d", info->original_size, bd->blocksize);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "original: %d, converted: %d, count: %d", info->original_size, bd->blocksize, bd->numblocks);
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
#endif

static bdesc_t * block_resizer_bd_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct resize_info * info = (struct resize_info *) object;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	return CALL(info->below_bd, read_block, number * info->merge_count, nbytes);
}

static bdesc_t * block_resizer_bd_synthetic_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct resize_info * info = (struct resize_info *) object;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	return CALL(info->below_bd, synthetic_read_block, number * info->merge_count, nbytes);
}

static int block_resizer_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) object;
	
	/* make sure it's a valid block */
	assert(number + block->length / object->blocksize <= object->numblocks);
	
	/* write it */
	return CALL(info->below_bd, write_block, block, number * info->merge_count);
}

static int block_resizer_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** block_resizer_bd_get_write_head(BD_t * object)
{
	struct resize_info * info = (struct resize_info *) object;
	return CALL(info->below_bd, get_write_head);
}

static int32_t block_resizer_bd_get_block_space(BD_t * object)
{
	struct resize_info * info = (struct resize_info *) object;
	return CALL(info->below_bd, get_block_space) / info->merge_count;
}

static int block_resizer_bd_destroy(BD_t * bd)
{
	struct resize_info * info = (struct resize_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->below_bd, bd);
	
	free_memset(info, sizeof(*info));
	free(info);
	
	return 0;
}

BD_t * block_resizer_bd(BD_t * disk, uint16_t blocksize)
{
	struct resize_info * info;
	uint32_t original_size;
	BD_t *bd;
	
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

	bd = &info->bd;
	BD_INIT(bd, block_resizer_bd);
	
	info->below_bd = disk;
	info->original_size = original_size;
	bd->blocksize = blocksize;
	info->merge_count = blocksize / original_size;
	bd->atomicsize = disk->atomicsize;
	bd->numblocks = disk->numblocks / info->merge_count;
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
