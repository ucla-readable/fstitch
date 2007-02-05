#include <lib/error.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/md_bd.h>

struct md_info {
	BD_t * bd[2];
	uint32_t numblocks;
	uint16_t blocksize, atomicsize;
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
	if (length >= 1)
		string[0] = 0;
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

static bdesc_t * md_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * read_bdesc, * bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > info->numblocks)
		return NULL;
	
	read_bdesc = CALL(info->bd[number & 1], read_block, number >> 1, count);
	if(!read_bdesc)
		return NULL;
	
	bdesc = bdesc_alloc_clone(read_bdesc, number);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);

	return bdesc;
}

static bdesc_t * md_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * read_bdesc, * bdesc;
	
	/* make sure it's a valid block */
	if(!count || number + count > info->numblocks)
		return NULL;
	
	read_bdesc = CALL(info->bd[number & 1], synthetic_read_block, number >> 1, count);
	if(!read_bdesc)
		return NULL;
	
	bdesc = bdesc_alloc_clone(read_bdesc, number);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);

	return bdesc;
}

static int md_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	bdesc_t * wblock;
	int value;
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->numblocks)
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

static int md_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t * md_bd_get_write_head(BD_t * object)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	chdesc_t * result = NULL;
	chdesc_t * head[2];
	head[0] = CALL(info->bd[0], get_write_head);
	head[1] = CALL(info->bd[1], get_write_head);
	chdesc_create_noop_array(NULL, NULL, &result, 2, head);
	return result;
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
	uint16_t level0 = disk0->level;
	uint16_t level1 = disk1->level;
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
		bd->level = level0;
	else
		bd->level = level1;

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
