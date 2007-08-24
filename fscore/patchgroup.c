#include <lib/platform.h>
#include <lib/hash_map.h>

#include <fscore/debug.h>
#include <fscore/sync.h>
#include <fscore/journal_bd.h>
#include <fscore/patchgroup.h>

#define PATCHGROUP_DEBUG 0

#if PATCHGROUP_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* Atomic patchgroup TODOs:
 *
 * Correctness:
 * - detect that a journal is present for the filesystems used by an patchgroup
 * - detect cyclic dependencies among patchgroup transactions (patch update)
 *   and block the second patchgroup transaction
 * - support multi-device transactions
 *
 * Performance:
 * - only add holds to the needed journal_bds
 * - make dependencies on an patchgroup transaction depend on the commit record
 */

/* TODO: describe big picture re why patch_add_depend() usage is safe */

struct patchgroup {
	patchgroup_id_t id;
	chweakref_t head;
	/* head_keep stays until we get an after */
	patch_t * head_keep;
	chweakref_t tail;
	/* tail_keep stays until we are released */
	patch_t * tail_keep;
	uint32_t references:30;
	/* has_data is set when we engage, not when we actually get data */
	uint32_t has_data:1;
	uint32_t is_released:1;
	uint32_t engaged_count:30;
	uint32_t has_afters:1;
	uint32_t has_befores:1;
	int flags;
};

typedef struct patchgroup_state {
	patchgroup_t * patchgroup;
	int engaged;
} patchgroup_state_t;

struct patchgroup_scope {
	patchgroup_id_t next_id;
	/* map from ID to patchgroup state */
	hash_map_t * id_map;
	patch_t * top;
	/* top_keep stays until we change the engaged set */
	patch_t * top_keep;
	chweakref_t bottom;
	int engaged_count;
};

/* Do not allow multiple atomic patchgroups to exist at a single point in time
 * for now. Soon we will detect inter-atomic patchgroup dependencies and remove
 * this restriction.
 */
static bool atomic_patchgroup_exists = 0;

static patchgroup_scope_t * current_scope = NULL;
static int masquerade_count = 0;

patchgroup_scope_t * patchgroup_scope_create(void)
{
	patchgroup_scope_t * scope = malloc(sizeof(*scope));
	if(scope)
	{
		Dprintf("%s(): scope = %p, debug = %d\n", __FUNCTION__, scope, FSTITCH_DEBUG_COUNT());
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

patchgroup_scope_t * patchgroup_scope_copy(patchgroup_scope_t * scope)
{
	hash_map_it_t it;
	patchgroup_state_t * state;
	patchgroup_scope_t * copy = patchgroup_scope_create();
	if(!copy)
		return NULL;
	Dprintf("%s(): scope = %p, copy = %p, debug = %d\n", __FUNCTION__, scope, copy, FSTITCH_DEBUG_COUNT());
	
	copy->next_id = scope->next_id;
	if(scope->top)
	{
		/* we need our own top_keep */
		if(patch_create_noop_list(NULL, &copy->top_keep, NULL) < 0)
			goto error_copy;
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, copy->top_keep, "top_keep");
		patch_claim_noop(copy->top_keep);
		if(patch_create_noop_list(NULL, &copy->top, copy->top_keep, NULL) < 0)
			goto error_top_keep;
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, copy->top, "top");
		copy->top->flags |= PATCH_NO_PATCHGROUP;
		FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, copy->top, PATCH_NO_PATCHGROUP);
	}
	patch_weak_retain(WEAK(scope->bottom), &copy->bottom, NULL, NULL);
	
	/* iterate over patchgroups and increase reference counts */
	hash_map_it_init(&it, scope->id_map);
	while((state = hash_map_val_next(&it)))
	{
		patchgroup_state_t * dup = malloc(sizeof(*dup));
		if(!dup)
			goto error_bottom;
		*dup = *state;
		if(hash_map_insert(copy->id_map, (void *) dup->patchgroup->id, dup) < 0)
		{
			free(dup);
			goto error_bottom;
		}
		dup->patchgroup->references++;
		/* FIXME: can we do better than just assert? */
		assert(dup->patchgroup->references);
		if(dup->engaged)
		{
			dup->patchgroup->engaged_count++;
			/* FIXME: can we do better than just assert? */
			assert(dup->patchgroup->engaged_count);
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
		state->patchgroup->references--;
		if(state->engaged)
			state->patchgroup->engaged_count--;
		free(state);
	}
	hash_map_destroy(copy->id_map);
	
	patch_weak_release(&copy->bottom, 0);
  error_top_keep:
	if(copy->top_keep)
		patch_satisfy(&copy->top_keep);	
  error_copy:
	free(copy);
	return NULL;
}

