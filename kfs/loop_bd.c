#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/loop_bd.h>


#define LOOP_DEBUG 0

#if LOOP_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct loop_state {
	LFS_t * lfs;
	fdesc_t * file;
	const char * filename;
};
typedef struct loop_state loop_state_t;


static uint32_t loop_get_numblocks(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_state_t * state = (loop_state_t *) bd->instance;
	return CALL(state->lfs, get_file_numblocks, state->file);
}

static uint16_t loop_get_blocksize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_state_t * state = (loop_state_t *) bd->instance;
	return CALL(state->lfs, get_blocksize);
}

static uint16_t loop_get_atomicsize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_state_t * state = (loop_state_t *) bd->instance;
	BD_t * lfs_bd = CALL(state->lfs, get_blockdev);
	return CALL(lfs_bd, get_atomicsize);
}

static bdesc_t * loop_read_block(BD_t * bd, uint32_t number)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_state_t * state = (loop_state_t *) bd->instance;
	uint32_t blksize = CALL(state->lfs, get_blocksize);
	bdesc_t * bdesc;

	bdesc = CALL(state->lfs, get_file_block, state->file, number*blksize);
	if (!bdesc)
		return NULL;

	// adjust bdesc to match this bd
	if (bdesc_alter(&bdesc) < 0)
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	bdesc->bd = bd;

	return bdesc;
}

static int loop_write_block(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_state_t * state = (loop_state_t *) bd->instance;
	BD_t * lfs_bd = CALL(state->lfs, get_blockdev);
	uint32_t refs;
	chdesc_t * head = NULL;
	chdesc_t * tail;
	int r;

	if(block->bd != bd)
		return -E_INVAL;

	refs = block->refs;
	block->translated++;
	block->bd = lfs_bd;

	r =  CALL(state->lfs, write_block, block, block->offset, block->length, block->ddesc->data, &head, &tail);

	if (refs)
	{
		block->bd = bd;
		block->translated--;
	}

	return r;
}

static int loop_sync(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_state_t * state = (loop_state_t *) bd->instance;
	BD_t * lfs_bd = CALL(state->lfs, get_blockdev);
	uint32_t refs;
	int r;

	if (!block)
		return CALL(state->lfs, sync, state->filename);

	assert(block->bd == bd);

	refs = block->refs;
	block->translated++;
	block->bd = lfs_bd;

	r = CALL(lfs_bd, sync, block);

	if (refs)
	{
		block->bd = bd;
		block->translated--;
	}

	return r;
}

static int loop_destroy(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_state_t * state = (loop_state_t *) bd->instance;

	CALL(state->lfs, free_fdesc, state->file);
	free((char *) state->filename);
	free(state->file);
	memset(state, 0, sizeof(*state));
	free(state);

	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


BD_t * loop_bd(LFS_t * lfs, const char * file)
{
	Dprintf("%s(lfs 0x%08x, file \"%s\")\n", __FUNCTION__, lfs, file);
	BD_t * bd;
	loop_state_t * state;

	if (!lfs || !file)
		return NULL;

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;

	state = malloc(sizeof(*state));
	if(!state)
		goto error_bd;

	bd->instance = state;

	ASSIGN(bd, loop, get_numblocks);
	ASSIGN(bd, loop, get_blocksize);
	ASSIGN(bd, loop, get_atomicsize);
	ASSIGN(bd, loop, read_block);
	ASSIGN(bd, loop, write_block);
	ASSIGN(bd, loop, sync);
	ASSIGN_DESTROY(bd, loop, destroy);

	state->lfs = lfs;

	state->filename = strdup(file);
	if (!state->filename)
		goto error_state;

	state->file = CALL(state->lfs, lookup_name, state->filename);
	if (!state->file)
		goto error_filename;

	return bd;

  error_filename:
	free((char *) state->filename);
  error_state:
	free(state);
  error_bd:
	free(bd);
	return NULL;
}
