#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/chdesc_stripper_bd.h>


#define CHDESC_STRIPPER_DEBUG 1

#if CHDESC_STRIPPER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct chdesc_stripper_state {
	BD_t * bd;
};
typedef struct chdesc_stripper_state chdesc_stripper_state_t;


//
// Intercepted BD_t functions

static int chdesc_stripper_write_block(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	const chdesc_t * tmp;
	chdesc_t * block_chdesc, * cur_chdesc;
	uint32_t refs;
	int r;

	if (block->bd != bd)
		printf("block->bd 0x%08x, bd 0x%08x\n", block->bd, bd);
	assert(block->bd == bd);

	tmp = depman_get_deps(block);
	r = chdesc_weak_retain((chdesc_t *) tmp, &block_chdesc);
	if (r < 0)
		return r;

	assert(!block_chdesc || !block_chdesc->dependents); // no one should depend on block

	// Satisfy inter-BD deps

	while (block_chdesc && block_chdesc->dependencies
		   && (cur_chdesc = block_chdesc->dependencies->desc))
	{
		assert(cur_chdesc->block->bd != bd); // no intra-BD deps should remain

		r = CALL(cur_chdesc->block->bd, sync, cur_chdesc->block);
		if (r < 0)
		{
			fprintf(STDERR_FILENO, "%s: while stripping, a BD write errored: %e\n", __FUNCTION__, r);
			chdesc_weak_release(&block_chdesc);
			return r;
		}
	}

	// Write the block

	refs = block->refs;
	block->translated++;
	block->bd = state->bd;

	r = CALL(state->bd, write_block, block);

	if (refs)
	{
		block->bd = bd;
		block->translated--;
	}

	if (r >= 0)
	{
		if (block_chdesc)
		{
			int s = depman_remove_chdesc(block_chdesc);
			assert(s >= 0);
		}
	}
	else
	{
		if (block_chdesc)
			chdesc_weak_release(&block_chdesc);
	}

	return r;
}

static int chdesc_stripper_destroy(BD_t * bd)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, bd);
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;

	memset(state, 0, sizeof(*state));
	free(state);
	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


//
// Passthrough BD_t functions needing translation

static bdesc_t * chdesc_stripper_read_block(BD_t * bd, uint32_t number)
{
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	bdesc_t * bdesc;
	int r;

	if (!(bdesc = CALL(state->bd, read_block, number)))
		return NULL;

	// adjust bdesc to match this bd
	if ((r = bdesc_alter(&bdesc)) < 0)
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	bdesc->bd = bd;

	return bdesc;
}

static int chdesc_stripper_sync(BD_t * bd, bdesc_t * block)
{
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	uint32_t refs;
	int r;

	if (!block)
		return CALL(state->bd, sync, NULL);

	assert(block->bd == bd);

	refs = block->refs;
	block->translated++;
	block->bd = state->bd;

	r = CALL(state->bd, sync, block);

	if (refs)
	{
		block->bd = bd;
		block->translated--;
	}

	return r;
}


//
// Passthrough BD_t functions

static uint32_t chdesc_stripper_get_numblocks(BD_t * bd)
{
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	return CALL(state->bd, get_numblocks);
}

static uint16_t chdesc_stripper_get_blocksize(BD_t * bd)
{
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	return CALL(state->bd, get_blocksize);
}

static uint16_t chdesc_stripper_get_atomicsize(BD_t * bd)
{
	chdesc_stripper_state_t * state = (chdesc_stripper_state_t *) bd->instance;
	return CALL(state->bd, get_atomicsize);
}


//
//

BD_t * chdesc_stripper_bd(BD_t * disk)
{
	BD_t * bd;
	chdesc_stripper_state_t * state;

	bd = malloc(sizeof(*bd));
	if (!bd)
		return NULL;
	
	state = malloc(sizeof(*state));
	if (!state)
		goto error_bd;
	bd->instance = state;
	
	ASSIGN(bd, chdesc_stripper, get_numblocks);
	ASSIGN(bd, chdesc_stripper, get_blocksize);
	ASSIGN(bd, chdesc_stripper, get_atomicsize);
	ASSIGN(bd, chdesc_stripper, read_block);
	ASSIGN(bd, chdesc_stripper, write_block);
	ASSIGN(bd, chdesc_stripper, sync);
	ASSIGN_DESTROY(bd, chdesc_stripper, destroy);

	state->bd = disk;
	
	return bd;

  error_bd:
	free(bd);
	return NULL;
}
