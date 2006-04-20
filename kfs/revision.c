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
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be rolled back */
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(!decider(scan->dependency.desc, data))
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(!decider(scan->dependency.desc, data))
			chdescs[i++] = scan->dependency.desc;
	
	for(;;)
	{
		int again = 0;
		for(i = count - 1; i != -1; i--)
		{
			/* already rolled back? */
			if(chdescs[i]->flags & CHDESC_ROLLBACK)
				continue;
			/* check for overlapping, non-rolled back chdescs above us */
			for(scan = chdescs[i]->dependents; scan; scan = scan->dependent.next)
			{
				if(scan->dependent.desc->flags & CHDESC_ROLLBACK)
					continue;
				if(!scan->dependent.desc->block || scan->dependent.desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->dependent.desc, chdescs[i]))
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
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be rolled forward */
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(!decider(scan->dependency.desc, data))
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(!decider(scan->dependency.desc, data))
			chdescs[i++] = scan->dependency.desc;
	
	for(;;)
	{
		int again = 0;
		for(i = 0; i != count; i++)
		{
			/* already rolled forward? */
			if(!(chdescs[i]->flags & CHDESC_ROLLBACK))
				continue;
			/* check for overlapping, rolled back chdescs below us */
			for(scan = chdescs[i]->dependencies; scan; scan = scan->dependency.next)
			{
				if(!(scan->dependency.desc->flags & CHDESC_ROLLBACK))
					continue;
				if(!scan->dependency.desc->block || scan->dependency.desc->block->ddesc != block->ddesc)
					continue;
				if(chdesc_overlap_check(scan->dependency.desc, chdescs[i]))
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
	size_t chdescs_size;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!root)
		return 0;
	
	/* find out how many chdescs are to be satisfied */
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(scan->dependency.desc->owner == bd)
			count++;
	
	chdescs_size = sizeof(*chdescs) * count;
	chdescs = smalloc(chdescs_size);
	if(!chdescs)
		return -E_NO_MEM;
	
	
	for(scan = root->dependencies; scan; scan = scan->dependency.next)
		if(scan->dependency.desc->owner == bd)
			chdescs[i++] = scan->dependency.desc;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
		for(i = 0; i != count; i++)
		{
			if(!chdescs[i])
				continue;
			if(chdescs[i]->dependencies)
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

#include <lib/string.h>
// TODO: how do we want to (and should we) optimize this in the kernel?
static void * __srealloc(void * p, size_t p_size, size_t new_size)
{
	void * q = smalloc(new_size);
	if(!q)
		return NULL;
	if(p)
		memcpy(q, p, p_size);
	sfree(p, p_size);
	return q;
}

#define STATIC_STATES_CAPACITY 256

static uint32_t ready_epoch = 1;

/* A chdesc that is externally ready has one of these properties:
 *   1. It has no dependencies.
 *   2. It only has dependencies whose levels are less than or equal to its target level.
 *   3. It only has dependencies which are on the same block and which are ready.
 *   4. It only has dependencies as in #2 and #3 above.
 * A chdesc that is internally ready need not satisfy as much: dependencies on
 * chdescs owned by other block devices are ignored. Note that this causes
 * indirect dependencies on chdescs owned by this block device to be missed. */

static bool revision_slice_chdesc_is_ready(chdesc_t * chdesc, const BD_t * const owner, const bdesc_t * const block, const uint16_t target_level, const bool external)
{
	/* recursion-on-the-heap support */
	struct chdesc_is_ready_state {
			chdesc_t * chdesc;
			chmetadesc_t * meta;
	};
	typedef struct chdesc_is_ready_state chdesc_is_ready_state_t;
	static chdesc_is_ready_state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	chdesc_is_ready_state_t * states = static_states;
	chdesc_is_ready_state_t * state = states;
	
	chmetadesc_t * meta;
	size_t next_index;

 recurse_start:
	if(chdesc->ready_epoch == ready_epoch)
	{
		if(chdesc->flags & CHDESC_READY)
			goto recurse_done;
		else
			goto exit;
	}

	chdesc->flags &= ~CHDESC_READY;
	chdesc->ready_epoch = ready_epoch;
	assert(!chdesc->block || chdesc->owner);
	meta = chdesc->dependencies;

 recurse_meta:
	for(; meta; meta = meta->dependency.next)
	{
		chdesc_t * dep = meta->dependency.desc;
		
		/* handle NOOP properly: it can have NULL block and owner */
		
		if(!dep->owner && !dep->block)
			/* unmanaged NOOP: always recurse */
			goto recurse_down;
		else if(!external && dep->owner != owner)
			continue;
		else if(!dep->block)
		{
			/* managed NOOP: just check level */
			if(CALL(dep->owner, get_devlevel) > target_level)
				goto exit;
		}
		else
		{
			/* normal chdesc or on-block NOOP: recurse when
			 * owner and block match, otherwise check level */
			if(dep->owner == owner && dep->block->ddesc == block->ddesc)
				goto recurse_down;
			else if(CALL(dep->owner, get_devlevel) > target_level)
				goto exit;
		}

		continue;

	recurse_down:
		/* recursively determine whether dep is ready.
		 * we don't recursively call this function and use the stack because the
		 * depth can grow large enough to overflow stacks that are just a few kB.
		 * we instead use the 'states' array to hold this function's recursive
		 * state */
		next_index = 1 + state - &states[0];
		state->chdesc = chdesc;
		state->meta = meta->dependency.next;
		chdesc = dep;
		if(next_index < states_capacity)
			state++;
		else
		{
			size_t cur_size = states_capacity * sizeof(*state);
			states_capacity *= 2;
			if(states == static_states)
			{
				states = smalloc(states_capacity * sizeof(*state));
				if(states)
					memcpy(states, static_states, cur_size);
			}
			else
				states = __srealloc(states, cur_size, states_capacity * sizeof(*state));

			if(!states)
			{
				kdprintf(STDERR_FILENO, "%s: __realloc(%u bytes) failed\n", __FUNCTION__, states_capacity);
				if(states != static_states)
					sfree(states, states_capacity * sizeof(*state));
				return 0;
			}
			state = &states[next_index];
		}
		goto recurse_start;
	}
	
	/* At this point, we know the current chdesc is ready. */
	chdesc->flags |= CHDESC_READY;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_READY);
	
 recurse_done:
	if(state != &states[0])
	{
		state--;
		chdesc = state->chdesc;
		meta = state->meta;
		goto recurse_meta;
	}
	
 exit:
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
	return (chdesc->flags & CHDESC_READY) != 0;
}

revision_slice_t * revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, bool external)
{
	int j = 0;
	chmetadesc_t * meta;
	uint16_t target_level = CALL(target, get_devlevel);
	revision_slice_t * slice = malloc(sizeof(*slice));
	if(!slice)
		return NULL;
	
	slice->owner = owner;
	slice->target = target;
	slice->full_size = 0;
	slice->ready_size = 0;
	slice->ready = NULL;
	if(!block->ddesc->changes)
		return slice;

	/* update ready epoch */
	if (++ready_epoch == 0)
		++ready_epoch;
	
	for(meta = block->ddesc->changes->dependencies; meta; meta = meta->dependency.next)
		if(meta->dependency.desc->owner == owner)
		{
			slice->full_size++;
			if(revision_slice_chdesc_is_ready(meta->dependency.desc, owner, block, target_level, external))
				slice->ready_size++;
		}
	
	if(slice->ready_size)
	{
		slice->ready = scalloc(slice->ready_size, sizeof(*slice->ready));
		if(!slice->ready)
		{
			free(slice);
			return NULL;
		}
		
		for(meta = block->ddesc->changes->dependencies; meta; meta = meta->dependency.next)
			if(meta->dependency.desc->owner == owner && (meta->dependency.desc->flags & CHDESC_READY))
				slice->ready[j++] = meta->dependency.desc;
		assert(j == slice->ready_size);
	}
	
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
			/* KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, slice->ready[i], CHDESC_READY); */
			/* XXX no longer needed slice->ready[i]->flags &= ~CHDESC_READY; */
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
			/* KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, slice->ready[i], CHDESC_READY); */
			/* XXX no longer needed slice->ready[i]->flags |= CHDESC_READY; */
		}
		else
			kdprintf(STDERR_FILENO, "%s(): chdesc is not owned by target, but it's in our slice...\n", __FUNCTION__);
	}
}

void revision_slice_destroy(revision_slice_t * slice)
{
	if(slice->ready)
		sfree(slice->ready, slice->ready_size * sizeof(*slice->ready));
	free(slice);
}
