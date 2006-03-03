#include <stdio.h>
#include <inc/error.h>
#include <lib/types.h>
#include <stdlib.h>
#include <string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/partition_bd.h>

struct partition_info {
	BD_t * bd;
	uint32_t start;
	uint32_t length;
	uint16_t blocksize;
	uint16_t level;
};

static int partition_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct partition_info * info = (struct partition_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "start: %d, length: %d, blocksize: %d", info->start, info->length, info->blocksize);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "[%d:%d]", info->start, info->length);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "start: %d, length: %d", info->start, info->length);
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

static uint32_t partition_bd_get_numblocks(BD_t * object)
{
	return ((struct partition_info *) OBJLOCAL(object))->length;
}

static uint16_t partition_bd_get_blocksize(BD_t * object)
{
	return ((struct partition_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t partition_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct partition_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static bdesc_t * partition_bd_read_block(BD_t * object, uint32_t number)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	bdesc = CALL(info->bd, read_block, info->start + number);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_clone(bdesc, number);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static bdesc_t * partition_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * bdesc, * new_bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	bdesc = CALL(info->bd, synthetic_read_block, info->start + number, synthetic);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_clone(bdesc, number);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static int partition_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return -E_INVAL;
	
	return CALL(info->bd, cancel_block, number + info->start);
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	bdesc_t * wblock;
	int value;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;

	wblock = bdesc_alloc_clone(block, block->number + info->start);
	if(!wblock)
		return -E_UNSPECIFIED;
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

static uint16_t partition_bd_get_devlevel(BD_t * object)
{
	return ((struct partition_info *) OBJLOCAL(object))->level;
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
	info->length = length;
	info->blocksize = CALL(disk, get_blocksize);
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
