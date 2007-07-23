#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/partition_bd.h>

struct partition_info {
	BD_t * bd;
	uint32_t start;
};

static bdesc_t * partition_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > object->numblocks)
		return NULL;
	
	bdesc = CALL(info->bd, read_block, info->start + number, count);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_clone(bdesc, number);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static bdesc_t * partition_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > object->numblocks)
		return NULL;
	
	bdesc = CALL(info->bd, synthetic_read_block, info->start + number, count);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_clone(bdesc, number);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * wblock;
	int value;
	
	/* make sure it's a valid block */
	if(block->number + block->count > object->numblocks)
		return -EINVAL;

	wblock = bdesc_alloc_clone(block, block->number + info->start);
	if(!wblock)
		return -1;
	bdesc_autorelease(wblock);
	
	/* this should never fail */
	value = chdesc_push_down(object, block, info->bd, wblock);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd, write_block, wblock);
}

static int partition_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** partition_bd_get_write_head(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	return CALL(info->bd, get_write_head);
}

static int32_t partition_bd_get_block_space(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	return CALL(info->bd, get_block_space);
}

static int partition_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct partition_info *) OBJLOCAL(bd))->bd, bd);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * partition_bd(BD_t * disk, uint32_t start, uint32_t length)
{
	struct partition_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct partition_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, partition_bd, info);
	
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
