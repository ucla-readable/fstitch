#include <inc/lib.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/partition_bd.h>

struct partition_info {
	BD_t * bd;
	uint32_t start;
	uint32_t length;
};

static uint32_t partition_bd_get_numblocks(BD_t * object)
{
	return ((struct partition_info *) object->instance)->length;
}

static uint32_t partition_bd_get_blocksize(BD_t * object)
{
	return CALL(((struct partition_info *) object->instance)->bd, get_blocksize);
}

static bdesc_t * partition_bd_read_block(BD_t * object, uint32_t number)
{
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct partition_info *) object->instance)->length)
		return NULL;
	
	bdesc = CALL(((struct partition_info *) object->instance)->bd, read_block, ((struct partition_info *) object->instance)->start + number);
	
	/* FIXME bdesc_alter() can fail */
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&bdesc);
	
	/* adjust the block descriptor to match the partition */
	bdesc->bd = object;
	bdesc->number = number;
	
	return bdesc;
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block)
{
	int value;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct partition_info *) object->instance)->length)
		return -1;
	
	block->translated++;
	block->bd = ((struct partition_info *) object->instance)->bd;
	block->number -= ((struct partition_info *) object->instance)->start;
	
	/* write it */
	value = CALL(block->bd, write_block, block);
	
	block->bd = object;
	block->number += ((struct partition_info *) object->instance)->start;
	block->translated--;
	
	return value;
}

static int partition_bd_sync(BD_t * object, bdesc_t * block)
{
	int value;
	
	if(!block)
		return CALL(((struct partition_info *) object->instance)->bd, sync, NULL);
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct partition_info *) object->instance)->length)
		return -1;
	
	block->translated++;
	block->bd = ((struct partition_info *) object->instance)->bd;
	block->number -= ((struct partition_info *) object->instance)->start;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	block->bd = object;
	block->number += ((struct partition_info *) object->instance)->start;
	block->translated--;
	
	return value;
}

static int partition_bd_destroy(BD_t * bd)
{
	free(bd->instance);
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
	bd->instance = info;
	
	ASSIGN(bd, partition_bd, get_numblocks);
	ASSIGN(bd, partition_bd, get_blocksize);
	ASSIGN(bd, partition_bd, read_block);
	ASSIGN(bd, partition_bd, write_block);
	ASSIGN(bd, partition_bd, sync);
	ASSIGN_DESTROY(bd, partition_bd, destroy);
	
	info->bd = disk;
	info->start = start;
	info->length = length;
	
	return bd;
}
