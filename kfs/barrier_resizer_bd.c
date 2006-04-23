#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/blockman.h>
#include <kfs/barrier_resizer_bd.h>

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
};

static int barrier_resizer_bd_get_config(void * object, int level, char * string, size_t length)
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

static int barrier_resizer_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static uint32_t barrier_resizer_bd_get_numblocks(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->block_count;
}

static uint16_t barrier_resizer_bd_get_blocksize(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->converted_size;
}

static uint16_t barrier_resizer_bd_get_atomicsize(BD_t * object)
{
	return ((struct resize_info *) OBJLOCAL(object))->atomic_size;
}

static bdesc_t * barrier_resizer_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t i;
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->block_count)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->converted_size, count);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	number *= info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		bdesc_t * sub = CALL(info->bd, read_block, number + i, 1);
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
static bdesc_t * barrier_resizer_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, bool * synthetic)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->block_count)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->converted_size, count);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int barrier_resizer_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int barrier_resizer_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct resize_info * info = (struct resize_info *) OBJLOCAL(object);
	uint32_t i, number;
	int value;
	
	/* FIXME: make this module support counts other than 1 */
	assert(block->count == 1);
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->block_count)
		return -E_INVAL;
	
	number = block->number * info->merge_count;
	for(i = 0; i != info->merge_count; i++)
	{
		info->forward_buffer[i].target = info->bd;
		info->forward_buffer[i].number = number + i;
		info->forward_buffer[i].offset = i * info->original_size;
		info->forward_buffer[i].size = info->original_size;
	}
	
	/* our level needs to look higher than where we want to send the chdescs, so that
	* while we're working with the micro-cache built into the partial forwarder, we
	* will appear to be at a level higher than the block device below us */
	object->level++;
	value = barrier_partial_forward(info->forward_buffer, info->merge_count, object, block);
	object->level--;
	return value;
}

static int barrier_resizer_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static int barrier_resizer_bd_destroy(BD_t * bd)
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

BD_t * barrier_resizer_bd(BD_t * disk, uint16_t blocksize)
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
	
	BD_INIT(bd, barrier_resizer_bd, info);
	
	info->bd = disk;
	info->original_size = original_size;
	info->converted_size = blocksize;
	info->merge_count = blocksize / original_size;
	info->atomic_size = CALL(disk, get_atomicsize);
	info->block_count = CALL(disk, get_numblocks) / info->merge_count;
	bd->level = disk->level;
	
	info->forward_buffer = malloc(info->merge_count * sizeof(*info->forward_buffer));
	if(!info->forward_buffer)
	{
		free(info);
		free(bd);
		return NULL;
	}

	info->blockman = blockman_create(blocksize);
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