size_t patchgroup_scope_size(patchgroup_scope_t * scope)
{
	return hash_map_size(scope->id_map);
}

void patchgroup_scope_destroy(patchgroup_scope_t * scope)
{
	Dprintf("%s(): scope = %p, debug = %d\n", __FUNCTION__, scope, FSTITCH_DEBUG_COUNT());
	hash_map_it2_t it = hash_map_it2_create(scope->id_map);
	patchgroup_scope_t * old_scope = current_scope;
	
	/* patchgroup_abandon() needs the current scope
	 * to be the one we are destroying... */
	current_scope = scope;
	
	/* iterate over patchgroups and abandon them */
	while(hash_map_it2_next(&it))
	{
		patchgroup_state_t * state = it.val;
		int r = patchgroup_disengage(state->patchgroup);
		assert(r >= 0);
		patchgroup_abandon(&state->patchgroup);
	}
	hash_map_destroy(scope->id_map);
	
	/* restore the current scope (unless it was destroyed) */
	current_scope = (old_scope == scope) ? NULL : old_scope;
	
	if(scope->top_keep)
		patch_satisfy(&scope->top_keep);
	patch_weak_release(&scope->bottom, 0);
	free(scope);
}

void patchgroup_scope_set_current(patchgroup_scope_t * scope)
{
	current_scope = scope;
}

patchgroup_t * patchgroup_create(int flags)
{
	patchgroup_t * op;
	patchgroup_state_t * state;
	patch_t * tail;
	patch_t * head;
	
	if(!current_scope)
		return NULL;
	if(!(!flags || flags == PATCHGROUP_FLAG_ATOMIC))
		return NULL;

	if(flags & PATCHGROUP_FLAG_ATOMIC)
	{
		if(atomic_patchgroup_exists)
			return NULL;
		atomic_patchgroup_exists = 1;
	}
	
	if(!(op = malloc(sizeof(*op))))
		return NULL;
	if(!(state = malloc(sizeof(*state))))
		goto error_op;
	Dprintf("%s(): patchgroup = %p, debug = %d\n", __FUNCTION__, op, FSTITCH_DEBUG_COUNT());
	
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
	state->patchgroup = op;
	state->engaged = 0;
	
	if(patch_create_noop_list(NULL, &op->head_keep, NULL) < 0)
		goto error_state;
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, op->head_keep, "head_keep");
	patch_claim_noop(op->head_keep);
	
	if(patch_create_noop_list(NULL, &op->tail_keep, NULL) < 0)
		goto error_head_keep;
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, op->tail_keep, "tail_keep");
	patch_claim_noop(op->tail_keep);
	
	if(patch_create_noop_list(NULL, &tail, op->tail_keep, NULL) < 0)
		goto error_tail_keep;
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, tail, "tail");
	patch_weak_retain(tail, &op->tail, NULL, NULL);
	
	if(patch_create_noop_list(NULL, &head, op->head_keep, NULL) < 0)
		goto error_tail;
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, head, "head");
	patch_weak_retain(head, &op->head, NULL, NULL);
	
	if(hash_map_insert(current_scope->id_map, (void *) op->id, state) < 0)
		goto error_head;
	
	return op;
	
error_head:
	patch_remove_depend(head, op->head_keep);
	patch_destroy(&head);
error_tail:
	patch_remove_depend(tail, op->tail_keep);
	patch_destroy(&tail);
error_tail_keep:
	patch_destroy(&op->tail_keep);
error_head_keep:
	patch_destroy(&op->head_keep);
error_state:
	free(state);
error_op:
	free(op);
	return NULL;
}

int patchgroup_sync(patchgroup_t * patchgroup)
{
	// TODO: sync just the needed patchgroups
	return fstitch_sync();
}

