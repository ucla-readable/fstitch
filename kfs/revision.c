#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/panic.h>
#include <inc/error.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/revision.h>

typedef bool (*revision_decider_t)(chdesc_t * chdesc, void * data);

static bool revision_owner_decider(chdesc_t * chdesc, void * data)
{
	return chdesc->owner == (BD_t *) data;
}

static bool revision_stamp_decider(chdesc_t * chdesc, void * data)
{
	return chdesc_has_stamp(chdesc, (uint32_t) data);
}

static int _revision_tail_prepare(bdesc_t * block, revision_decider_t decider, void * data)
{
	chdesc_t * scan;
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be rolled back */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decider(scan, data))
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decider(scan, data))
			chdescs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		for(i = 0; i != count; i++)
		{
			chdepdesc_t * scan;
			/* already rolled back? */
			if(chdescs[i]->flags & CHDESC_ROLLBACK)
				continue;
			/* check for overlapping, non-rolled back chdescs above us */
			for(scan = chdescs[i]->afters; scan; scan = scan->after.next)
			{
				if(scan->after.desc->flags & CHDESC_ROLLBACK)
					continue;
				if(!scan->after.desc->block || scan->after.desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->after.desc, chdescs[i]))
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
	
	sfree(chdescs, chdescs_size);
	
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
	return _revision_tail_prepare(block, revision_owner_decider, bd);
}

int revision_tail_prepare_stamp(bdesc_t * block, uint32_t stamp)
{
	return _revision_tail_prepare(block, revision_stamp_decider, (void *) stamp);
}

static int _revision_tail_revert(bdesc_t * block, revision_decider_t decider, void * data)
{
	chdesc_t * scan;
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be rolled forward */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decider(scan, data))
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decider(scan, data))
			chdescs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		for(i = count - 1; i != -1; i--)
		{
			chdepdesc_t * scan;
			/* already rolled forward? */
			if(!(chdescs[i]->flags & CHDESC_ROLLBACK))
				continue;
			/* check for overlapping, rolled back chdescs below us */
			for(scan = chdescs[i]->befores; scan; scan = scan->before.next)
			{
				if(!(scan->before.desc->flags & CHDESC_ROLLBACK))
					continue;
				if(!scan->before.desc->block || scan->before.desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->before.desc, chdescs[i]))
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
	
	sfree(chdescs, chdescs_size);
	
	return 0;
}

int revision_tail_revert(bdesc_t * block, BD_t * bd)
{
	return _revision_tail_revert(block, revision_owner_decider, bd);
}

int revision_tail_revert_stamp(bdesc_t * block, uint32_t stamp)
{
	return _revision_tail_revert(block, revision_stamp_decider, (void *) stamp);
}

