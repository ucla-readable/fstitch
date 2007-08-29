#include <lib/platform.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/debug.h>
#include <fscore/modman.h>
#include <fscore/patch.h>

#include <modules/unlink_bd.h>

struct unlink_info {
	BD_t my_bd;
	
	BD_t * bd;
	patch_t ** write_head;
};

static bdesc_t * unlink_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	return CALL(((struct unlink_info *) object)->bd, read_block, number, count, page);
}

static bdesc_t * unlink_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	return CALL(((struct unlink_info *) object)->bd, synthetic_read_block, number, count, page);
}

static int unlink_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct unlink_info * info = (struct unlink_info *) object;
	patch_t * write_head = info->write_head ? *info->write_head : NULL;
	patch_t * next = NULL;
	patch_t * patch;
	const int engaged = patchgroup_engaged();
	int r;
	
	/* inspect and modify all patches passing through */
	for(patch = block->ddesc->index_patches[object->graph_index].head; patch; patch = next)
	{
		int needs_head = 1;
		patchdep_t ** deps = &patch->befores;
		
		assert(patch->owner == object);
		next = patch->ddesc_index_next;
		
		while(*deps)
		{
			patch_t * dep = (*deps)->before.desc;
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
			 * with patchgroups, period */
			patch_dep_remove(*deps);
		}
		
		if(needs_head && write_head)
		{
			patch->flags |= PATCH_SAFE_AFTER;
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, patch, PATCH_SAFE_AFTER);
			r = patch_add_depend(patch, write_head);
			if(r < 0)
				kpanic("Holy Mackerel!");
			patch->flags &= ~PATCH_SAFE_AFTER;
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_CLEAR_FLAGS, patch, PATCH_SAFE_AFTER);
		}
		
		if(engaged)
		{
			/* scan the afters as well, and unhook any patchgroup patches */
			/* WARNING: see warning above */
			deps = &patch->afters;
			while(*deps)
				if(((*deps)->after.desc->flags & PATCH_NO_PATCHGROUP) && (*deps)->after.desc->type == EMPTY)
					patch_dep_remove(*deps);
				else
					deps = &(*deps)->before.next;
			/* and set the patchgroup exemption flag */
			patch->flags |= PATCH_NO_PATCHGROUP;
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, patch, PATCH_NO_PATCHGROUP);
		}
	}
	
	/* this should never fail */
	r = patch_push_down(block, object, info->bd);
	if(r < 0)
		return r;
	
	/* write it */
	return CALL(info->bd, write_block, block, number);
}

static int unlink_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
	return FLUSH_EMPTY;
}

static patch_t ** unlink_bd_get_write_head(BD_t * object)
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