int patchgroup_add_depend(patchgroup_t * after, patchgroup_t * before)
{
	int r = 0;
	if(!after || !before)
		return -EINVAL;
	/* from before's perspective, we are adding an after
	 *   => before must not be engaged [anywhere] if it is not atomic */
	if(!(before->flags & PATCHGROUP_FLAG_ATOMIC) && before->engaged_count)
		return -EBUSY;
	/* from after's perspective, we are adding a before
	 *   => after must not be released (standard case) or have an after (noop case) */
	assert(!after->tail_keep == after->is_released);
	if(after->is_released || after->has_afters)
		return -EINVAL;
	Dprintf("%s(): after = %p -> before = %p, debug = %d\n", __FUNCTION__, after, before, FSTITCH_DEBUG_COUNT());
	/* we only create head => tail directly if we need to: when we are adding
	 * an after to an patchgroup and it still has both its head and tail */
	if(WEAK(before->head) && WEAK(before->tail))
	{
		/* for efficiency, when that head and tail are not already connected
		 * transitively: that is, head has only head_keep as a before */
		if(WEAK(before->head)->befores && !WEAK(before->head)->befores->before.next &&
		   WEAK(before->head)->befores->before.desc == before->head_keep)
		{
			r = patch_add_depend(WEAK(before->head), WEAK(before->tail));
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
			r = patch_add_depend(WEAK(before->head), after->tail_keep);
			if(r >= 0)
			{
				patch_remove_depend(WEAK(after->tail), after->tail_keep);
				patch_weak_release(&after->tail, 0);
				patch_weak_retain(WEAK(before->head), &after->tail, NULL, NULL);
				FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, WEAK(after->tail), "tail");
			}
			else
				goto oh_well;
		}
		else
			oh_well:
#endif
			/* notice that this can fail if there is a before cycle */
			r = patch_add_depend(WEAK(after->tail), WEAK(before->head));
	}
	if(r >= 0)
	{
		after->has_befores = 1;
		before->has_afters = 1;
		if(before->head_keep)
			patch_satisfy(&before->head_keep);
	}
	else
		fprintf(stderr, "%s: patch_add_depend() unexpectedly failed (%i)\n", __FUNCTION__, r);
	return r;
}

static int patchgroup_update_top_bottom(const patchgroup_state_t * changed_state, bool was_engaged)
{
	hash_map_it_t it;
	patchgroup_state_t * state = NULL;
	patch_t * top;
	patch_t * top_keep;
	patch_t * bottom;
	patch_t * save_top = current_scope->top;
	int r, count = 0;
	Dprintf("%s(): start updating, debug = %d\n", __FUNCTION__, FSTITCH_DEBUG_COUNT());
	
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
				assert(WEAK(state->patchgroup->head) && state->patchgroup->head_keep);
#ifdef YOU_LIKE_INCORRECT_OPTIMIZATIONS
				if(!WEAK(state->patchgroup->head)->befores->before.next)
				{
					/* this is the first top we are adding to head,
					 * so we can inherit this top as our head */
					assert(WEAK(state->patchgroup->head)->befores->before.desc == state->patchgroup->head_keep);
					assert(!WEAK(state->patchgroup->head)->afters);
					r = patch_add_depend(save_top, state->patchgroup->head_keep);
					if(r >= 0)
					{
						patch_remove_depend(WEAK(state->patchgroup->head), state->patchgroup->head_keep);
						patch_weak_release(&state->patchgroup->head, 0);
						patch_weak_retain(save_top, &state->patchgroup->head, NULL, NULL);
						FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, WEAK(state->patchgroup->head), "head");
					}
					else
						goto oh_well;
				}
				else
				{
					oh_well:
#endif
					r = patch_add_depend(WEAK(state->patchgroup->head), save_top);
					if(r < 0)
						kpanic("Can't recover from failure!");
#ifdef YOU_LIKE_INCORRECT_OPTIMIZATIONS
				}
