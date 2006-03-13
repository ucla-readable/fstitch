#if !defined(__KERNEL__)
#include <assert.h>
#else
#warning Add assert.h support
#define assert(x) do { } while(0)
#endif
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/panic.h>
#include <inc/error.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/revision.h>

typedef bool (*revision_decider_t)(chdesc_t * chdesc, void * data);

static bool revision_barrier_decider(chdesc_t * chdesc, void * data)
{
	return chdesc->owner == (BD_t *) data;
}

static bool revision_stamp_decider(chdesc_t * chdesc, void * data)
{
	return chdesc_has_stamp(chdesc, (uint32_t) data);
}

static int _revision_tail_prepare(bdesc_t * block, revision_decider_t decider, void * data)
{
	chdesc_t * root = block->ddesc->changes;
	chmetadesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be rolled back */
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!decider(scan->desc, data))
			count++;
	
	chdescs = malloc(sizeof(*chdescs) * count);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!decider(scan->desc, data))
			chdescs[i++] = scan->desc;
	
	for(;;)
	{
		int again = 0;
		for(i = 0; i != count; i++)
		{
			/* already rolled back? */
			if(chdescs[i]->flags & CHDESC_ROLLBACK)
				continue;
			/* check for overlapping, non-rolled back chdescs above us */
			for(scan = chdescs[i]->dependents; scan; scan = scan->next)
			{
				if(scan->desc->flags & CHDESC_ROLLBACK)
					continue;
				if(!scan->desc->block || scan->desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->desc, chdescs[i]))
					break;
			}
			if(scan)
				again = 1;
			else
			{
				int r = chdesc_rollback(chdescs[i]);
				if(r < 0)
					panic("chdesc_rollback() failed!");
			}
		}
		if(!again)
			break;
	}
	
	free(chdescs);
	
	return 0;
}

int revision_tail_prepare(bdesc_t * block, BD_t * bd)
{
#if CHDESC_BYTE_SUM > 1
	/* be paranoid and rollback/reapply all chdescs to check their sums */
	if(bd)
	{
		int r = revision_tail_prepare(block, NULL);
		assert(r >= 0);
		r = revision_tail_revert(block, NULL);
		assert(r >= 0);
	}
#endif
	return _revision_tail_prepare(block, revision_barrier_decider, bd);
}

int revision_tail_prepare_stamp(bdesc_t * block, uint32_t stamp)
{
	return _revision_tail_prepare(block, revision_stamp_decider, (void *) stamp);
}

static int _revision_tail_revert(bdesc_t * block, revision_decider_t decider, void * data)
{
	chdesc_t * root = block->ddesc->changes;
	chmetadesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be rolled forward */
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!decider(scan->desc, data))
			count++;
	
	chdescs = malloc(sizeof(*chdescs) * count);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!decider(scan->desc, data))
			chdescs[i++] = scan->desc;
	
	for(;;)
	{
		int again = 0;
		for(i = 0; i != count; i++)
		{
			/* already rolled forward? */
			if(!(chdescs[i]->flags & CHDESC_ROLLBACK))
				continue;
			/* check for overlapping, rolled back chdescs below us */
			for(scan = chdescs[i]->dependencies; scan; scan = scan->next)
			{
				if(!(scan->desc->flags & CHDESC_ROLLBACK))
					continue;
				if(!scan->desc->block || scan->desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->desc, chdescs[i]))
					break;
			}
			if(scan)
				again = 1;
			else
			{
				int r = chdesc_apply(chdescs[i]);
				if(r < 0)
					panic("chdesc_apply() failed!");
			}
		}
		if(!again)
			break;
	}
	
	free(chdescs);
	
	return 0;
}

int revision_tail_revert(bdesc_t * block, BD_t * bd)
{
	return _revision_tail_revert(block, revision_barrier_decider, bd);
}

int revision_tail_revert_stamp(bdesc_t * block, uint32_t stamp)
{
	return _revision_tail_revert(block, revision_stamp_decider, (void *) stamp);
}

