#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/md_bd.h>

struct md_info {
	BD_t my_bd;
	
	BD_t * bd[2];
};

static bdesc_t * md_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct md_info * info = (struct md_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd[number & 1], read_block, number >> 1, count);
}

static bdesc_t * md_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct md_info * info = (struct md_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd[number & 1], synthetic_read_block, number >> 1, count);
}

static int md_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct md_info * info = (struct md_info *) object;
	int value;
	
	/* make sure it's a valid block */
	assert(number + block->ddesc->length / object->blocksize <= object->numblocks);
	
	/* this should never fail */
	value = chdesc_push_down(block, object, info->bd[number & 1]);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd[number & 1], write_block, block, number >> 1);
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
	struct md_info * info = (struct md_info *) object;
	int32_t result[2];
	result[0] = CALL(info->bd[0], get_block_space);
	result[1] = CALL(info->bd[1], get_block_space);
	return (result[0] > result[1]) ? result[1] : result[0];
}

static int md_bd_destroy(BD_t * bd)
{
	struct md_info *info = (struct md_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd[1], bd);
	modman_dec_bd(info->bd[0], bd);
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

BD_t * md_bd(BD_t * disk0, BD_t * disk1)
{
	struct md_info * info;
	uint32_t numblocks0 = disk0->numblocks;
	uint32_t numblocks1 = disk1->numblocks;
	uint16_t blocksize = disk0->blocksize;
	uint16_t atomicsize0 = disk0->atomicsize;
	uint16_t atomicsize1 = disk1->atomicsize;
	BD_t * bd;
	
	/* block sizes must be the same */
	if(blocksize != disk1->blocksize)
		return NULL;
	
	/* no write heads allowed */
	if(CALL(disk0, get_write_head) || CALL(disk1, get_write_head))
		return NULL;
	
	info = malloc(sizeof(struct md_info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	BD_INIT(bd, md_bd);
	
	info->bd[0] = disk0;
	info->bd[1] = disk1;
	/* we can use minimum number of blocks and atomic size safely */
	bd->numblocks = 2 * MIN(numblocks0, numblocks1);
	bd->blocksize = blocksize;
	bd->atomicsize = MIN(atomicsize0, atomicsize1);
	
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
