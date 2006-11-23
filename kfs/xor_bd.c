#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/blockman.h>
#include <kfs/xor_bd.h>

/* The xor device must be a barrier, because the data is different on each side. */

struct xor_info {
	BD_t * bd;
	uint32_t numblocks, xor_key;
	uint16_t blocksize, atomicsize;
	blockman_t * blockman;
};

static int xor_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct xor_info * info = (struct xor_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "key: 0x%08x, count: %d, blocksize: %d", info->xor_key, info->numblocks, info->blocksize);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "key: 0x%08x", info->xor_key);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "key: 0x%08x, count: %d", info->xor_key, info->numblocks);
	}
	return 0;
}

static int xor_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t xor_bd_get_numblocks(BD_t * object)
{
	return ((struct xor_info *) OBJLOCAL(object))->numblocks;
}

static uint16_t xor_bd_get_blocksize(BD_t * object)
{
	return ((struct xor_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t xor_bd_get_atomicsize(BD_t * object)
{
	return ((struct xor_info *) OBJLOCAL(object))->atomicsize;
}

/* xor the block in place */
static void xor_bd_xor(bdesc_t * block, uint32_t xor_key)
{
	uint32_t * data = (uint32_t *) block->ddesc->data;
	int i, size = block->ddesc->length / sizeof(*data);
	for(i = 0; i != size; i++)
		data[i] ^= xor_key;
}

static bdesc_t * xor_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct xor_info * info = (struct xor_info *) OBJLOCAL(object);
	bdesc_t * block;
	bdesc_t * orig;
	
	block = blockman_managed_lookup(info->blockman, number);
	if(block)
	{
		assert(block->count == count);
		if(!block->ddesc->synthetic)
			return block;
	}
	else
	{
		/* make sure it's a valid block */
		if(!count || number + count > info->numblocks)
			return NULL;

		block = bdesc_alloc(number, info->blocksize, count);
		if(!block)
			return NULL;
		bdesc_autorelease(block);
	}
	
	orig = CALL(info->bd, read_block, number, count);
	if(!orig)
		return NULL;
	
	assert(block->ddesc->length == orig->ddesc->length);
	memcpy(block->ddesc->data, orig->ddesc->data, orig->ddesc->length);
	xor_bd_xor(block, info->xor_key);
	
	if(block->ddesc->synthetic)
		block->ddesc->synthetic = 0;
	else
	{
		if(blockman_managed_add(info->blockman, block) < 0)
			/* kind of a waste of the read... but we have to do it */
			return NULL;
		/* lock the block only if we keep the new block */
		barrier_lock_block(orig, object);
	}
	
	return block;
}

/* we are a barrier, so just synthesize it if it's not already in this zone */
static bdesc_t * xor_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct xor_info * info = (struct xor_info *) OBJLOCAL(object);
	bdesc_t * block;
	bdesc_t * orig;
	
	block = blockman_managed_lookup(info->blockman, number);
	if(block)
	{
		assert(block->count == count);
		return block;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->numblocks)
		return NULL;
	
	block = bdesc_alloc(number, info->blocksize, count);
	if(!block)
		return NULL;
	bdesc_autorelease(block);
	
	block->ddesc->synthetic = 1;
	
	/* we must lock the lower block before we can allow the upper block to exist */
	orig = CALL(info->bd, synthetic_read_block, number, count);
	if(!orig)
		return NULL;
	barrier_lock_block(orig, object);
	
	if(blockman_managed_add(info->blockman, block) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

static int xor_bd_mangle(bdesc_t * block, void * data, int mangle)
{
	xor_bd_xor(block, *(uint32_t *) data);
	return 0;
}

static int xor_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct xor_info * info = (struct xor_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->numblocks)
		return -E_INVAL;
	
	return barrier_single_forward(info->bd, block->number, object, block, xor_bd_mangle, &info->xor_key);
}

static int xor_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static int xor_bd_destroy(BD_t * bd)
{
	struct xor_info * info = (struct xor_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	blockman_destroy(&info->blockman);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

static void xor_bd_block_destroy(BD_t * owner, uint32_t block, uint16_t length)
{
	struct xor_info * info = (struct xor_info *) OBJLOCAL(owner);
	bdesc_t * orig = CALL(info->bd, synthetic_read_block, block, length / info->blocksize);
	assert(orig);
	barrier_unlock_block(orig, owner);
}

BD_t * xor_bd(BD_t * disk, uint32_t xor_key)
{
	struct xor_info * info;
	BD_t * bd;

	if(!disk)
		return NULL;

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	bd->level = 0;
	
	info = malloc(sizeof(struct xor_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->blocksize = CALL(disk, get_blocksize);
	info->blockman = blockman_create(info->blocksize, bd, xor_bd_block_destroy);
	if(!info->blockman)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, xor_bd, info);
	
	info->bd = disk;
	info->xor_key = xor_key;
	info->numblocks = CALL(disk, get_numblocks);
	info->atomicsize = CALL(disk, get_atomicsize);

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
