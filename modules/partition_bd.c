#include <lib/platform.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/modman.h>
#include <fscore/patch.h>

#include <modules/partition_bd.h>

struct partition_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint32_t start;
};

static bdesc_t * partition_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct partition_info * info = (struct partition_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, read_block, info->start + number, count, page);
}

static bdesc_t * partition_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct partition_info * info = (struct partition_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, synthetic_read_block, info->start + number, count, page);
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct partition_info * info = (struct partition_info *) object;
	int value;
	
	/* make sure it's a valid block */
	assert(block->length && number + block->length / object->blocksize <= object->numblocks);

	/* this should never fail */
	value = patch_push_down(block, object, info->bd);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd, write_block, block, number + info->start);
}

static int partition_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
	return FLUSH_EMPTY;
}

static patch_t ** partition_bd_get_write_head(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t partition_bd_get_block_space(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) object;
	return CALL(info->bd, get_block_space);
}

static int partition_bd_destroy(BD_t * bd)
{
	struct partition_info *info = (struct partition_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

BD_t * partition_bd(BD_t * disk, uint32_t start, uint32_t length)
{
	struct partition_info * info;
	BD_t * bd;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	BD_INIT(bd, partition_bd);
	
	info->bd = disk;
	info->start = start;
	bd->blocksize = disk->blocksize;
	bd->numblocks = length;
	bd->atomicsize = disk->atomicsize;
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
