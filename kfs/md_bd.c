#include <lib/platform.h>

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
		return -EINVAL;
	
	wblock = bdesc_alloc_clone(block, block->number >> 1);
	if(!wblock)
		return -1;
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

static chdesc_t ** md_bd_get_write_head(BD_t * object)
{
	return NULL;
}

static int32_t md_bd_get_block_space(BD_t * object)
{
	struct md_info * info = (struct md_info *) OBJLOCAL(object);
	int32_t result[2];
	result[0] = CALL(info->bd[0], get_block_space);
	result[1] = CALL(info->bd[1], get_block_space);
	return (result[0] > result[1]) ? result[1] : result[0];
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
	BD_t * bd;
	
	/* block sizes must be the same */
	if(blocksize != CALL(disk1, get_blocksize))
		return NULL;
	
	/* no write heads allowed */
	if(CALL(disk0, get_write_head) || CALL(disk1, get_write_head))
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
	
	if (disk0->level > disk1->level)
		bd->level = disk0->level;
	else
		bd->level = disk1->level;
	if (disk0->graph_index > disk1->graph_index)
		bd->graph_index = disk0->graph_index + 1;
	else
		bd->graph_index = disk1->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
		goto error_add;

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
