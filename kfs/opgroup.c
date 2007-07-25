#include <lib/platform.h>
#include <lib/hash_map.h>

#include <kfs/debug.h>
#include <kfs/sync.h>
#include <kfs/journal_bd.h>
#include <kfs/opgroup.h>

#define OPGROUP_DEBUG 0

#if OPGROUP_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* Atomic opgroup TODOs:
 *
 * Correctness:
 * - detect that a journal is present for the filesystems used by an opgroup
 * - detect cyclic dependencies among opgroup transactions (chdesc update)
 *   and block the second opgroup transaction
 * - support multi-device transactions
 *
 * Performance:
 * - only add holds to the needed journal_bds
 * - make dependencies on an opgroup transaction depend on the commit record
 */

/* TODO: describe big picture re why chdesc_add_depend() usage is safe */

struct opgroup {
	opgroup_id_t id;
	chweakref_t head;
	/* head_keep stays until we get an after */
	chdesc_t * head_keep;
	chweakref_t tail;
	/* tail_keep stays until we are released */
	chdesc_t * tail_keep;
	uint32_t references:30;
	/* has_data is set when we engage, not when we actually get data */
	uint32_t has_data:1;
	uint32_t is_released:1;
	uint32_t engaged_count:30;
	uint32_t has_afters:1;
	uint32_t has_befores:1;
	int flags;
};

typedef struct opgroup_state {
	opgroup_t * opgroup;
	int engaged;
} opgroup_state_t;

struct opgroup_scope {
	opgroup_id_t next_id;
	/* map from ID to opgroup state */
	hash_map_t * id_map;
	chdesc_t * top;
	/* top_keep stays until we change the engaged set */
	chdesc_t * top_keep;
	chweakref_t bottom;
	int engaged_count;
};

/* Do not allow multiple atomic opgroups to exist at a single point in time
 * for now. Soon we will detect inter-atomic opgroup dependencies and remove
 * this restriction.
 */
static bool atomic_opgroup_exists = 0;

static opgroup_scope_t * current_scope = NULL;

opgroup_scope_t * opgroup_scope_create(void)
{
	opgroup_scope_t * scope = malloc(sizeof(*scope));
	if(scope)
	{
		Dprintf("%s(): scope = %p, debug = %d\n", __FUNCTION__, scope, KFS_DEBUG_COUNT());
		scope->next_id = 1;
		scope->top = NULL;
		scope->top_keep = NULL;
		WEAK_INIT(scope->bottom);
		scope->engaged_count = 0;
		scope->id_map = hash_map_create();
		if(!scope->id_map)
		{
			free(scope);
			scope = NULL;
		}
	}
	return scope;
}

opgroup_scope_t * opgroup_scope_copy(opgroup_scope_t * scope)
{
	hash_map_it_t it;
	opgroup_state_t * state;
	opgroup_scope_t * copy = opgroup_scope_create();
	if(!copy)
		return NULL;
	Dprintf("%s(): scope = %p, copy = %p, debug = %d\n", __FUNCTION__, scope, copy, KFS_DEBUG_COUNT());
	
	copy->next_id = scope->next_id;
	if(scope->top)
	{
		/* we need our own top_keep */
		if(chdesc_create_noop_list(NULL, &copy->top_keep, NULL) < 0)
			goto error_copy;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, copy->top_keep, "top_keep");
		chdesc_claim_noop(copy->top_keep);
		if(chdesc_create_noop_list(NULL, &copy->top, copy->top_keep, NULL) < 0)
			goto error_top_keep;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, copy->top, "top");
	}
	chdesc_weak_retain(WEAK(scope->bottom), &copy->bottom, NULL, NULL);
	
	/* iterate over opgroups and increase reference counts */
	hash_map_it_init(&it, scope->id_map);
	while((state = hash_map_val_next(&it)))
	{
		opgroup_state_t * dup = malloc(sizeof(*dup));
		if(!dup)
			goto error_bottom;
		*dup = *state;
		if(hash_map_insert(copy->id_map, (void *) dup->opgroup->id, dup) < 0)
		{
			free(dup);
			goto error_bottom;
		}
		dup->opgroup->references++;
		/* FIXME: can we do better than just assert? */
		assert(dup->opgroup->references);
		if(dup->engaged)
		{
			dup->opgroup->engaged_count++;
			/* FIXME: can we do better than just assert? */
			assert(dup->opgroup->engaged_count);
			copy->engaged_count++;
		}
	}
	assert(copy->engaged_count == scope->engaged_count);
	
	return copy;
	
  error_bottom:
	hash_map_it_init(&it, copy->id_map);
	while((state = hash_map_val_next(&it)))
	{
		/* don't need to check for 0 */
		state->opgroup->references--;
		if(state->engaged)
			state->opgroup->engaged_count--;
		free(state);
	}
	hash_map_destroy(copy->id_map);
	
	chdesc_weak_release(&copy->bottom, 0);
  error_top_keep:
	if(copy->top_keep)
		chdesc_satisfy(&copy->top_keep);	
  error_copy:
	free(copy);
	return NULL;
}

