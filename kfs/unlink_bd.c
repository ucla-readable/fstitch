#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/debug.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/unlink_bd.h>

struct unlink_info {
	BD_t * bd;
};

static int unlink_bd_get_config(void * object, int level, char * string, size_t length)
{
	/* no config to report */
	if(length >= 1)
		string[0] = 0;
	return 0;
}

static int unlink_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if(length >= 1)
		string[0] = 0;
	return 0;
}

static uint32_t unlink_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t unlink_bd_get_blocksize(BD_t * object)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, get_blocksize);
}

static uint16_t unlink_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static bdesc_t * unlink_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, read_block, number, count);
}

static bdesc_t * unlink_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, synthetic_read_block, number, count);
}

static int unlink_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct unlink_info * info = (struct unlink_info *) OBJLOCAL(object);
	chdesc_t * write_head = CALL(info->bd, get_write_head);
	chdesc_t * next = NULL;
	chdesc_t * chdesc;
	int r;
	
	/* inspect and modify all chdescs passing through */
	for(chdesc = block->ddesc->index_changes[object->graph_index].head; chdesc; chdesc = next)
	{
		int needs_head = 0;
		chdepdesc_t ** deps = &chdesc->befores;
		
		assert(chdesc->owner == object);
		next = chdesc->ddesc_index_next;
		
		while(*deps)
		{
			chdesc_t * dep = (*deps)->before.desc;
			/* if it's the write head, or if it's on the same block, leave it alone */
			if(dep == write_head || (dep->block && dep->block->ddesc == block->ddesc))
			{
				deps = &(*deps)->before.next;
				continue;
			}
			/* otherwise remove this dependency */
			/* WARNING: this makes this module incompatible
			 * with opgroups between different file systems */
			chdesc_dep_remove(*deps);
			needs_head = 1;
		}
		
		if(needs_head && write_head)
		{
			chdesc->flags |= CHDESC_SAFE_AFTER;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_SAFE_AFTER);
			r = chdesc_add_depend(chdesc, write_head);
			if(r < 0)
				kpanic("Holy Mackerel!");
			chdesc->flags &= ~CHDESC_SAFE_AFTER;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdesc, CHDESC_SAFE_AFTER);
		}
	}
	
	/* this should never fail */
	r = chdesc_push_down(object, block, info->bd, block);
	if(r < 0)
		return r;
	
	/* write it */
	return CALL(info->bd, write_block, block);
}

static int unlink_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t * unlink_bd_get_write_head(BD_t * object)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, get_write_head);
}

static int32_t unlink_bd_get_block_space(BD_t * object)
{
	return CALL(((struct unlink_info *) OBJLOCAL(object))->bd, get_block_space);
}

static int unlink_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct unlink_info *) OBJLOCAL(bd))->bd, bd);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * unlink_bd(BD_t * disk)
{
	struct unlink_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct unlink_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, unlink_bd, info);
	
	info->bd = disk;
	bd->level = disk->level;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
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
