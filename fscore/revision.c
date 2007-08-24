#include <lib/platform.h>

#include <fscore/patch.h>
#include <fscore/modman.h>
#include <fscore/debug.h>
#include <fscore/fstitchd.h>
#include <fscore/revision.h>

enum decider {
	OWNER,
	FLIGHT
};

/* <% is equivalent to { but does not cause syntax highlighting to go nuts */
#define decide(type, patch, data) (<% int __result; \
	switch(type) <% \
		case OWNER: \
			/* it had better be either owned by us or rollbackable */ \
			assert((patch)->owner == (BD_t *) (data) || patch_is_rollbackable(patch)); \
			__result = (patch)->owner == (BD_t *) (data); \
			break; \
		case FLIGHT: \
			__result = ((patch)->flags & PATCH_INFLIGHT) != 0; \
			break; \
		default: \
			kpanic("Unknown decider type %d", type); \
	%> __result; %>)

static void dump_revision_loop_state(bdesc_t * block, int count, patch_t ** patchs, const char * function)
{
	int i;
	fprintf(stderr, "%s() is very confused! (debug = %d)\n", function, FSTITCH_DEBUG_COUNT());
	for(i = 0; i != count; i++)
	{
		patchdep_t * scan;
		int total = 0;
		if(!patchs[i])
		{
			fprintf(stderr, "(slot null)\n");
			continue;
		}
		fprintf(stderr, "%p [T%d, L%d, F%x]", patchs[i], patchs[i]->type, patch_level(patchs[i]), patchs[i]->flags);
		if(!patch_is_rollbackable(patchs[i]))
			fprintf(stderr, "!");
		fprintf(stderr, " (<-");
		for(scan = patchs[i]->afters; scan; scan = scan->after.next)
		{
			total++;
			if(!scan->after.desc->block || scan->after.desc->block->ddesc != block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->after.desc, scan->after.desc->type, scan->after.desc->flags);
			if(!patch_is_rollbackable(scan->after.desc))
				fprintf(stderr, "!");
			if(patch_overlap_check(scan->after.desc, patchs[i]))
				fprintf(stderr, "*");
			if(scan->after.desc->block->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")%d (->", total);
		total = 0;
		for(scan = patchs[i]->befores; scan; scan = scan->before.next)
		{
			total++;
			if(!scan->before.desc->block || scan->before.desc->block->ddesc != block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->before.desc, scan->before.desc->type, scan->before.desc->flags);
			if(!patch_is_rollbackable(scan->before.desc))
				fprintf(stderr, "!");
			if(patch_overlap_check(scan->before.desc, patchs[i]))
				fprintf(stderr, "*");
			if(scan->before.desc->block->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")%d (-->", total);
		for(scan = patchs[i]->befores; scan; scan = scan->before.next)
		{
			if(!scan->before.desc->block || scan->before.desc->block->ddesc == block->ddesc)
				continue;
			fprintf(stderr, " %p [%d, %x]", scan->before.desc, scan->before.desc->type, scan->before.desc->flags);
			if(!patch_is_rollbackable(scan->before.desc))
				fprintf(stderr, "!");
			if(scan->before.desc->block->in_flight)
				fprintf(stderr, "^");
		}
		fprintf(stderr, ")\n");
	}
	kpanic("too confused to continue");
}

#define REVISION_ARRAY_SIZE 80

static patch_t * revision_static_array[REVISION_ARRAY_SIZE];
static patch_t ** revision_alloc_array = NULL;
static size_t revision_alloc_array_count = 0;

static void revision_array_free(void * ignore)
{
	if(revision_alloc_array)
		sfree(revision_alloc_array, revision_alloc_array_count * sizeof(*revision_alloc_array));
	revision_alloc_array_count = 0;
}

static patch_t ** revision_get_array(size_t count)
{
	if(count <= REVISION_ARRAY_SIZE)
		return revision_static_array;
	if(count <= revision_alloc_array_count)
		return revision_alloc_array;
	if(!revision_alloc_array_count)
	{
		int r = fstitchd_register_shutdown_module(revision_array_free, NULL, SHUTDOWN_POSTMODULES);
		if(r < 0)
			return NULL;
	}
	if(revision_alloc_array)
		sfree(revision_alloc_array, revision_alloc_array_count * sizeof(*revision_alloc_array));
	revision_alloc_array_count = count;
	revision_alloc_array = smalloc(count * sizeof(*revision_alloc_array));
	return revision_alloc_array;
}

#if REVISION_TAIL_INPLACE
static int _revision_tail_prepare(bdesc_t * block, enum decider decider, void * data)
#else
static int _revision_tail_prepare(bdesc_t * block, uint8_t * buffer, enum decider decider, void * data)
#endif
{
	patch_t * scan;
	patch_t ** patchs;
	int i = 0, count = 0;
	
#if !REVISION_TAIL_INPLACE
	memcpy(buffer, bdesc_data(block), block->length);
#endif
	
	if(!block->all_patches)
		return 0;
	
	/* find out how many patchs are to be rolled back */
	/* TODO: look into using ready_patches here? */
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	patchs = revision_get_array(count);
	if(!patchs)
		return -ENOMEM;
	
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			patchs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
		for(i = 0; i != count; i++)
		{
			patchdep_t * scan;
			/* already rolled back? */
			if(patchs[i]->flags & PATCH_ROLLBACK)
				continue;
			/* check for overlapping, non-rolled back patchs above us */
			for(scan = patchs[i]->afters; scan; scan = scan->after.next)
			{
				if(scan->after.desc->flags & PATCH_ROLLBACK)
					continue;
				if(!scan->after.desc->block || scan->after.desc->block->ddesc != block->ddesc)
					continue;
				if(patch_overlap_check(scan->after.desc, patchs[i]))
					break;
			}
			if(scan)
				again = 1;
			else
			{
#if REVISION_TAIL_INPLACE
				int r = patch_rollback(patchs[i]);
#else
				int r = patch_rollback(patchs[i], buffer);
#endif
				if(r < 0)
				{
					fprintf(stderr, "patch_rollback() failed!\n");
					assert(0);
				}
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			dump_revision_loop_state(block, count, patchs, __FUNCTION__);
			break;
		}
	}
	
	return count;
}

#if REVISION_TAIL_INPLACE
int revision_tail_prepare(bdesc_t * block, BD_t * bd)
{
	assert(!block->in_flight);
	return _revision_tail_prepare(block, OWNER, bd);
}
#else
int revision_tail_prepare(bdesc_t * block, BD_t * bd, uint8_t * buffer)
{
	assert(!block->in_flight);
	return _revision_tail_prepare(block, buffer, OWNER, bd);
}
#endif

static int _revision_tail_revert(bdesc_t * block, enum decider decider, void * data)
{
	patch_t * scan;
#if REVISION_TAIL_INPLACE
	patch_t ** patchs;
	int i = 0, count = 0;
	
	if(!block->all_patches)
		return 0;
	
	/* find out how many patchs are to be rolled forward */
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	patchs = revision_get_array(count);
	if(!patchs)
		return -ENOMEM;
	
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
			patchs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
		for(i = count - 1; i != -1; i--)
		{
			patchdep_t * scan;
			/* already rolled forward? */
			if(!(patchs[i]->flags & PATCH_ROLLBACK))
				continue;
			/* check for overlapping, rolled back patchs below us */
			for(scan = patchs[i]->befores; scan; scan = scan->before.next)
			{
				if(!(scan->before.desc->flags & PATCH_ROLLBACK))
					continue;
				if(!scan->before.desc->block || scan->before.desc->block->ddesc != block->ddesc)
					continue;
				if(patch_overlap_check(scan->before.desc, patchs[i]))
					break;
			}
			if(scan)
				again = 1;
			else
			{
				int r = patch_apply(patchs[i]);
				if(r < 0)
				{
					fprintf(stderr, "patch_apply() failed!\n");
					assert(0);
				}
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			dump_revision_loop_state(block, count, patchs, __FUNCTION__);
			break;
		}
	}
#else
	int count = 0;
	
	if(!block->all_patches)
		return 0;
	
	/* we can roll them forward in any order we want, since it just marks it as rolled forward */
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(!decide(decider, scan, data))
		{
			patch_apply(scan);
			count++;
		}
#endif
	
	return count;
}

int revision_tail_revert(bdesc_t * block, BD_t * bd)
{
	return _revision_tail_revert(block, OWNER, bd);
}

static int _revision_tail_acknowledge(bdesc_t * block, enum decider decider, void * data)
{
	patch_t * scan;
	patch_t ** patchs;
	int i = 0, count = 0;
	
	if(!block->all_patches)
		return 0;
	
	/* find out how many patchs are to be satisfied */
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(decide(decider, scan, data))
			count++;
	if(!count)
		return 0;
	
	patchs = revision_get_array(count);
	if(!patchs)
		return -ENOMEM;
	
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(decide(decider, scan, data))
			patchs[i++] = scan;
	
	for(;;)
	{
		int again = 0;
		int progress = 0;
		for(i = count - 1; i != -1; i--)
		{
			if(!patchs[i])
				continue;
			if(patchs[i]->befores)
				again = 1;
			else
			{
				patch_satisfy(&patchs[i]);
				progress = 1;
			}
		}
		if(!again)
			break;
		if(!progress)
		{
			dump_revision_loop_state(block, count, patchs, __FUNCTION__);
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
	patch_t * scan;
	int r;
	
	if(!block->all_patches)
		return 0;
	
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(scan->owner == bd)
			patch_set_inflight(scan);
		else if(!patch_is_rollbackable(scan))
			fprintf(stderr, "%s(): NRB that doesn't belong to us!\n", __FUNCTION__);
	
	block->in_flight = 1;
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
	block->in_flight = 0;
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
		flight_free(slot);
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

/* Modules don't in general know whether patchs that they don't own are above
 * or below them. But that's OK, because they don't need to. Hence there is no
 * revision_slice_prepare() function, because modules don't need to apply or
 * roll back any patchs to use revision slices. Basically a revision slice is a
 * set of patchs at a particular time, organized in a nice way so
 * that we can figure out which ones are ready to be written down and which ones
 * are not. */

/* move 'patch' from its ddesc's all_patches list to the list 'tmp_ready' and preserve its all_patches neighbors its tmp list */
static void link_tmp_ready(patch_t ** tmp_ready, patch_t *** tmp_ready_tail, patch_t * patch)
{
	patch_tmpize_all_patches(patch);

	patch->ddesc_pprev = tmp_ready;
	patch->ddesc_next = *tmp_ready;
	*tmp_ready = patch;
	if(patch->ddesc_next)
		patch->ddesc_next->ddesc_pprev = &patch->ddesc_next;
	else
		*tmp_ready_tail = &patch->ddesc_next;
}

/* move 'patch' back from the list 'tmp_ready' to its ddesc's all_patches */
static void unlink_tmp_ready(patch_t ** tmp_ready, patch_t *** tmp_ready_tail, patch_t * patch)
{
	assert(patch->block && patch->owner);
	if(patch->ddesc_pprev)
	{
		if(patch->ddesc_next)
			patch->ddesc_next->ddesc_pprev = patch->ddesc_pprev;
		else
			*tmp_ready_tail = patch->ddesc_pprev;
		*patch->ddesc_pprev = patch->ddesc_next;
		patch->ddesc_next = NULL;
		patch->ddesc_pprev = NULL;
	}
	else
		assert(!patch->ddesc_next);

	patch_untmpize_all_patches(patch);
}

int revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, revision_slice_t * slice)
{
	patch_t * tmp_ready = NULL;
	patch_t ** tmp_ready_tail = &tmp_ready;
	patch_dlist_t * rcl = &block->ready_patches[owner->level];
	patch_t * scan;
	/* To write a block revision, all non-ready patchs on the block must
	 * first be rolled back. Thus when there are non-ready patchs with
	 * omitted data fields the revision cannot contain any patchs.
	 * 'nonready_nonrollbackable' implements this. */
	bool nonready_nonrollbackable = 0;

	assert(owner->level - 1 == target->level);
	
	slice->owner = owner;
	slice->target = target;
	slice->all_ready = 1;
	slice->ready_size = 0;
	slice->ready = NULL;

	/* move all the patchs down a level that can be moved down a level */
	while((scan = rcl->head))
	{
		slice->ready_size++;

		/* push down to update the ready list */
		link_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
		patch_unlink_index_patches(scan);
		patch_unlink_ready_patches(scan);
		FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_OWNER, scan, target);
		scan->owner = target;
		patch_propagate_level_change(scan, owner->level, target->level);
		patch_update_ready_patches(scan);
		patch_link_index_patches(scan);
	}

#if PATCH_NRB && !PATCH_RB_NRB_READY
	if(block->nrb && block->nrb->owner == owner)
		nonready_nonrollbackable = 1;
#endif

	/* TODO: instead of scanning, we could keep and read a running count in the ddesc */
	for(scan = block->all_patches; scan; scan = scan->ddesc_next)
		if(scan->owner == owner)
		{
			slice->all_ready = 0;
			break;
		}

	if(slice->ready_size)
	{
		patch_t * scan;
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
				patch_t * next = scan->ddesc_next;
				patch_unlink_index_patches(scan);
				patch_unlink_ready_patches(scan);
				FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_OWNER, scan, owner);
				scan->owner = owner;
				patch_propagate_level_change(scan, target->level, owner->level);
				unlink_tmp_ready(&tmp_ready, &tmp_ready_tail, scan);
				patch_update_ready_patches(scan);
				patch_link_index_patches(scan);
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
			patch_t * next = scan->ddesc_next;
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
	/* like patch_push_down, but without block reassignment (only needed
	 * for things changing block numbers) and for slices instead of all
	 * patchs: it only pushes down the ready part of the slice */
	int i;
	for(i = 0; i != slice->ready_size; i++)
	{
		if(!slice->ready[i])
			continue;
		if(slice->ready[i]->owner == slice->owner)
		{
			uint16_t prev_level = patch_level(slice->ready[i]);
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_OWNER, slice->ready[i], slice->target);
			patch_unlink_index_patches(slice->ready[i]);
			patch_unlink_ready_patches(slice->ready[i]);
			slice->ready[i]->owner = slice->target;
			patch_update_ready_patches(slice->ready[i]);
			patch_link_index_patches(slice->ready[i]);
			if(prev_level != patch_level(slice->ready[i]))
				patch_propagate_level_change(slice->ready[i], prev_level, patch_level(slice->ready[i]));
		}
		else
			fprintf(stderr, "%s(): patch is not owned by us, but it's in our slice...\n", __FUNCTION__);
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
			uint16_t prev_level = patch_level(slice->ready[i]);
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_OWNER, slice->ready[i], slice->owner);
			patch_unlink_index_patches(slice->ready[i]);
			patch_unlink_ready_patches(slice->ready[i]);
			slice->ready[i]->owner = slice->owner;
			patch_update_ready_patches(slice->ready[i]);
			patch_link_index_patches(slice->ready[i]);
			if(prev_level != patch_level(slice->ready[i]))
				patch_propagate_level_change(slice->ready[i], prev_level, patch_level(slice->ready[i]));
		}
		else
			fprintf(stderr, "%s(): patch is not owned by target, but it's in our slice...\n", __FUNCTION__);
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
	int r = fstitchd_register_shutdown_module(flight_pool_free_all, NULL, SHUTDOWN_POSTMODULES);
	if(r < 0)
		return r;
#endif
	return 0;
}