size_t opgroup_scope_size(opgroup_scope_t * scope)
{
	return hash_map_size(scope->id_map);
}

void opgroup_scope_destroy(opgroup_scope_t * scope)
{
	Dprintf("%s(): scope = %p, debug = %d\n", __FUNCTION__, scope, KFS_DEBUG_COUNT());
	hash_map_it2_t it = hash_map_it2_create(scope->id_map);
	opgroup_scope_t * old_scope = current_scope;
	
	/* opgroup_abandon() needs the current scope
	 * to be the one we are destroying... */
	current_scope = scope;
	
	/* iterate over opgroups and abandon them */
	while(hash_map_it2_next(&it))
	{
		opgroup_state_t * state = it.val;
		int r = opgroup_disengage(state->opgroup);
		assert(r >= 0);
		opgroup_abandon(&state->opgroup);
	}
	hash_map_destroy(scope->id_map);
	
	/* restore the current scope (unless it was destroyed) */
	current_scope = (old_scope == scope) ? NULL : old_scope;
	
	if(scope->top_keep)
		chdesc_satisfy(&scope->top_keep);
	chdesc_weak_release(&scope->bottom, 0);
	free(scope);
}

void opgroup_scope_set_current(opgroup_scope_t * scope)
{
	current_scope = scope;
}

opgroup_t * opgroup_create(int flags)
{
	opgroup_t * op;
	opgroup_state_t * state;
	chdesc_t * tail;
	chdesc_t * head;
	
	if(!current_scope)
		return NULL;
	if(!(!flags || flags == OPGROUP_FLAG_ATOMIC))
		return NULL;

	if(flags & OPGROUP_FLAG_ATOMIC)
	{
		if(atomic_opgroup_exists)
			return NULL;
		atomic_opgroup_exists = 1;
	}
	
	if(!(op = malloc(sizeof(*op))))
		return NULL;
	if(!(state = malloc(sizeof(*state))))
		goto error_op;
	Dprintf("%s(): opgroup = %p, debug = %d\n", __FUNCTION__, op, KFS_DEBUG_COUNT());
	
	op->id = current_scope->next_id++;
	WEAK_INIT(op->head);
	WEAK_INIT(op->tail);
	op->references = 1;
	op->has_data = 0;
	op->is_released = 0;
	op->engaged_count = 0;
	op->has_afters = 0;
	op->has_befores = 0;
	op->flags = flags;
	state->opgroup = op;
	state->engaged = 0;
	
	if(chdesc_create_noop_list(NULL, &op->head_keep, NULL) < 0)
		goto error_state;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, op->head_keep, "head_keep");
	chdesc_claim_noop(op->head_keep);
	
	if(chdesc_create_noop_list(NULL, &op->tail_keep, NULL) < 0)
		goto error_head_keep;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, op->tail_keep, "tail_keep");
	chdesc_claim_noop(op->tail_keep);
	
	if(chdesc_create_noop_list(NULL, &tail, op->tail_keep, NULL) < 0)
		goto error_tail_keep;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, tail, "tail");
	chdesc_weak_retain(tail, &op->tail, NULL, NULL);
	
	if(chdesc_create_noop_list(NULL, &head, op->head_keep, NULL) < 0)
		goto error_tail;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "head");
	chdesc_weak_retain(head, &op->head, NULL, NULL);
	
	if(hash_map_insert(current_scope->id_map, (void *) op->id, state) < 0)
		goto error_head;
	
	return op;
	
error_head:
	chdesc_remove_depend(head, op->head_keep);
	chdesc_destroy(&head);
error_tail:
	chdesc_remove_depend(tail, op->tail_keep);
	chdesc_destroy(&tail);
error_tail_keep:
	chdesc_destroy(&op->tail_keep);
