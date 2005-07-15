#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/md_bd.h>

struct md_info {
	BD_t * bd[2];
	uint32_t numblocks;
	uint16_t blocksize, atomicsize, level;
};

static int md_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct md_info * info = (struct md_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "disks: 2, count: %d, blocksize: %d", info->numblocks, info->blocksize);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "disks: 2");
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "disks: 2, count: %d", info->numblocks);
	}
	return 0;
}

static int md_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t md_bd_get_numblocks(BD_t * object)
{
	return ((struct md_info *) OBJLOCAL(object))->numblocks;
}

static uint16_t md_bd_get_blocksize(BD_t * object)
{
	return ((struct md_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t md_bd_get_atomicsize(BD_t * object)
{
	return ((struct md_info *) OBJLOCAL(object))->atomicsize;
}

static bdesc_t * md_bd_read_block(BD_t * object, uint32_t number)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * read_bdesc, * bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	read_bdesc = CALL(info->bd[number & 1], read_block, number >> 1);
	if(!read_bdesc)
		return NULL;
	
	bdesc = bdesc_alloc_clone(read_bdesc, number);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);

	return bdesc;
}

static bdesc_t * md_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * read_bdesc, * bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	read_bdesc = CALL(info->bd[number & 1], synthetic_read_block, number >> 1, synthetic);
	if(!read_bdesc)
		return NULL;
	
	bdesc = bdesc_alloc_clone(read_bdesc, number);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);

	return bdesc;
}

static int md_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return -E_INVAL;
	
	return CALL(info->bd[number & 1], cancel_block, number >> 1);
}

static int md_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * wblock;
	int value;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
		return -E_INVAL;
	
	wblock = bdesc_alloc_clone(block, block->number >> 1);
	if(!wblock)
		return -E_UNSPECIFIED;
	bdesc_autorelease(wblock);
	
	/* this should never fail */
	value = chdesc_push_down(object, block, info->bd[block->number & 1], wblock);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd[block->number & 1], write_block, wblock);
}

static int md_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	int value;
	
	if(block == SYNC_FULL_DEVICE)
	{
		int r = CALL(info->bd[0], sync, SYNC_FULL_DEVICE, NULL);
		if(r < 0)
		{
			/* for reliability, do bd[1] anyway */
			CALL(info->bd[1], sync, SYNC_FULL_DEVICE, NULL);
			return r;
		}
		return CALL(info->bd[1], sync, SYNC_FULL_DEVICE, NULL);
	}
	
	/* make sure it's a valid block */
	if(block >= info->numblocks)
		return -E_INVAL;
	
	/* sync it */
	value = CALL(info->bd[block & 1], sync, block, ch);
	
	return value;
}

static uint16_t md_bd_get_devlevel(BD_t * object)
{
	return ((struct md_info *) OBJLOCAL(object))->level;
}

static int md_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct md_info *) OBJLOCAL(bd))->bd[1], bd);
	modman_dec_bd(((struct md_info *) OBJLOCAL(bd))->bd[0], bd);
	free(OBJLOCAL(bd));
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
	uint16_t level0 = CALL(disk0, get_devlevel);
	uint16_t level1 = CALL(disk1, get_devlevel);
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
	
	BD_INIT(bd, md_bd, info);
	
	info->bd[0] = disk0;
	info->bd[1] = disk1;
	/* we can use minimum number of blocks and atomic size safely */
	info->numblocks = 2 * MIN(numblocks0, numblocks1);
	info->blocksize = blocksize;
	info->atomicsize = MIN(atomicsize0, atomicsize1);
	
	if (level0 > level1)
		info->level = level0;
	else
		info->level = level1;

	if(modman_add_anon_bd(bd, __FUNCTION__))
		goto error_add;
	if(modman_inc_bd(disk0, bd, "Disk 0") < 0)
		goto error_inc_1;
	if(modman_inc_bd(disk1, bd, "Disk 1") < 0)
		goto error_inc_2;
	
	return bd;
	
error_inc_2:
	modman_dec_bd(disk0, bd);
error_inc_1:
	modman_rem_bd(bd);
error_add:
	DESTROY(bd);
	return NULL;
}
