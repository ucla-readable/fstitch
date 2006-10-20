#include <inc/error.h>
#include <lib/assert.h>
#include <lib/hash_map.h>
#include <lib/kdprintf.h>
#include <lib/panic.h>

#include <kfs/sync.h>
#include <kfs/journal_bd.h>
#include <kfs/opgroup.h>

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

struct opgroup {
	opgroup_id_t id;
	chdesc_t * head;
	/* head_keep stays until we get an after */
	chdesc_t * head_keep;
	chdesc_t * tail;
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
	chdesc_t * bottom;
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
		scope->next_id = 1;
		scope->top = NULL;
		scope->top_keep = NULL;
		scope->bottom = NULL;
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
		goto error_1;
	
	copy->next_id = scope->next_id;
	copy->top = scope->top;
	if(copy->top)
	{
		/* we need our own top_keep */
		if(chdesc_create_noop_list(NULL, NULL, &copy->top_keep, NULL) < 0)
			goto error_2;
		chdesc_claim_noop(copy->top_keep);
		if(chdesc_add_depend(copy->top, copy->top_keep) < 0)
			goto error_3;
	}
	if(chdesc_weak_retain(scope->bottom, &copy->bottom) < 0)
		goto error_3;
	
	/* iterate over opgroups and increase reference counts */
	hash_map_it_init(&it, scope->id_map);
	while((state = hash_map_val_next(&it)))
	{
		opgroup_state_t * dup = malloc(sizeof(*dup));
		if(!dup)
			goto error_4;
		*dup = *state;
		if(hash_map_insert(copy->id_map, (void *) dup->opgroup->id, dup) < 0)
		{
			free(dup);
			goto error_4;
		}
		dup->opgroup->references++;
		/* FIXME: can we do better than just assert? */
		assert(dup->opgroup->references);
		if(dup->engaged)
		{
			dup->opgroup->engaged_count++;
			/* FIXME: can we do better than just assert? */
			assert(dup->opgroup->engaged_count);
		}
	}
	
	return copy;
	
error_4:
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
	
	chdesc_weak_release(&copy->bottom);
error_3:
	if(copy->top_keep)
		chdesc_satisfy(&copy->top_keep);
error_2:
	free(copy);
error_1:
	return NULL;
}