error_head_keep:
	chdesc_destroy(&op->head_keep);
error_state:
	free(state);
error_op:
	free(op);
	return NULL;
}

int opgroup_sync(opgroup_t * opgroup)
{
	// TODO: sync just the needed opgroups
	return kfs_sync();
}

int opgroup_add_depend(opgroup_t * after, opgroup_t * before)
{
	int r = 0;
	if(!after || !before)
		return -EINVAL;
	/* from before's perspective, we are adding an after
	 *   => before must not be engaged [anywhere] if it is not atomic */
	if(!(before->flags & OPGROUP_FLAG_ATOMIC) && before->engaged_count)
		return -EBUSY;
	/* from after's perspective, we are adding a before
	 *   => after must not be released (standard case) or have an after (noop case) */
	assert(!after->tail_keep == after->is_released);
	if(after->is_released || after->has_afters)
		return -EINVAL;
	Dprintf("%s(): after = %p -> before = %p, debug = %d\n", __FUNCTION__, after, before, KFS_DEBUG_COUNT());
	/* we only create head => tail directly if we need to: when we are adding
	 * an after to an opgroup and it still has both its head and tail */
	if(WEAK(before->head) && WEAK(before->tail))
	{
		/* for efficiency, when that head and tail are not already connected
		 * transitively: that is, head has only head_keep as a before */
		if(WEAK(before->head)->befores && !WEAK(before->head)->befores->before.next &&
		   WEAK(before->head)->befores->before.desc == before->head_keep)
		{
			r = chdesc_add_depend(WEAK(before->head), WEAK(before->tail));
			if(r < 0)
				return r;
		}
	}
	/* it might not have a head if it's already been written to disk */
	/* (in this case, it won't be engaged again since it will have
	 * afters now, so we don't need to recreate it) */
	if(WEAK(before->head))
	{
#ifdef YOU_LIKE_INCORRECT_OPTIMIZATIONS
		if(!WEAK(after->tail)->befores->before.next)
		{
			/* this is the first before we are adding to after,
			 * so we can inherit before's head as our tail */
			assert(WEAK(after->tail)->befores->before.desc == after->tail_keep);
			assert(!WEAK(after->tail)->afters);
			r = chdesc_add_depend(WEAK(before->head), after->tail_keep);
			if(r >= 0)
			{
				chdesc_remove_depend(WEAK(after->tail), after->tail_keep);
				chdesc_weak_release(&after->tail, 0);
				chdesc_weak_retain(WEAK(before->head), &after->tail, NULL, NULL);
				KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, WEAK(after->tail), "tail");
			}
			else
				goto oh_well;
		}
		else
			oh_well:
#endif
			/* notice that this can fail if there is a before cycle */
			r = chdesc_add_depend(WEAK(after->tail), WEAK(before->head));
	}
	if(r >= 0)
	{
		after->has_befores = 1;
		before->has_afters = 1;
		if(before->head_keep)
			chdesc_satisfy(&before->head_keep);
	}
	else
		fprintf(stderr, "%s: chdesc_add_depend() unexpectedly failed (%i)\n", __FUNCTION__, r);
	return r;
}