int revision_tail_acknowledge(bdesc_t * block, BD_t * bd)
{
	chdesc_t * scan;
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be satisfied */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(scan->owner == bd)
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(scan->owner == bd)
			chdescs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
		for(i = count - 1; i != -1; i--)
		{
			if(!chdescs[i])
				continue;
			if(chdescs[i]->befores)
				again = 1;
			else
			{
				chdesc_satisfy(&chdescs[i]);
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			kdprintf(STDERR_FILENO, "%s(): there exist unsatisfied chdescs but no progress was made! (debug = %d)\n", __FUNCTION__, KFS_DEBUG_COUNT());
			break;
		}
	}
	sfree(chdescs, chdescs_size);
	
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

/* move 'chdesc' from its ddesc's all_changes list to the list 'tmp_ready' and preserve its all_changes neighbors its tmp list */
static void link_tmp_ready(chdesc_t ** tmp_ready, chdesc_t *** tmp_ready_tail, chdesc_t * chdesc)
{
	chdesc_tmpize_all_changes(chdesc);

	chdesc->ddesc_pprev = tmp_ready;
	chdesc->ddesc_next = *tmp_ready;
	*tmp_ready = chdesc;
	if(chdesc->ddesc_next)
		chdesc->ddesc_next->ddesc_pprev = &chdesc->ddesc_next;
	else
		*tmp_ready_tail = &chdesc->ddesc_next;
}

/* move 'chdesc' back from the list 'tmp_ready' to its ddesc's all_changes */
static void unlink_tmp_ready(chdesc_t ** tmp_ready, chdesc_t *** tmp_ready_tail, chdesc_t * chdesc)
{
	assert(chdesc->block && chdesc->owner);
	if(chdesc->ddesc_pprev)
	{
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = chdesc->ddesc_pprev;
		else
			*tmp_ready_tail = chdesc->ddesc_pprev;
		*chdesc->ddesc_pprev = chdesc->ddesc_next;
		chdesc->ddesc_next = NULL;
		chdesc->ddesc_pprev = NULL;
	}
	else
		assert(!chdesc->ddesc_next);

	chdesc_untmpize_all_changes(chdesc);
}

int revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, revision_slice_t * slice)
{
	chdesc_t * tmp_ready = NULL;
	chdesc_t ** tmp_ready_tail = &tmp_ready;
	chdesc_dlist_t * rcl = &block->ddesc->ready_changes[owner->level];
	chdesc_t * scan;
	/* To write a block revision, all non-ready chdescs on the block must
	 * first be rolled back. Thus when there are non-ready chdescs with
	 * omitted data fields the revision cannot contain any chdescs.
	 * 'nonready_nonrollbackable' implements this. */
	bool nonready_nonrollbackable = 0;

	assert(owner->level - 1 == target->level);
	
	slice->owner = owner;
	slice->target = target;
	slice->all_ready = 1;
	slice->ready_size = 0;
	slice->ready = NULL;

	/* move all the chdescs down a level that can be moved down a level */
	while((scan = rcl->head))
	{
		slice->ready_size++;

		/* push down to update the ready list */
		link_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
		chdesc_unlink_ready_changes(scan);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, scan, target);
		scan->owner = target;
		chdesc_propagate_level_change(scan, owner->level, target->level);
		chdesc_update_ready_changes(scan);
	}

	/* TODO: instead of scanning, we could keep and read a running count in the ddesc */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
	{
		if(scan->owner == owner)
		{
			slice->all_ready = 0;
#if CHDESC_DATA_OMITTANCE
			if(!chdesc_is_rollbackable(scan))
			{
				nonready_nonrollbackable = 1;
				break;
			}
#else
			break;
#endif
		}
	}

	if(slice->ready_size)
	{
		chdesc_t * scan;
		int j = 0;
		if(!nonready_nonrollbackable)
			slice->ready = scalloc(slice->ready_size, sizeof(*slice->ready));
		if(!slice->ready)
		{
			/* pull back up from push down */
			/* it's sad that the tmp list exists solely for this error case.
			 * and it's sad that this scalloc() exists solely for pull_up. */
			for(scan = tmp_ready; scan;)
			{
				chdesc_t * next = scan->ddesc_next;
				chdesc_unlink_ready_changes(scan);
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, scan, owner);
				scan->owner = owner;
				chdesc_propagate_level_change(scan, target->level, owner->level);
				unlink_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
				chdesc_update_ready_changes(scan);
				scan = next;
			}
			
			if(nonready_nonrollbackable)
			{
				slice->ready_size = 0;
				return 0;
			}
			return -E_NO_MEM;
		}

		for(scan = tmp_ready; scan;)
		{
			chdesc_t * next = scan->ddesc_next;
			slice->ready[j++] = scan;
			unlink_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
			scan = next;
		}
		assert(j == slice->ready_size);
	}
	
	return 0;
}

void revision_slice_push_down(revision_slice_t * slice)
{
	/* like chdesc_push_down, but without block reassignment (only needed
	 * for things changing block numbers) and for slices instead of all
	 * chdescs: it only pushes down the ready part of the slice */
	int i;
	for(i = 0; i != slice->ready_size; i++)
	{
		if(!slice->ready[i])
			continue;
		if(slice->ready[i]->owner == slice->owner)
		{
			uint16_t prev_level = chdesc_level(slice->ready[i]);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, slice->ready[i], slice->target);
			chdesc_unlink_ready_changes(slice->ready[i]);
			slice->ready[i]->owner = slice->target;
			chdesc_update_ready_changes(slice->ready[i]);
			if(prev_level != chdesc_level(slice->ready[i]))
				chdesc_propagate_level_change(slice->ready[i], prev_level, chdesc_level(slice->ready[i]));
		}
		else
			kdprintf(STDERR_FILENO, "%s(): chdesc is not owned by us, but it's in our slice...\n", __FUNCTION__);
	}
}

void revision_slice_pull_up(revision_slice_t * slice)
{
	/* the reverse of revision_slice_push_down, in case write() fails */
	int i;
	for(i = 0; i != slice->ready_size; i++)
	{
		if(!slice->ready[i])
			continue;
		if(slice->ready[i]->owner == slice->target)
		{
			uint16_t prev_level = chdesc_level(slice->ready[i]);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, slice->ready[i], slice->owner);
			chdesc_unlink_ready_changes(slice->ready[i]);
			slice->ready[i]->owner = slice->owner;
			chdesc_update_ready_changes(slice->ready[i]);
			if(prev_level != chdesc_level(slice->ready[i]))
				chdesc_propagate_level_change(slice->ready[i], prev_level, chdesc_level(slice->ready[i]));
		}
		else
			kdprintf(STDERR_FILENO, "%s(): chdesc is not owned by target, but it's in our slice...\n", __FUNCTION__);
	}
}

void revision_slice_destroy(revision_slice_t * slice)
{
	if(slice->ready)
	{
		sfree(slice->ready, slice->ready_size * sizeof(*slice->ready));
		slice->ready = NULL;
	}
	slice->owner = NULL;
	slice->target = NULL;
	slice->all_ready = 0;
	slice->ready_size = 0;
}
