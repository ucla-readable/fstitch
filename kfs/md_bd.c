#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/md_bd.h>

struct md_info {
	BD_t * bd[2];
	uint32_t numblocks;
	uint16_t blocksize, atomicsize;
};

static uint32_t md_bd_get_numblocks(BD_t * object)
{
	return ((struct md_info *) object->instance)->numblocks;
}

static uint16_t md_bd_get_blocksize(BD_t * object)
{
	return ((struct md_info *) object->instance)->blocksize;
}

static uint16_t md_bd_get_atomicsize(BD_t * object)
{
	return ((struct md_info *) object->instance)->atomicsize;
}

static bdesc_t * md_bd_read_block(BD_t * object, uint32_t number)
{
	struct md_info * info = (struct md_info *) object->instance;
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	bdesc = CALL(info->bd[number & 1], read_block, number >> 1);
	
	if(!bdesc)
		return NULL;
	
	/* ensure we can alter the structure without conflict */
	if(bdesc_alter(&bdesc))
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	
	/* adjust the block descriptor to match the md */
	bdesc->bd = object;
	bdesc->number = number;
	
	return bdesc;
}

static int md_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct md_info * info = (struct md_info *) object->instance;
	uint32_t refs = block->refs, number = block->number;
	int value;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
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
	block->bd = info->bd[number & 1];
	block->number >>= 1;
	
	/* write it */
	value = CALL(block->bd, write_block, block);
	
	if(refs)
	{
		block->bd = object;
		block->number = number;
		block->translated--;
	}
	
	return value;
}

static int md_bd_sync(BD_t * object, bdesc_t * block)
{
	struct md_info * info = (struct md_info *) object->instance;
	uint32_t refs, number;
	int value;
	
	if(!block)
	{
		int r = CALL(info->bd[0], sync, NULL);
		if(r < 0)
		{
			/* for reliability, do bd[1] anyway */
			CALL(info->bd[1], sync, NULL);
			return r;
		}
		return CALL(info->bd[1], sync, NULL);
	}
	
	/* save reference count and number */
	refs = block->refs;
	number = block->number;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
		return -E_INVAL;
	
	block->translated++;
	block->bd = info->bd[number & 1];
	block->number >>= 1;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	if(refs)
	{
		block->bd = object;
		block->number = number;
		block->translated--;
	}
	
	return value;
}

static int md_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct md_info *) bd->instance)->bd[1]);
	modman_dec_bd(((struct md_info *) bd->instance)->bd[0]);
	free(bd->instance);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * md_bd(BD_t * disk0, BD_t * disk1)
{
	struct md_info * info;
	uint32_t numblocks0 = CALL(disk0, get_numblocks);
	uint32_t numblocks1 = CALL(disk1, get_numblocks);
	uint16_t blocksize = CALL(disk0, get_blocksize);
	uint16_t atomicsize0 = CALL(disk0, get_atomicsize);
	uint16_t atomicsize1 = CALL(disk1, get_atomicsize);
	BD_t * bd;
	
	/* block sizes must be the same */
	if(blocksize != CALL(disk1, get_blocksize))
		return NULL;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct md_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	bd->instance = info;
	
	ASSIGN(bd, md_bd, get_numblocks);
	ASSIGN(bd, md_bd, get_blocksize);
	ASSIGN(bd, md_bd, get_atomicsize);
	ASSIGN(bd, md_bd, read_block);
	ASSIGN(bd, md_bd, write_block);
	ASSIGN(bd, md_bd, sync);
	ASSIGN_DESTROY(bd, md_bd, destroy);
	
	info->bd[0] = disk0;
	info->bd[1] = disk1;
	/* we can use minimum number of blocks and atomic size safely */
	info->numblocks = 2 * MIN(numblocks0, numblocks1);
	info->blocksize = blocksize;
	info->atomicsize = MIN(atomicsize0, atomicsize1);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	modman_inc_bd(disk0);
	modman_inc_bd(disk1);
	
	return bd;
}