void opgroup_scope_destroy(opgroup_scope_t * scope)
{
	hash_map_it_t it;
	opgroup_state_t * state;
	opgroup_scope_t * old_scope = current_scope;
	
	/* opgroup_abandon() needs the current scope
	 * to be the one we are destroying... */
	current_scope = scope;
	
	/* iterate over opgroups and abandon them */
	hash_map_it_init(&it, scope->id_map);
	while((state = hash_map_val_next(&it)))
	{
		/* This loop is tricky, because opgroup_abandon() will call
		 * hash_map_erase() and free the opgroup_state_t we just got
		 * from the iterator. So we have to restart the iterator every
		 * time through the loop. */
		int r = opgroup_disengage(state->opgroup);
		assert(r >= 0);
		opgroup_abandon(&state->opgroup);
		hash_map_it_init(&it, scope->id_map);
	}
	hash_map_destroy(scope->id_map);
	
	/* restore the current scope (unless it was destroyed) */
	current_scope = (old_scope == scope) ? NULL : old_scope;
	
	if(scope->top_keep)
		chdesc_satisfy(&scope->top_keep);
	chdesc_weak_release(&scope->bottom);
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
		goto error_1;
	if(!(state = malloc(sizeof(*state))))
		goto error_2;
	
	op->id = current_scope->next_id++;
	op->references = 1;
	op->has_data = 0;
	op->is_released = 0;
	op->engaged_count = 0;
	op->has_afters = 0;
	op->has_befores = 0;
	op->flags = flags;
	state->opgroup = op;
	state->engaged = 0;
	
	if(chdesc_create_noop_list(NULL, NULL, &op->head, NULL) < 0)
		goto error_3;
	if(chdesc_weak_retain(op->head, &op->head) < 0)
		goto error_4;
	if(chdesc_create_noop_list(NULL, NULL, &op->head_keep, NULL) < 0)
		goto error_4;
	chdesc_claim_noop(op->head_keep);
	if(chdesc_add_depend(op->head, op->head_keep) < 0)
		goto error_5;
	if(chdesc_create_noop_list(NULL, NULL, &op->tail, NULL) < 0)
		goto error_6;
	if(chdesc_weak_retain(op->tail, &op->tail) < 0)
		goto error_7;
	if(chdesc_add_depend(op->head, op->tail) < 0)
		goto error_7;
	if(chdesc_create_noop_list(NULL, NULL, &op->tail_keep, NULL) < 0)
		goto error_8;
	chdesc_claim_noop(op->tail_keep);
	if(chdesc_add_depend(op->tail, op->tail_keep) < 0)
		goto error_9;
	if(hash_map_insert(current_scope->id_map, (void *) op->id, state) < 0)
		goto error_10;
	
	return op;
	
error_10:
	chdesc_remove_depend(op->tail, op->tail_keep);
error_9:
	chdesc_destroy(&op->tail_keep);
error_8:
	chdesc_remove_depend(op->head, op->tail);
error_7:
	chdesc_destroy(&op->tail);
error_6:
	chdesc_remove_depend(op->head, op->head_keep);
error_5:
	chdesc_destroy(&op->head_keep);
error_4:
	chdesc_destroy(&op->head);
error_3:
	free(state);
error_2:
	free(op);
error_1:
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
		return -E_INVAL;
	/* from before's perspective, we are adding an after
	 *   => before must not be engaged [anywhere] if it is not atomic */
	if(!(before->flags & OPGROUP_FLAG_ATOMIC) && before->engaged_count)
		return -E_BUSY;
	/* from after's perspective, we are adding a before
	 *   => after must not be released (standard case) or have an after (noop case) */
	assert(!after->tail_keep == after->is_released);
	if(after->is_released || after->has_afters)
		return -E_INVAL;
	/* it might not have a head if it's already been written to disk */
	/* (in this case, it won't be engaged again since it will have
	 * afters now, so we don't need to recreate it) */
	if(before->head)
		/* notice that this can fail if there is a before cycle */
		r = chdesc_add_depend(after->tail, before->head);
	if(r >= 0)
	{
		after->has_befores = 1;
		before->has_afters = 1;
		if(before->head_keep)
			chdesc_satisfy(&before->head_keep);
	}
	else
		kdprintf(STDERR_FILENO, "%s: chdesc_add_depend() unexpectedly failed (%i)\n", __FUNCTION__, r);
	return r;
}

static int opgroup_update_top_bottom(void)
{
	hash_map_it_t it;
	opgroup_state_t * state;
	chdesc_t * top;
	chdesc_t * top_keep;
	chdesc_t * bottom;
	chdesc_t * save_top = current_scope->top;
	int r, count = 0;
	
	/* create new top and bottom */
	r = chdesc_create_noop_list(NULL, NULL, &top, NULL);
	if(r < 0)
		goto error_1;
	r = chdesc_create_noop_list(NULL, NULL, &top_keep, NULL);
	if(r < 0)
		goto error_2;
	chdesc_claim_noop(top_keep);
	r = chdesc_add_depend(top, top_keep);
	if(r < 0)
		goto error_3;
	r = chdesc_create_noop_list(NULL, NULL, &bottom, NULL);
	if(r < 0)
		goto error_4;
	r = chdesc_add_depend(top, bottom);
	if(r < 0)
	{
		chdesc_destroy(&bottom);
	error_4:
		chdesc_remove_depend(top, top_keep);
	error_3:
		chdesc_destroy(&top_keep);
	error_2:
		chdesc_destroy(&top);
	error_1:
		return r;
	}
	
	hash_map_it_init(&it, current_scope->id_map);
	while((state = hash_map_val_next(&it)))
		if(state->engaged)
		{
			assert(state->opgroup->head);
			r = chdesc_add_depend(state->opgroup->head, top);
			if(r < 0)
			{
			error_loop:
				chdesc_satisfy(&top_keep);
				/* satisfy a chdesc with befores... is this OK? */
				chdesc_satisfy(&bottom);
				return r;
			}
			if(state->opgroup->tail)
				if(chdesc_add_depend(bottom, state->opgroup->tail) < 0)
					goto error_loop;
			count++;
		}
	
	if(!bottom->befores)
	{
		chdesc_remove_depend(top, bottom);
		/* let it get garbage collected */
		bottom = NULL;
	}
	
	if(chdesc_weak_retain(bottom, &current_scope->bottom) < 0)
	{
		r = chdesc_weak_retain(save_top, &current_scope->top);
		assert(r >= 0);
		goto error_loop;
	}
	
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
	
	return 0;
}

