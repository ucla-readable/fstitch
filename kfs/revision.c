#include <lib/platform.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/kfsd.h>
#include <kfs/revision.h>

enum decider {
	OWNER,
	FLIGHT
};

/* <% is equivalent to { but does not cause syntax highlighting to go nuts */
#define decide(type, chdesc, data) (<% int __result; \
	switch(type) <% \
		case OWNER: \
			/* it had better be either owned by us or rollbackable */ \
			assert((chdesc)->owner == (BD_t *) (data) || chdesc_is_rollbackable(chdesc)); \
			__result = (chdesc)->owner == (BD_t *) (data); \
			break; \
		case FLIGHT: \
			__result = ((chdesc)->flags & CHDESC_INFLIGHT) != 0; \
			break; \
		default: \
			kpanic("Unknown decider type %d", type); \
	%> __result; %>)

static void dump_revision_loop_state(bdesc_t * block, int count, chdesc_t ** chdescs, const char * function)
{
	int i;
	fprintf(stderr, "%s() is very confused! (debug = %d)\n", function, KFS_DEBUG_COUNT());
	for(i = 0; i != count; i++)
	{
		chdepdesc_t * scan;
		int total = 0;
		if(!chdescs[i])
		{
			fprintf(stderr, "(slot null)\n");
			continue;
		}
		fprintf(stderr, "%p [T%d, L%d, F%x]", chdescs[i], chdescs[i]->type, chdesc_level(chdescs[i]), chdescs[i]->flags);
		if(!chdesc_is_rollbackable(chdescs[i]))
			fprintf(stderr, "!");
		fprintf(stderr, " (<-");
		for(scan = chdescs[i]->afters; scan; scan = scan->after.next)
		{
			total++;
			if(!scan->after.desc->block || scan->after.desc->block->ddesc != block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->after.desc, scan->after.desc->type, scan->after.desc->flags);
			if(!chdesc_is_rollbackable(scan->after.desc))
				fprintf(stderr, "!");
			if(chdesc_overlap_check(scan->after.desc, chdescs[i]))
				fprintf(stderr, "*");
			if(scan->after.desc->block->ddesc->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")%d (->", total);
		total = 0;
		for(scan = chdescs[i]->befores; scan; scan = scan->before.next)
		{
			total++;
			if(!scan->before.desc->block || scan->before.desc->block->ddesc != block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->before.desc, scan->before.desc->type, scan->before.desc->flags);
			if(!chdesc_is_rollbackable(scan->before.desc))
				fprintf(stderr, "!");
			if(chdesc_overlap_check(scan->before.desc, chdescs[i]))
				fprintf(stderr, "*");
			if(scan->before.desc->block->ddesc->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")%d (-->", total);
		for(scan = chdescs[i]->befores; scan; scan = scan->before.next)
		{
			if(!scan->before.desc->block || scan->before.desc->block->ddesc == block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->before.desc, scan->before.desc->type, scan->before.desc->flags);
			if(!chdesc_is_rollbackable(scan->before.desc))
				fprintf(stderr, "!");
			if(scan->before.desc->block->ddesc->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")\n");
	}
	kpanic("too confused to continue");
}

#define REVISION_ARRAY_SIZE 80

static chdesc_t * revision_static_array[REVISION_ARRAY_SIZE];
static chdesc_t ** revision_alloc_array = NULL;
static size_t revision_alloc_array_count = 0;

static void revision_array_free(void * ignore)
{
	if(revision_alloc_array)
		sfree(revision_alloc_array, revision_alloc_array_count * sizeof(*revision_alloc_array));
	revision_alloc_array_count = 0;
}

static chdesc_t ** revision_get_array(size_t count)
{
	if(count <= REVISION_ARRAY_SIZE)
		return revision_static_array;
	if(count <= revision_alloc_array_count)
		return revision_alloc_array;
	if(!revision_alloc_array_count)
	{
		int r = kfsd_register_shutdown_module(revision_array_free, NULL, SHUTDOWN_POSTMODULES);
		if(r < 0)
			return NULL;
	}
	if(revision_alloc_array)
		sfree(revision_alloc_array, revision_alloc_array_count * sizeof(*revision_alloc_array));
	revision_alloc_array_count = count;
	revision_alloc_array = smalloc(count * sizeof(*revision_alloc_array));
	return revision_alloc_array;
}

static int _revision_tail_prepare(bdesc_t * block, enum decider decider, void * data)
{
	chdesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be rolled back */
	/* TODO: look into using ready_changes here? */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	chdescs = revision_get_array(count);
	if(!chdescs)
		return -ENOMEM;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			chdescs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
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
				{
					fprintf(stderr, "chdesc_rollback() failed!\n");
					assert(0);
				}
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			dump_revision_loop_state(block, count, chdescs, __FUNCTION__);
			break;
		}
	}
	
	return count;
}

int revision_tail_prepare(bdesc_t * block, BD_t * bd)
{
	assert(!block->ddesc->in_flight);
	return _revision_tail_prepare(block, OWNER, bd);
}

static int _revision_tail_revert(bdesc_t * block, enum decider decider, void * data)
{
	chdesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be rolled forward */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	chdescs = revision_get_array(count);
	if(!chdescs)
		return -ENOMEM;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			chdescs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
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
				{
					fprintf(stderr, "chdesc_apply() failed!\n");
					assert(0);
				}
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			dump_revision_loop_state(block, count, chdescs, __FUNCTION__);
			break;
		}
	}
	
	return count;
}