static int opgroup_update_top_bottom(const opgroup_state_t * changed_state, bool was_engaged)
{
	hash_map_it_t it;
	opgroup_state_t * state = NULL;
	chdesc_t * top;
	chdesc_t * top_keep;
	chdesc_t * bottom;
	chdesc_t * save_top = current_scope->top;
	int r, count = 0;
	Dprintf("%s(): start updating, debug = %d\n", __FUNCTION__, KFS_DEBUG_COUNT());
	
	/* when top has only top_keep as a before, then don't bother attaching any heads to it */
	if(save_top && (save_top->befores->before.next ||
	   save_top->befores->before.desc != current_scope->top_keep))
	{
		/* attach heads to top only when done with top so
		 * that top can gain befores along the way */
		hash_map_it_init(&it, current_scope->id_map);
		while((state = hash_map_val_next(&it)))
			if(save_top && ((state == changed_state) ? was_engaged : state->engaged))
			{
				assert(WEAK(state->opgroup->head) && state->opgroup->head_keep);
				if(!WEAK(state->opgroup->head)->befores->before.next)
				{
					/* this is the first top we are adding to head,
					 * so we can inherit this top as our head */
					assert(WEAK(state->opgroup->head)->befores->before.desc == state->opgroup->head_keep);
					assert(!WEAK(state->opgroup->head)->afters);
					r = chdesc_add_depend(save_top, state->opgroup->head_keep);
					if(r >= 0)
					{
						chdesc_remove_depend(WEAK(state->opgroup->head), state->opgroup->head_keep);
						chdesc_weak_release(&state->opgroup->head, 0);
						chdesc_weak_retain(save_top, &state->opgroup->head, NULL, NULL);
						KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, WEAK(state->opgroup->head), "head");
					}
					else
						goto oh_well;
				}
				else
				{
					oh_well:
					r = chdesc_add_depend(WEAK(state->opgroup->head), save_top);
					if(r < 0)
						kpanic("Can't recover from failure!");
				}
			}
	}
	
	/* create new top and bottom */
	r = chdesc_create_noop_list(NULL, &top_keep, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, top_keep, "top_keep");
	chdesc_claim_noop(top_keep);
	
	r = chdesc_create_noop_list(NULL, &bottom, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, bottom, "bottom");
	
	hash_map_it_init(&it, current_scope->id_map);
	while((state = hash_map_val_next(&it)))
		if(state->engaged)
		{
			if(WEAK(state->opgroup->tail))
				if(chdesc_add_depend(bottom, WEAK(state->opgroup->tail)) < 0)
					kpanic("Can't recover from failure!");
			count++;
		}
	
	r = chdesc_create_noop_list(NULL, &top, top_keep, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, top, "top");
	
	if(!bottom->befores)
	{
		/* let it get garbage collected */
		bottom = NULL;
	}
	else if(!bottom->befores->before.next)
	{
		/* only one tail; inherit it for bottom! */
		chdesc_t * old = bottom;
		bottom = bottom->befores->before.desc;
		chdesc_remove_depend(old, bottom);
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, bottom, "bottom");
	}
	
	chdesc_weak_retain(bottom, &current_scope->bottom, NULL, NULL);
	
	if(!count)
	{
		chdesc_satisfy(&top_keep);
		top = NULL;
	}
	
	current_scope->top = top;
	if(current_scope->top_keep)
		chdesc_satisfy(&current_scope->top_keep);
	/* we claimed it so no need to weak retain */
	current_scope->top_keep = top_keep;
	Dprintf("%s(): finished updating, debug = %d\n", __FUNCTION__, KFS_DEBUG_COUNT());
	
	return 0;
}