int revision_tail_acknowledge(bdesc_t * block, BD_t * bd)
{
	chdesc_t * root = block->ddesc->changes;
	chmetadesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be satisfied */
	for(scan = root->dependencies; scan; scan = scan->next)
		if(scan->desc->owner == bd)
			count++;
	
	chdescs = malloc(sizeof(*chdescs) * count);
	if(!chdescs)
		return -E_NO_MEM;
	
	
	for(scan = root->dependencies; scan; scan = scan->next)
		if(scan->desc->owner == bd)
			chdescs[i++] = scan->desc;
	
	for(;;)
	{
		int again = 0;
		for(i = 0; i != count; i++)
		{
			if(!chdescs[i])
				continue;
			if(chdescs[i]->dependencies)
				again = 1;
			else
				chdesc_satisfy(&chdescs[i]);
		}
		if(!again)
			break;
	}
	free(chdescs);
	
	return revision_tail_revert(block, bd);
}


/* ---- Revision slices: library functions for use inside barrier zones ---- */

/* Unless we use chdesc stamps, of which there are a limited number, we don't
 * know whether chdescs that we don't own are above or below us. But that's OK,
 * because we don't need to. Hence there is no revision_slice_prepare()
 * function, because we don't need to apply or roll back any chdescs to use
 * revision slices. Basically a revision slice is a set of change descriptors at
 * a particular time, organized in a nice way so that we can figure out which
 * ones are ready to be written down and which ones are not. */

/* A chdesc that is externally ready has one of these properties:
 *   1. It has no dependencies.
 *   2. It only has dependencies whose levels are less than or equal to its target level.
 *   3. It only has dependencies which are on the same block and which are ready.
 *   4. It only has dependencies as in #2 and #3 above.
 * A chdesc that is internally ready need not satisfy as much: dependencies on
 * chdescs owned by other block devices are ignored. Note that this causes
 * indirect dependencies on chdescs owned by this block device to be missed. */
/* FIXME: this function will have O(n^2) traversal behavior in the case when n blocks are *not* ready... */
static bool revision_slice_chdesc_is_ready(chdesc_t * chdesc, BD_t * owner, bdesc_t * block, uint16_t target_level, bool external)
{
	/* assume ready until we find evidence to the contrary */
	bool ready = 1;
	chmetadesc_t * meta;
	
	if(chdesc->flags & CHDESC_READY)
		return 1;
	
	assert(!chdesc->block || chdesc->owner);
	
	for(meta = chdesc->dependencies; meta; meta = meta->next)
	{
		chdesc_t * dep = meta->desc;
		bool recurse = 0;
		
		/* handle NOOP properly: it can have NULL block and owner */
		
		if(!dep->owner && !dep->block)
			/* unmanaged NOOP: always recurse */
			recurse = 1;
		else if(!external && dep->owner != owner)
			continue;
		else if(!dep->block)
		{
			/* managed NOOP: just check level */
			if(CALL(dep->owner, get_devlevel) > target_level)
			{
				ready = 0;
				break;
			}
		}
		else
		{
			/* normal chdesc or on-block NOOP: recurse when
			 * owner and block match, otherwise check level */
			if(dep->owner == owner && dep->block->ddesc == block->ddesc)
				recurse = 1;
			/* here the !external case is both an optimization and a
			 * way to make sure the right semantics are preserved:
			 * we know !external means dep->owner == owner, and
			 * presumably the level of owner is greater than the
			 * target level... however, they can be equal, as they
			 * would be when revision_slice_create() is called from
			 * the block resizer, and we want that to cause
			 * unreadiness since this dependency is on a different
			 * block than the one we are examining right now */
			else if(!external || CALL(dep->owner, get_devlevel) > target_level)
			{
				ready = 0;
				break;
			}
		}
		
		if(recurse && !revision_slice_chdesc_is_ready(dep, owner, block, target_level, external))
		{
			ready = 0;
			break;
		}
	}
	
	if(ready && chdesc->block)
	{
		/* only set CHDESC_READY if we know it will get cleared by revision_slice_push_down */
		assert(chdesc->block->ddesc == block->ddesc);
		
		chdesc->flags |= CHDESC_READY;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_READY);
	}
	return ready;
}

