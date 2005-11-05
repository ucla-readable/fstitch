#include <lib/fixed_max_heap.h>
#include <malloc.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/revision.h>

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MARKED is set to 1 for each chdesc in graph,
 * distance is set to 0 for each chdesc in graph.
 *
 * side effect: returns number of chdescs in the graph. these two
 * functionalities were merged to avoid traversing the graph an extra
 * time.
 */
static int reset_distance(chdesc_t * ch)
{
	chmetadesc_t * p;
	int num = 1;
	if(ch->flags & CHDESC_MARKED)
		return 0;
	ch->flags |= CHDESC_MARKED;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, ch, CHDESC_MARKED);
	ch->distance = 0;
	for(p = ch->dependencies; p; p = p->next)
		num += reset_distance(p->desc);
	return num;
}

/*
 * precondition: all nodes have distance set to zero.
 *
 * postconditions: distance of each chdesc is set to (maximum distance
 * from ch) + num.
 *
 * to get the distance from each node to the root, do
 * calculate_distance(root, 0);
 */
static void calculate_distance(chdesc_t * ch, int num)
{
	chmetadesc_t * p;
	if(num && ch->distance >= num)
		return;
	ch->distance = num;
	for(p = ch->dependencies; p; p = p->next)
		calculate_distance(p->desc, num + 1);
}

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
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		// we really want a min heap, so use negative distance
		fixed_max_heap_insert(heap, d->desc, -d->desc->distance);
	// pop & rollback
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(decider(c, data))
			continue;
		r = chdesc_rollback(c);
		if(r < 0)
			panic("can't rollback!\n");
	}
	fixed_max_heap_free(heap);
	
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
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(decider(c, data))
			continue;
		r = chdesc_apply(c);
		if(r < 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	
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
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(c->owner == bd)
		{
			chdesc_satisfy(&c);
			continue;
		}
		r = chdesc_apply(c);
		if(r < 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	
	return 0;
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
		slice->full = NULL;
	
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
			fprintf(STDERR_FILENO, "%s(): chdesc is not owned by us, but it's in our slice...\n", __FUNCTION__);
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
			fprintf(STDERR_FILENO, "%s(): chdesc is not owned by target, but it's in our slice...\n", __FUNCTION__);
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
