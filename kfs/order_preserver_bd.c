#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/order_preserver_bd.h>

#define ORDER_PRESERVER_DEBUG 0

#if ORDER_PRESERVER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct order_preserver_state {
	BD_t * bd;
	chdesc_t * prev_head;
};
typedef struct order_preserver_state order_preserver_state_t;


//
// Intercepted BD_t functions

static int order_preserver_write_block(BD_t * bd, bdesc_t * block_new)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block_new);
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
	bdesc_t * block_old;
	chdesc_t * head = NULL, * tail = NULL;
	// a backup of state->prev_head so that it can be restored upon a failure:
	chdesc_t * prev_head_backup = NULL; 
	int r;

	assert(block_new->bd == bd);
	// block_new should have no deps
	// (however it /is/ ok for others to depend on block. ex: inter-bd deps.)
	assert(!depman_get_deps(block_new));

	block_old = CALL(state->bd, read_block, block_new->number);
	if (!block_old)
		return -E_UNSPECIFIED;
	if ((r = chdesc_create_full(block_old, block_new->ddesc->data, &head, &tail)) < 0)
		goto error_read_block_old;

	if (state->prev_head)
	{
		if ((r = chdesc_add_depend(tail, state->prev_head)))
			goto error_chdesc_create;

		if ((r = chdesc_weak_retain(state->prev_head, &prev_head_backup)) < 0)
			goto error_add_depend;
		chdesc_weak_release(&state->prev_head);
	}

	if ((r = chdesc_weak_retain(head, &state->prev_head)) < 0)
		goto error_prev_head_backup_weak_retain;

	if ((r = depman_add_chdesc(head)) < 0)
		goto error_head_weak_retain;

	if ((r = CALL(state->bd, write_block, block_old)) < 0)
		goto error_depman_add;
	
	bdesc_drop(&block_old);

	// prev_head_backup was not needed (no errors), release it
	chdesc_weak_release(&prev_head_backup);

	// drop block_new *only* on success
	bdesc_drop(&block_new);

	return r;

  error_depman_add:
	// TODO: remove the subgraph added to depman
	fprintf(STDERR_FILENO, "WARNING: %s%d: post-failure leakage into depman.\n", __FILE__, __LINE__);
  error_head_weak_retain:
	chdesc_weak_release(&prev_head_backup);
  error_prev_head_backup_weak_retain:
	if (prev_head_backup)
	{
		(void) chdesc_weak_retain(prev_head_backup, &state->prev_head);
		chdesc_weak_release(&prev_head_backup);
	}
  error_add_depend:
	if (state->prev_head)
		(void) chdesc_remove_depend(head, state->prev_head);
  error_chdesc_create:
	// TODO: add this: (void) chdesc_destroy_graph(&head);
	fprintf(STDERR_FILENO, "WARNING: %s%d: post-failure chdesc leakage.\n", __FILE__, __LINE__);
  error_read_block_old:
	if (block_old)
		bdesc_drop(&block_old);
	return r;
}

static int order_preserver_destroy(BD_t * bd)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, bd);
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;

	if (state->prev_head)
		chdesc_weak_release(&state->prev_head);

	memset(state, 0, sizeof(*state));
	free(state);
	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


//
// Passthrough BD_t functions needing translation

static bdesc_t * order_preserver_read_block(BD_t * bd, uint32_t number)
{
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
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

static int order_preserver_sync(BD_t * bd, bdesc_t * block)
{
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
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

static uint32_t order_preserver_get_numblocks(BD_t * bd)
{
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
	return CALL(state->bd, get_numblocks);
}

static uint16_t order_preserver_get_blocksize(BD_t * bd)
{
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
	return CALL(state->bd, get_blocksize);
}

static uint16_t order_preserver_get_atomicsize(BD_t * bd)
{
	order_preserver_state_t * state = (order_preserver_state_t *) bd->instance;
	return CALL(state->bd, get_atomicsize);
}


//
//

BD_t * order_preserver_bd(BD_t * disk)
{
	BD_t * bd;
	order_preserver_state_t * state;

	bd = malloc(sizeof(*bd));
	if (!bd)
		return NULL;
	
	state = malloc(sizeof(*state));
	if (!state)
		goto error_bd;
	bd->instance = state;
	
	ASSIGN(bd, order_preserver, get_numblocks);
	ASSIGN(bd, order_preserver, get_blocksize);
	ASSIGN(bd, order_preserver, get_atomicsize);
	ASSIGN(bd, order_preserver, read_block);
	ASSIGN(bd, order_preserver, write_block);
	ASSIGN(bd, order_preserver, sync);
	ASSIGN_DESTROY(bd, order_preserver, destroy);

	state->bd = disk;
	state->prev_head = NULL;
	
	return bd;

  error_bd:
	free(bd);
	return NULL;
}