int revision_tail_revert(bdesc_t * block, BD_t * bd)
{
	return _revision_tail_revert(block, OWNER, bd);
}

static int _revision_tail_acknowledge(bdesc_t * block, enum decider decider, void * data)
{
	chdesc_t * scan;
	chdesc_t ** chdescs;
	int i = 0, count = 0;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	/* find out how many chdescs are to be satisfied */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	chdescs = revision_get_array(count);
	if(!chdescs)
		return -ENOMEM;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(decide(decider, scan, data))
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
			dump_revision_loop_state(block, count, chdescs, __FUNCTION__);
			break;
		}
	}
	
	return 0;
}

int revision_tail_acknowledge(bdesc_t * block, BD_t * bd)
{
	int r = _revision_tail_acknowledge(block, OWNER, bd);
	if(r < 0)
		return r;
	return revision_tail_revert(block, bd);
}

#ifdef __KERNEL__
#include <lib/pool.h>
#include <linux/sched.h>

struct flight {
	bdesc_t * block;
	struct flight * next;
};
static spinlock_t flight_plan = SPIN_LOCK_UNLOCKED;
static struct flight * scheduled_flights = NULL;
static struct flight * holding_pattern = NULL;
static DECLARE_WAIT_QUEUE_HEAD(control_tower);

DECLARE_POOL(flight, struct flight);

void flight_pool_free_all(void * ignore)
{
	flight_free_all();
}

int revision_tail_schedule_flight(void)
{
	unsigned long flags;
	struct flight * slot = flight_alloc();
	if(!slot)
		return -ENOMEM;
	spin_lock_irqsave(&flight_plan, flags);
	slot->next = scheduled_flights;
	scheduled_flights = slot;
	spin_unlock_irqrestore(&flight_plan, flags);
	return 0;
}

void revision_tail_cancel_flight(void)
{
	unsigned long flags;
	struct flight * slot;
	spin_lock_irqsave(&flight_plan, flags);
	slot = scheduled_flights;
	scheduled_flights = slot->next;
	spin_unlock_irqrestore(&flight_plan, flags);
	flight_free(slot);
}

int revision_tail_flights_exist(void)
{
	unsigned long flags;
	int exist;
	spin_lock_irqsave(&flight_plan, flags);
	exist = scheduled_flights || holding_pattern;
	spin_unlock_irqrestore(&flight_plan, flags);
	return exist;
}

int revision_tail_inflight_ack(bdesc_t * block, BD_t * bd)
{
	chdesc_t * scan;
	int r;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(scan->owner == bd)
			chdesc_set_inflight(scan);
		else if(!chdesc_is_rollbackable(scan))
			fprintf(stderr, "%s(): NRB that doesn't belong to us!\n", __FUNCTION__);
	
	block->ddesc->in_flight = 1;
	bdesc_retain(block);
	
	/* FIXME: recover if we fail here */
	r = revision_tail_revert(block, bd);
	assert(r >= 0);
	return r;
}

static int revision_tail_ack_landed(bdesc_t * block)
{
	int r = _revision_tail_acknowledge(block, FLIGHT, NULL);
	assert(r >= 0);
	block->ddesc->in_flight = 0;
	bdesc_release(&block);
	return 0;
}

void revision_tail_request_landing(bdesc_t * block)
{
	unsigned long flags;
	struct flight * slot;
	spin_lock_irqsave(&flight_plan, flags);
	slot = scheduled_flights;
	scheduled_flights = slot->next;
	slot->block = block;
	slot->next = holding_pattern;
	holding_pattern = slot;
	wake_up_all(&control_tower);
	spin_unlock_irqrestore(&flight_plan, flags);
}

