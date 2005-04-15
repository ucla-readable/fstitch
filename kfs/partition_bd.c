#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/partition_bd.h>

struct partition_info {
	BD_t * bd;
	uint32_t start;
	uint32_t length;
	uint16_t blocksize;
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
	snprintf(string, length, "");
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
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	bdesc = CALL(info->bd, read_block, info->start + number);
	
	if(!bdesc)
		return NULL;
	
	/* ensure we can alter the structure without conflict */
	if(bdesc_alter(&bdesc))
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	
	/* adjust the block descriptor to match the partition */
	bdesc->bd = object;
	bdesc->number = number;
	
	return bdesc;
}

static int partition_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	uint32_t refs = block->refs;
	int value;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;
	
	/* Important logic:
	 * If somebody has a reference to this bdesc (i.e. refs > 0), then that
	 * entity must be above us. This is because all entities that have
	 * references to a bdesc think it has the same BD (in this case, us),
	 * and everybody below us uses a different BD. So, if there is a
	 * reference to this bdesc, it will still exist after the call to
	 * write_block below. If not, the principle of caller-drop applies to
	 * the bdesc and we do not have to (nor can we) de-translate it.
	 * However, we must still set the translated flag, as it is the cue to
	 * bdesc_retain() to notify the dependency manager of the change. In the
	 * case where translated = 1 and refs = 0, bdesc_retain simply clears
	 * the translated flag. */
	block->translated++;
	block->bd = info->bd;
	block->number += info->start;
	
	/* write it */
	value = CALL(block->bd, write_block, block);
	
	if(refs)
	{
		block->bd = object;
		block->number -= info->start;
		block->translated--;
	}
	
	return value;
}

static int partition_bd_sync(BD_t * object, bdesc_t * block)
{
	struct partition_info * info = (struct partition_info *) OBJLOCAL(object);
	uint32_t refs;
	int value;
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* save reference count */
	refs = block->refs;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;
	
	block->translated++;
	block->bd = info->bd;
	block->number += info->start;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	if(refs)
	{
		block->bd = object;
		block->number -= info->start;
		block->translated--;
	}
	
	return value;
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
	OBJLOCAL(bd) = info;
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, partition_bd, get_config);
	OBJASSIGN(bd, partition_bd, get_status);
	ASSIGN(bd, partition_bd, get_numblocks);
	ASSIGN(bd, partition_bd, get_blocksize);
	ASSIGN(bd, partition_bd, get_atomicsize);
	ASSIGN(bd, partition_bd, read_block);
	ASSIGN(bd, partition_bd, write_block);
	ASSIGN(bd, partition_bd, sync);
	DESTRUCTOR(bd, partition_bd, destroy);
	
	info->bd = disk;
	info->start = start;
	info->length = length;
	info->blocksize = CALL(disk, get_blocksize);
	
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
