#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/partition_bd.h>

struct partition_info {
	BD_t bd;
	
	BD_t * below_bd;
	uint32_t start;
};

#if 0
static int partition_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct partition_info * info = (struct partition_info *) bd;
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "start: %d, length: %d, blocksize: %d", info->start, bd->numblocks, bd->blocksize);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "[%d:%d]", info->start, bd->numblocks);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "start: %d, length: %d", info->start, bd->numblocks);
	}
	return 0;
}

static int partition_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}
#endif

static bdesc_t * partition_bd_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct partition_info * info = (struct partition_info *) object;
	bdesc_t *bdesc;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	bdesc = CALL(info->below_bd, read_block, info->start + number, nbytes);
	if (bdesc)
		bdesc->b_number = number;
	
	return bdesc;
}

static bdesc_t * partition_bd_synthetic_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct partition_info * info = (struct partition_info *) object;
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);
	
	bdesc = CALL(info->below_bd, synthetic_read_block, info->start + number, nbytes);
	if (bdesc)
		bdesc->b_number = number;
	
	return bdesc;
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct partition_info * info = (struct partition_info *) object;
	
	/* make sure it's a valid block */
	assert(number + block->ddesc->length / object->blocksize <= object->numblocks);

	/* write it */
	return CALL(info->below_bd, write_block, block, number + info->start);
}

static int partition_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** partition_bd_get_write_head(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) object;
	return CALL(info->below_bd, get_write_head);
}

static int32_t partition_bd_get_block_space(BD_t * object)
{
	struct partition_info * info = (struct partition_info *) object;
	return CALL(info->below_bd, get_block_space);
}

static int partition_bd_destroy(BD_t * bd)
{
	struct partition_info *info = (struct partition_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->below_bd, bd);
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

BD_t * partition_bd(BD_t * disk, uint32_t start, uint32_t length)
{
	struct partition_info * info;
	BD_t * bd;
	
	info = malloc(sizeof(struct partition_info));
	if(!info)
		return NULL;
	bd = &info->bd;
	
	BD_INIT(bd, partition_bd);
	
	info->below_bd = disk;
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
