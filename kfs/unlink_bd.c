#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/debug.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/unlink_bd.h>

struct unlink_info {
	BD_t my_bd;
	
	BD_t * bd;
	chdesc_t ** write_head;
};

static bdesc_t * unlink_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	return CALL(((struct unlink_info *) object)->bd, read_block, number, count);
}

static bdesc_t * unlink_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	return CALL(((struct unlink_info *) object)->bd, synthetic_read_block, number, count);
}

static int unlink_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct unlink_info * info = (struct unlink_info *) object;
	chdesc_t * write_head = info->write_head ? *info->write_head : NULL;
	chdesc_t * next = NULL;
	chdesc_t * chdesc;
	const int engaged = opgroup_engaged();
	int r;
	
	/* inspect and modify all chdescs passing through */
	for(chdesc = block->ddesc->index_changes[object->graph_index].head; chdesc; chdesc = next)
	{
		int needs_head = 1;
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
				if(dep == write_head)
					needs_head = 0;
				continue;
			}
			/* otherwise remove this dependency */
			/* WARNING: this makes this module incompatible
			 * with opgroups, period */
			chdesc_dep_remove(*deps);
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
		
		if(engaged)
		{
			/* scan the afters as well, and unhook any opgroup chdescs */
			/* WARNING: see warning above */
			deps = &chdesc->afters;
			while(*deps)
				if(((*deps)->after.desc->flags & CHDESC_NO_OPGROUP) && (*deps)->after.desc->type == NOOP)
					chdesc_dep_remove(*deps);
				else
					deps = &(*deps)->before.next;
			/* and set the opgroup exemption flag */
			chdesc->flags |= CHDESC_NO_OPGROUP;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_NO_OPGROUP);
		}
	}
	
	/* this should never fail */
	r = chdesc_push_down(block, object, info->bd);
	if(r < 0)
		return r;
	
	/* write it */
	return CALL(info->bd, write_block, block, number);
}

static int unlink_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** unlink_bd_get_write_head(BD_t * object)
{
	return ((struct unlink_info *) object)->write_head;
}

static int32_t unlink_bd_get_block_space(BD_t * object)
{
	return CALL(((struct unlink_info *) object)->bd, get_block_space);
}

static int unlink_bd_destroy(BD_t * bd)
{
	struct unlink_info * info = (struct unlink_info *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

BD_t * unlink_bd(BD_t * disk)
{
	struct unlink_info * info;
	BD_t * bd;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	BD_INIT(bd, unlink_bd);
	
	info->bd = disk;
	info->write_head = CALL(disk, get_write_head);
	bd->level = disk->level;
	bd->graph_index = disk->graph_index + 1;
	bd->numblocks = disk->numblocks;
	bd->blocksize = disk->blocksize;
	bd->atomicsize = disk->atomicsize;
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