void revision_tail_process_landing_requests(void)
{
	unsigned long flags;
	spin_lock_irqsave(&flight_plan, flags);
	while(holding_pattern)
	{
		struct flight * slot = holding_pattern;
		holding_pattern = slot->next;
		spin_unlock_irqrestore(&flight_plan, flags);
		revision_tail_ack_landed(slot->block);
		free(slot);
		spin_lock_irqsave(&flight_plan, flags);
	}
	spin_unlock_irqrestore(&flight_plan, flags);
}

void revision_tail_wait_for_landing_requests(void)
{
	unsigned long flags;
	DEFINE_WAIT(wait);
	spin_lock_irqsave(&flight_plan, flags);
	while(!holding_pattern)
	{
		prepare_to_wait(&control_tower, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&flight_plan, flags);
		schedule();
		spin_lock_irqsave(&flight_plan, flags);
	}
	finish_wait(&control_tower, &wait);
	spin_unlock_irqrestore(&flight_plan, flags);
}
#endif /* __KERNEL__ */


/* ---- Revision slices ---- */

/* Modules don't in general know whether chdescs that they don't own are above
 * or below them. But that's OK, because they don't need to. Hence there is no
 * revision_slice_prepare() function, because modules don't need to apply or
 * roll back any chdescs to use revision slices. Basically a revision slice is a
 * set of change descriptors at a particular time, organized in a nice way so
 * that we can figure out which ones are ready to be written down and which ones
 * are not. */

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
		chdesc_unlink_index_changes(scan);
		chdesc_unlink_ready_changes(scan);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, scan, target);
		scan->owner = target;
		chdesc_propagate_level_change(scan, owner->level, target->level);
		chdesc_update_ready_changes(scan);
		chdesc_link_index_changes(scan);
	}

#if CHDESC_NRB && !CHDESC_RB_NRB_READY
	if(block->ddesc->nrb && block->ddesc->nrb->owner == owner)
		nonready_nonrollbackable = 1;
#endif

	/* TODO: instead of scanning, we could keep and read a running count in the ddesc */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(scan->owner == owner)
		{
			slice->all_ready = 0;
			break;
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
			/* it's sad that the tmp list exists solely for this error case,
			 * and it's sad that this scalloc() exists solely for pull_up. */
			for(scan = tmp_ready; scan;)
			{
				chdesc_t * next = scan->ddesc_next;
				chdesc_unlink_index_changes(scan);
				chdesc_unlink_ready_changes(scan);
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, scan, owner);
				scan->owner = owner;
				chdesc_propagate_level_change(scan, target->level, owner->level);
				unlink_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
				chdesc_update_ready_changes(scan);
				chdesc_link_index_changes(scan);
				scan = next;
			}
			
			if(nonready_nonrollbackable)
			{
				slice->ready_size = 0;
				return 0;
			}
			return -ENOMEM;
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
			chdesc_unlink_index_changes(slice->ready[i]);
			chdesc_unlink_ready_changes(slice->ready[i]);
			slice->ready[i]->owner = slice->target;
			chdesc_update_ready_changes(slice->ready[i]);
			chdesc_link_index_changes(slice->ready[i]);
			if(prev_level != chdesc_level(slice->ready[i]))
				chdesc_propagate_level_change(slice->ready[i], prev_level, chdesc_level(slice->ready[i]));
		}
		else
			fprintf(stderr, "%s(): chdesc is not owned by us, but it's in our slice...\n", __FUNCTION__);
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
			chdesc_unlink_index_changes(slice->ready[i]);
			chdesc_unlink_ready_changes(slice->ready[i]);
			slice->ready[i]->owner = slice->owner;
			chdesc_update_ready_changes(slice->ready[i]);
			chdesc_link_index_changes(slice->ready[i]);
			if(prev_level != chdesc_level(slice->ready[i]))
				chdesc_propagate_level_change(slice->ready[i], prev_level, chdesc_level(slice->ready[i]));
		}
		else
			fprintf(stderr, "%s(): chdesc is not owned by target, but it's in our slice...\n", __FUNCTION__);
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

int revision_init(void)
{
#ifdef __KERNEL__
	int r = kfsd_register_shutdown_module(flight_pool_free_all, NULL, SHUTDOWN_POSTMODULES);
	if(r < 0)
		return r;
#endif
	return 0;
}