#endif
			}
	}
	
	/* create new top and bottom */
	r = patch_create_noop_list(NULL, &top_keep, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, top_keep, "top_keep");
	patch_claim_noop(top_keep);
	
	r = patch_create_noop_list(NULL, &bottom, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, bottom, "bottom");
	
	hash_map_it_init(&it, current_scope->id_map);
	while((state = hash_map_val_next(&it)))
		if(state->engaged)
		{
			if(WEAK(state->patchgroup->tail))
				if(patch_add_depend(bottom, WEAK(state->patchgroup->tail)) < 0)
					kpanic("Can't recover from failure!");
			count++;
		}
	
	r = patch_create_noop_list(NULL, &top, top_keep, NULL);
	if(r < 0)
		kpanic("Can't recover from failure!");
	FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, top, "top");
	top->flags |= PATCH_NO_PATCHGROUP;
	FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, top, PATCH_NO_PATCHGROUP);
	
	if(!bottom->befores)
	{
		/* let it get garbage collected */
		bottom = NULL;
	}
#ifdef YOU_LIKE_INCORRECT_OPTIMIZATIONS
	else if(!bottom->befores->before.next)
	{
		/* only one tail; inherit it for bottom! */
		patch_t * old = bottom;
		bottom = bottom->befores->before.desc;
		patch_remove_depend(old, bottom);
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, bottom, "bottom");
	}
#endif
	
	patch_weak_retain(bottom, &current_scope->bottom, NULL, NULL);
	
	if(!count)
	{
		patch_satisfy(&top_keep);
		top = NULL;
	}
	
	current_scope->top = top;
	if(current_scope->top_keep)
		patch_satisfy(&current_scope->top_keep);
	/* we claimed it so no need to weak retain */
	current_scope->top_keep = top_keep;
	Dprintf("%s(): finished updating, debug = %d\n", __FUNCTION__, FSTITCH_DEBUG_COUNT());
	
	return 0;
}

int patchgroup_engage(patchgroup_t * patchgroup)
{
	int r;
	patchgroup_state_t * state;
	
	if(!current_scope)
		return -ENODEV;
	if(!patchgroup)
		return -EINVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) patchgroup->id);
	if(!state)
		return -ENOENT;
	assert(state->patchgroup == patchgroup);
	if(!(patchgroup->flags & PATCHGROUP_FLAG_ATOMIC) && (!patchgroup->is_released || !patchgroup->is_released))
		return -EINVAL;
	/* can't engage it if it is not atomic and it has afters */
	if(!(patchgroup->flags & PATCHGROUP_FLAG_ATOMIC) && patchgroup->has_afters)
		return -EINVAL;
	/* can't engage it if it is atomic and has been released */
	if((patchgroup->flags & PATCHGROUP_FLAG_ATOMIC) && patchgroup->is_released)
		return -EINVAL;
	if(state->engaged)
		return 0;
	Dprintf("%s(): patchgroup = %p, debug = %d\n", __FUNCTION__, patchgroup, FSTITCH_DEBUG_COUNT());
	
	state->engaged = 1;
	patchgroup->engaged_count++;
	/* FIXME: can we do better than just assert? */
	assert(state->patchgroup->engaged_count);
	current_scope->engaged_count++;
	
	r = patchgroup_update_top_bottom(state, 0);
	if(r < 0)
	{
		state->engaged = 0;
		patchgroup->engaged_count--;
		current_scope->engaged_count--;
	}
	else
	{
		if((patchgroup->flags & PATCHGROUP_FLAG_ATOMIC) && !patchgroup->has_data)
			journal_bd_add_hold();
		/* mark it as having data since it is now engaged */
		/* (and therefore could acquire data at any time) */
		patchgroup->has_data = 1;
	}

	return r;
}

int patchgroup_disengage(patchgroup_t * patchgroup)
{
	int r;
	patchgroup_state_t * state;
	
	if(!current_scope)
		return -ENODEV;
	if(!patchgroup)
		return -EINVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) patchgroup->id);
	if(!state)
		return -ENOENT;
	assert(state->patchgroup == patchgroup);
	if(!state->engaged)
		return 0;
	Dprintf("%s(): patchgroup = %p, debug = %d\n", __FUNCTION__, patchgroup, FSTITCH_DEBUG_COUNT());
	
	state->engaged = 0;
	patchgroup->engaged_count--;
	current_scope->engaged_count--;
	
	r = patchgroup_update_top_bottom(state, 1);
	if(r < 0)
	{
		state->engaged = 1;
		patchgroup->engaged_count++;
		current_scope->engaged_count++;
	}

	return r;
}