int opgroup_engage(opgroup_t * opgroup)
{
	int r;
	opgroup_state_t * state;
	
	if(!current_scope)
		return -ENODEV;
	if(!opgroup)
		return -EINVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -ENOENT;
	assert(state->opgroup == opgroup);
	if(!(opgroup->flags & OPGROUP_FLAG_ATOMIC) && (!opgroup->is_released || !opgroup->is_released))
		return -EINVAL;
	/* can't engage it if it is not atomic and it has afters */
	if(!(opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->has_afters)
		return -EINVAL;
	/* can't engage it if it is atomic and has been released */
	if((opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->is_released)
		return -EINVAL;
	if(state->engaged)
		return 0;
	Dprintf("%s(): opgroup = %p, debug = %d\n", __FUNCTION__, opgroup, KFS_DEBUG_COUNT());
	
	state->engaged = 1;
	opgroup->engaged_count++;
	/* FIXME: can we do better than just assert? */
	assert(state->opgroup->engaged_count);
	current_scope->engaged_count++;
	
	r = opgroup_update_top_bottom(state, 0);
	if(r < 0)
	{
		state->engaged = 0;
		opgroup->engaged_count--;
		current_scope->engaged_count--;
	}
	else
	{
		if((opgroup->flags & OPGROUP_FLAG_ATOMIC) && !opgroup->has_data)
			journal_bd_add_hold();
		/* mark it as having data since it is now engaged */
		/* (and therefore could acquire data at any time) */
		opgroup->has_data = 1;
	}

	return r;
}

int opgroup_disengage(opgroup_t * opgroup)
{
	int r;
	opgroup_state_t * state;
	
	if(!current_scope)
		return -ENODEV;
	if(!opgroup)
		return -EINVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -ENOENT;
	assert(state->opgroup == opgroup);
	if(!state->engaged)
		return 0;
	Dprintf("%s(): opgroup = %p, debug = %d\n", __FUNCTION__, opgroup, KFS_DEBUG_COUNT());
	
	state->engaged = 0;
	opgroup->engaged_count--;
	current_scope->engaged_count--;
	
	r = opgroup_update_top_bottom(state, 1);
	if(r < 0)
	{
		state->engaged = 1;
		opgroup->engaged_count++;
		current_scope->engaged_count++;
	}

	return r;
}

int opgroup_release(opgroup_t * opgroup)
{
	if(!opgroup)
		return -EINVAL;
	/* can't release atomic opgroup if it is engaged */
	if((opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->engaged_count)
		return -EINVAL;
	Dprintf("%s(): opgroup = %p, debug = %d\n", __FUNCTION__, opgroup, KFS_DEBUG_COUNT());
	if(opgroup->tail_keep)
	{
		chdesc_satisfy(&opgroup->tail_keep);
		if(opgroup->flags & OPGROUP_FLAG_ATOMIC)
			journal_bd_remove_hold();
		opgroup->is_released = 1;
	}
	return 0;
}

int opgroup_abandon(opgroup_t ** opgroup)
{
	opgroup_state_t * state;
	if(!current_scope)
		return -ENODEV;
	if(!opgroup || !*opgroup)
		return -EINVAL;
	state = hash_map_erase(current_scope->id_map, (void *) (*opgroup)->id);
	if(!state)
		return -ENOENT;
	assert(state->opgroup == *opgroup);
	/* can't abandon a non-released atomic opgroup */
	if(((*opgroup)->flags & OPGROUP_FLAG_ATOMIC) && !(*opgroup)->is_released)
		return -EINVAL;
	/* can't abandon an engaged opgroup */
	if(state->engaged)
		return -EBUSY;
	Dprintf("%s(): opgroup = %p, debug = %d\n", __FUNCTION__, *opgroup, KFS_DEBUG_COUNT());
	if(!--state->opgroup->references)
	{
		if((*opgroup)->flags & OPGROUP_FLAG_ATOMIC)
		{
			assert(atomic_opgroup_exists);
			atomic_opgroup_exists = 0;
		}

		/* no more references to this opgroup */
		if(state->opgroup->tail_keep || !state->opgroup->is_released)
		{
			if(!state->opgroup->has_data)
				opgroup_release(state->opgroup);
			else
				kpanic("Don't know how to roll back an abandoned opgroup!");
		}
		if(state->opgroup->head_keep)
			chdesc_satisfy(&state->opgroup->head_keep);
		chdesc_weak_release(&state->opgroup->head, 0);
		chdesc_weak_release(&state->opgroup->tail, 0);
		free(state->opgroup);
	}

	/* opgroup_scope_destroy() passes us *opgroup inside state... */
	*opgroup = NULL;
	free(state);
	return 0;
}

opgroup_t * opgroup_lookup(opgroup_id_t id)
{
	opgroup_state_t * state;
	if(!current_scope)
		return NULL;
	state = (opgroup_state_t *) hash_map_find_val(current_scope->id_map, (void *) id);
	return state ? state->opgroup : NULL;
}

opgroup_id_t opgroup_id(const opgroup_t * opgroup)
{
	if(!opgroup)
		return -EINVAL;
	return opgroup->id;
}

int opgroup_engaged(void)
{
	return current_scope && current_scope->engaged_count;
}

int opgroup_prepare_head(chdesc_t ** head)
{
	if(!current_scope || !WEAK(current_scope->bottom))
		return 0;
	
	if(*head)
	{
		int r;
		/* heuristic: does *head already depend on bottom as the first dependency? */
		if((*head)->befores && (*head)->befores->before.desc == WEAK(current_scope->bottom))
			return 0;
		/* heuristic: does bottom already depend on *head as the first dependency? */
		if(WEAK(current_scope->bottom)->befores && WEAK(current_scope->bottom)->befores->before.desc == *head)
		{
			*head = WEAK(current_scope->bottom);
			return 0;
		}
		r = chdesc_create_noop_list(NULL, head, WEAK(current_scope->bottom), *head, NULL);
		if(r < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "and");
	}
	else
		*head = WEAK(current_scope->bottom);
	
	return 0;
}

int opgroup_finish_head(chdesc_t * head)
{
	if(!current_scope || !current_scope->top || !head || head == WEAK(current_scope->bottom))
		return 0;
	return chdesc_add_depend(current_scope->top, head);
}

int opgroup_label(opgroup_t * opgroup, const char * label)
{
	if(!opgroup)
		return -EINVAL;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, opgroup->head, "og head: %s", label);
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, opgroup->tail, "og tail: %s", label);
	return 0;
}