revision_slice_t * revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, bool external)
{
	int i = 0, j = 0;
	chmetadesc_t * meta;
	uint16_t target_level = CALL(target, get_devlevel);
	revision_slice_t * slice = malloc(sizeof(*slice));
	if(!slice)
		return NULL;
	
	slice->owner = owner;
	slice->target = target;
	slice->full_size = 0;
	slice->ready_size = 0;
	if(!block->ddesc->changes)
	{
		slice->full = NULL;
		slice->ready = NULL;
		return slice;
	}
	
	for(meta = block->ddesc->changes->dependencies; meta; meta = meta->next)
		if(meta->desc->owner == owner)
		{
			slice->full_size++;
			if(revision_slice_chdesc_is_ready(meta->desc, owner, block, target_level, external))
				slice->ready_size++;
		}
	
	if(slice->full_size)
	{
		slice->full = calloc(slice->full_size, sizeof(*slice->full));
		if(!slice->full)
		{
			/* no need to clear CHDESC_READY: anything ready
			 * now will still be ready until it's written */
			free(slice);
			return NULL;
		}
		if(slice->ready_size)
		{
			slice->ready = calloc(slice->ready_size, sizeof(*slice->ready));
			if(!slice->ready)
			{
				/* see comment above */
				free(slice->full);
				free(slice);
				return NULL;
			}
		}
		else
			slice->ready = NULL;
	}
	else
	{
		slice->full = NULL;
		slice->ready = NULL;
	}
	
	for(meta = block->ddesc->changes->dependencies; meta; meta = meta->next)
		if(meta->desc->owner == owner)
		{
			slice->full[i++] = meta->desc;
			if(meta->desc->flags & CHDESC_READY)
				slice->ready[j++] = meta->desc;
		}
	assert(i == slice->full_size);
	assert(j == slice->ready_size);
	
	return slice;
}

void revision_slice_push_down(revision_slice_t * slice)
{
	/* like chdesc_push_down, but without block reassignment (only needed
	 * for things changing block numbers) and for slices instead of all
	 * chdescs: it only pushes down the ready part of the slice */
	/* CLEAR CHDESC_READY */
	int i;
	for(i = 0; i != slice->ready_size; i++)
	{
		if(!slice->ready[i])
			continue;
		if(slice->ready[i]->owner == slice->owner)
		{
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, slice->ready[i], slice->target);
			slice->ready[i]->owner = slice->target;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, slice->ready[i], CHDESC_READY);
			slice->ready[i]->flags &= ~CHDESC_READY;
		}
		else
			kdprintf(STDERR_FILENO, "%s(): chdesc is not owned by us, but it's in our slice...\n", __FUNCTION__);
	}
}

void revision_slice_pull_up(revision_slice_t * slice)
{
	/* the reverse of revision_slice_push_down, in case write() fails */
	/* SET CHDESC_READY */
	int i;
	for(i = 0; i != slice->ready_size; i++)
	{
		if(!slice->ready[i])
			continue;
		if(slice->ready[i]->owner == slice->target)
		{
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, slice->ready[i], slice->owner);
			slice->ready[i]->owner = slice->owner;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, slice->ready[i], CHDESC_READY);
			slice->ready[i]->flags |= CHDESC_READY;
		}
		else
			kdprintf(STDERR_FILENO, "%s(): chdesc is not owned by target, but it's in our slice...\n", __FUNCTION__);
	}
}

void revision_slice_destroy(revision_slice_t * slice)
{
	if(slice->full)
		free(slice->full);
	if(slice->ready)
		free(slice->ready);
	free(slice);
}