int patchgroup_release(patchgroup_t * patchgroup)
{
	if(!patchgroup)
		return -EINVAL;
	/* can't release atomic patchgroup if it is engaged */
	if((patchgroup->flags & PATCHGROUP_FLAG_ATOMIC) && patchgroup->engaged_count)
		return -EINVAL;
	Dprintf("%s(): patchgroup = %p, debug = %d\n", __FUNCTION__, patchgroup, FSTITCH_DEBUG_COUNT());
	if(patchgroup->tail_keep)
	{
		patch_satisfy(&patchgroup->tail_keep);
		if(patchgroup->flags & PATCHGROUP_FLAG_ATOMIC)
			journal_bd_remove_hold();
		patchgroup->is_released = 1;
	}
	return 0;
}

int patchgroup_abandon(patchgroup_t ** patchgroup)
{
	patchgroup_state_t * state;
	if(!current_scope)
		return -ENODEV;
	if(!patchgroup || !*patchgroup)
		return -EINVAL;
	state = hash_map_erase(current_scope->id_map, (void *) (*patchgroup)->id);
	if(!state)
		return -ENOENT;
	assert(state->patchgroup == *patchgroup);
	/* can't abandon a non-released atomic patchgroup */
	if(((*patchgroup)->flags & PATCHGROUP_FLAG_ATOMIC) && !(*patchgroup)->is_released)
		return -EINVAL;
	/* can't abandon an engaged patchgroup */
	if(state->engaged)
		return -EBUSY;
	Dprintf("%s(): patchgroup = %p, debug = %d\n", __FUNCTION__, *patchgroup, FSTITCH_DEBUG_COUNT());
	if(!--state->patchgroup->references)
	{
		if((*patchgroup)->flags & PATCHGROUP_FLAG_ATOMIC)
		{
			assert(atomic_patchgroup_exists);
			atomic_patchgroup_exists = 0;
		}

		/* no more references to this patchgroup */
		if(state->patchgroup->tail_keep || !state->patchgroup->is_released)
		{
			if(!state->patchgroup->has_data)
				patchgroup_release(state->patchgroup);
			else
				kpanic("Don't know how to roll back an abandoned patchgroup!");
		}
		if(state->patchgroup->head_keep)
			patch_satisfy(&state->patchgroup->head_keep);
		patch_weak_release(&state->patchgroup->head, 0);
		patch_weak_release(&state->patchgroup->tail, 0);
		free(state->patchgroup);
	}

	/* patchgroup_scope_destroy() passes us *patchgroup inside state... */
	*patchgroup = NULL;
	free(state);
	return 0;
}

patchgroup_t * patchgroup_lookup(patchgroup_id_t id)
{
	patchgroup_state_t * state;
	if(!current_scope)
		return NULL;
	state = (patchgroup_state_t *) hash_map_find_val(current_scope->id_map, (void *) id);
	return state ? state->patchgroup : NULL;
}

patchgroup_id_t patchgroup_id(const patchgroup_t * patchgroup)
{
	if(!patchgroup)
		return -EINVAL;
	return patchgroup->id;
}

int patchgroup_engaged(void)
{
	return (current_scope && current_scope->engaged_count) || masquerade_count;
}

void patchgroup_masquerade(void)
{
	masquerade_count++;
}

void patchgroup_demasquerade(void)
{
	assert(masquerade_count);
	masquerade_count--;
}

int patchgroup_prepare_head(patch_t ** head)
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
		r = patch_create_noop_list(NULL, head, WEAK(current_scope->bottom), *head, NULL);
		if(r < 0)
			return r;
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, *head, "and");
		patch_set_noop_declare(*head);
	}
	else
		*head = WEAK(current_scope->bottom);
	
	return 0;
}

int patchgroup_finish_head(patch_t * head)
{
	if(!current_scope || !current_scope->top || !head || head == WEAK(current_scope->bottom))
		return 0;
	if(head->flags & PATCH_NO_PATCHGROUP)
		return 0;
	return patch_add_depend(current_scope->top, head);
}

int patchgroup_label(patchgroup_t * patchgroup, const char * label)
{
	if(!patchgroup)
		return -EINVAL;
	if(WEAK(patchgroup->head))
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, WEAK(patchgroup->head), "og head: %s", label);
	if(WEAK(patchgroup->tail))
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, WEAK(patchgroup->tail), "og tail: %s", label);
	return 0;
}
