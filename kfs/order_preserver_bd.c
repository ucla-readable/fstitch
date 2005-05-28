#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/modman.h>
#include <kfs/order_preserver_bd.h>

#define ORDER_PRESERVER_DEBUG 0

#if ORDER_PRESERVER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct order_info {
	BD_t * bd;
	chdesc_t * prev_head;
};
typedef struct order_info order_info_t;


static int order_preserver_get_config(void * object, int level, char * string, size_t length)
{
	/* no configuration of interest */
	snprintf(string, length, "");
	return 0;
}

static int order_preserver_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}


//
// Intercepted BD_t functions

static int order_preserver_write_block(BD_t * bd, bdesc_t * block_new)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block_new);
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	bdesc_t * block_old;
	chdesc_t * head = NULL, * tail = NULL;
	// a backup of info->prev_head so that it can be restored upon a failure:
	chdesc_t * prev_head_backup = NULL; 
	int r;

	assert(block_new->bd == bd);
	// block_new should have no deps
	// (however it /is/ ok for others to depend on block. ex: inter-bd deps.)
	assert(!depman_get_deps(block_new));

	block_old = CALL(info->bd, read_block, block_new->number);
	if (!block_old)
		return -E_UNSPECIFIED;
	if ((r = chdesc_create_full(block_old, block_new->ddesc->data, &head, &tail)) < 0)
		goto error_read_block_old;

	if (info->prev_head)
	{
		if ((r = chdesc_add_depend(tail, info->prev_head)))
			goto error_chdesc_create;

		if ((r = chdesc_weak_retain(info->prev_head, &prev_head_backup)) < 0)
			goto error_add_depend;
	}

	if ((r = chdesc_weak_retain(head, &info->prev_head)) < 0)
		goto error_prev_head_backup_weak_retain;

	if ((r = depman_add_chdesc(head)) < 0)
		goto error_head_weak_retain;

	if ((r = CALL(info->bd, write_block, block_old)) < 0)
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
	chdesc_weak_release(&info->prev_head);
  error_prev_head_backup_weak_retain:
	if (prev_head_backup)
	{
		(void) chdesc_weak_retain(prev_head_backup, &info->prev_head);
		chdesc_weak_release(&prev_head_backup);
	}
  error_add_depend:
	if (info->prev_head)
		(void) chdesc_remove_depend(head, info->prev_head);
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
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);

	if (info->prev_head)
		chdesc_weak_release(&info->prev_head);

	memset(info, 0, sizeof(*info));
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


//
// Passthrough BD_t functions needing translation

static bdesc_t * order_preserver_read_block(BD_t * bd, uint32_t number)
{
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	bdesc_t * bdesc;
	int r;

	if (!(bdesc = CALL(info->bd, read_block, number)))
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
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	uint32_t refs;
	int r;

	if (!block)
		return CALL(info->bd, sync, NULL);

	assert(block->bd == bd);

	refs = block->refs;
	block->translated++;
	block->bd = info->bd;

	r = CALL(info->bd, sync, block);

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
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	return CALL(info->bd, get_numblocks);
}

static uint16_t order_preserver_get_blocksize(BD_t * bd)
{
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	return CALL(info->bd, get_blocksize);
}

static uint16_t order_preserver_get_atomicsize(BD_t * bd)
{
	order_info_t * info = (order_info_t *) OBJLOCAL(bd);
	return CALL(info->bd, get_atomicsize);
}


//
//

BD_t * order_preserver_bd(BD_t * disk)
{
	BD_t * bd;
	order_info_t * info;

	bd = malloc(sizeof(*bd));
	if (!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if (!info)
		goto error_bd;
	
	BD_INIT(bd, order_preserver, info);
	
	info->bd = disk;
	info->prev_head = NULL;
	
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

  error_bd:
	free(bd);
	return NULL;
}