int opgroup_engage(opgroup_t * opgroup)
{
	int r;
	opgroup_state_t * state;
	
	if(!current_scope)
		return -E_NO_DEV;
	if(!opgroup)
		return -E_INVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == opgroup);
	if(!(opgroup->flags & OPGROUP_FLAG_ATOMIC) && (!opgroup->is_released || !opgroup->is_released))
		return -E_INVAL;
	/* can't engage it if it is not atomic and it has afters */
	if(!(opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->has_afters)
		return -E_INVAL;
	/* can't engage it if it is atomic and has been released */
	if((opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->is_released)
		return -E_INVAL;
	if(state->engaged)
		return 0;
	
	state->engaged = 1;
	opgroup->engaged_count++;
	/* FIXME: can we do better than just assert? */
	assert(state->opgroup->engaged_count);
	
	r = opgroup_update_top_bottom();
	if(r < 0)
	{
		state->engaged = 0;
		opgroup->engaged_count--;
	}
	else
	{
		if ((opgroup->flags & OPGROUP_FLAG_ATOMIC) && !opgroup->has_data)
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
		return -E_NO_DEV;
	if(!opgroup)
		return -E_INVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == opgroup);
	if(!state->engaged)
		return 0;
	
	state->engaged = 0;
	opgroup->engaged_count--;
	
	r = opgroup_update_top_bottom();
	if(r < 0)
	{
		state->engaged = 1;
		opgroup->engaged_count++;
	}

	return r;
}

int opgroup_release(opgroup_t * opgroup)
{
	if(!opgroup)
		return -E_INVAL;
	/* can't release atomic opgroup if it is engaged */
	if((opgroup->flags & OPGROUP_FLAG_ATOMIC) && opgroup->engaged_count)
		return -E_INVAL;
	if(opgroup->tail_keep)
	{
		chdesc_satisfy(&opgroup->tail_keep);
		if (opgroup->flags & OPGROUP_FLAG_ATOMIC)
			journal_bd_remove_hold();
		opgroup->is_released = 1;
	}
	return 0;
}

int opgroup_abandon(opgroup_t ** opgroup)
{
	opgroup_state_t * state;
	if(!current_scope)
		return -E_NO_DEV;
	if(!opgroup || !*opgroup)
		return -E_INVAL;
	state = hash_map_erase(current_scope->id_map, (void *) (*opgroup)->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == *opgroup);
	/* can't abandon a non-released atomic opgroup */
	if(((*opgroup)->flags & OPGROUP_FLAG_ATOMIC) && !(*opgroup)->is_released)
		return -E_INVAL;
	/* can't abandon an engaged opgroup */
	if(state->engaged)
		return -E_BUSY;
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
				panic("Don't know how to roll back an abandoned opgroup!");
		}
		if(state->opgroup->head_keep)
			chdesc_satisfy(&state->opgroup->head_keep);
		chdesc_weak_release(&state->opgroup->head);
		chdesc_weak_release(&state->opgroup->tail);
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
		return -E_INVAL;
	return opgroup->id;
}

int opgroup_prepare_head(chdesc_t ** head)
{
	if(!current_scope || !current_scope->bottom)
		return 0;
	
	if(*head)
	{
		int r;
		chdesc_t * nh;
		r = chdesc_create_noop_list(NULL, NULL, &nh, NULL);
		if(r < 0)
			return r;
		r = chdesc_add_depend(nh, current_scope->bottom);
		if(r < 0)
			return r;
		r = chdesc_add_depend(nh, *head);
		if(r < 0)
			/* let it get cleaned up automatically */
			return r;
		*head = nh;
	}
	else
		*head = current_scope->bottom;
	
	return 0;
}

int opgroup_finish_head(chdesc_t * head)
{
	if(!current_scope || !current_scope->top || !head)
		return 0;
	return chdesc_add_depend(current_scope->top, head);
}
