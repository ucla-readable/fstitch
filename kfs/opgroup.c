#include <lib/hash_map.h>
#include <lib/panic.h>
#include <inc/error.h>
#include <assert.h>

#include <kfs/opgroup.h>

struct opgroup {
	opgroup_id_t id;
	chdesc_t * head;
	chdesc_t * tail;
	chdesc_t * keep;
	uint32_t references:29;
	uint32_t has_data:1;
	uint32_t has_dependents:1;
	uint32_t has_dependencies:1;
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
	chdesc_t * bottom;
};

static opgroup_scope_t * current_scope = NULL;

opgroup_scope_t * opgroup_scope_create(void)
{
	opgroup_scope_t * scope = malloc(sizeof(*scope));
	if(scope)
	{
		scope->next_id = 1;
		scope->top = NULL;
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
	if(chdesc_weak_retain(scope->top, &copy->top) < 0)
		goto error_2;
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
		if(hash_map_insert(copy->id_map, (void *) state->opgroup->id, dup) < 0)
		{
			free(dup);
			goto error_4;
		}
		state->opgroup->references++;
	}
	
	return copy;
	
error_4:
	hash_map_it_init(&it, copy->id_map);
	while((state = hash_map_val_next(&it)))
	{
		/* don't need to check for 0 */
		state->opgroup->references--;
		free(state);
	}
	hash_map_destroy(copy->id_map);
	
	chdesc_weak_release(&copy->bottom);
error_3:
	chdesc_weak_release(&copy->top);
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
		opgroup_abandon(&state->opgroup);
		hash_map_it_init(&it, scope->id_map);
	}
	hash_map_destroy(scope->id_map);
	
	/* restore the current scope (unless it was destroyed) */
	current_scope = (old_scope == scope) ? NULL : old_scope;
	
	chdesc_weak_release(&scope->top);
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
	if(flags)
		return NULL;
	
	if(!(op = malloc(sizeof(*op))))
		goto error_1;
	if(!(state = malloc(sizeof(*state))))
		goto error_2;
	
	op->id = current_scope->next_id++;
	op->references = 1;
	op->has_data = 0;
	op->has_dependents = 0;
	op->has_dependencies = 0;
	state->opgroup = op;
	state->engaged = 0;
	
	if(!(op->head = chdesc_create_noop(NULL, NULL)))
		goto error_3;
	if(chdesc_weak_retain(op->head, &op->head) < 0)
		goto error_4;
	if(!(op->tail = chdesc_create_noop(NULL, NULL)))
		goto error_4;
	if(chdesc_weak_retain(op->tail, &op->tail) < 0)
		goto error_5;
	if(chdesc_add_depend(op->head, op->tail) < 0)
		goto error_5;
	if(!(op->keep = chdesc_create_noop(NULL, NULL)))
		goto error_6;
	chdesc_claim_noop(op->keep);
	if(chdesc_add_depend(op->tail, op->keep) < 0)
		goto error_7;
	if(hash_map_insert(current_scope->id_map, (void *) op->id, state) < 0)
		goto error_8;
	
	return op;
	
error_8:
	chdesc_remove_depend(op->tail, op->keep);
error_7:
	chdesc_destroy(&op->keep);
error_6:
	chdesc_remove_depend(op->head, op->tail);
error_5:
	chdesc_destroy(&op->tail);
error_4:
	chdesc_destroy(&op->head);
error_3:
	free(state);
error_2:
	free(op);
error_1:
	return NULL;
}

int opgroup_add_depend(opgroup_t * dependent, opgroup_t * dependency)
{
	int r = 0;
	if(!dependent || !dependency)
		return -E_INVAL;
	/* from dependency's perspective, we are adding a dependent
	 *   => always allowed */
	/* from dependent's perspective, we are adding a dependency
	 *   => dependent must not be released */
	if(!dependent->keep)
		return -E_INVAL;
	/* TODO: make sure this function doesn't need anything more */
	/* it might not have a head if it's already been written to disk */
	if(dependency->head)
		/* notice that this can fail if there is a dependency cycle */
		r = chdesc_add_depend(dependent->tail, dependency->head);
	if(r >= 0)
	{
		dependent->has_dependencies = 1;
		dependency->has_dependents = 1;
	}
	return r;
}

static int opgroup_update_top_bottom(void)
{
	hash_map_it_t it;
	opgroup_state_t * state;
	chdesc_t * top;
	chdesc_t * bottom;
	chdesc_t * save_top = current_scope->top;
	int r;
	
	/* TODO: finish this function */
	return -1;
	
	/* create new top and bottom */
	top = chdesc_create_noop(NULL, NULL);
	if(!top)
		return -E_NO_MEM;
	bottom = chdesc_create_noop(NULL, NULL);
	if(!bottom)
	{
		chdesc_destroy(&top);
		return -E_NO_MEM;
	}
	if((r = chdesc_add_depend(top, bottom)) < 0)
	{
		chdesc_destroy(&bottom);
		chdesc_destroy(&top);
		return r;
	}
	
	hash_map_it_init(&it, current_scope->id_map);
	while((state = hash_map_val_next(&it)))
		if(state->engaged)
		{
			if(!state->opgroup->head)
			{
				/* ... */
			}
			if(chdesc_add_depend(state->opgroup->head, top) < 0)
			{
				/* ... */
			}
			if(!state->opgroup->tail)
			{
				/* ... */
			}
			if(chdesc_add_depend(bottom, state->opgroup->tail) < 0)
			{
				/* ... */
			}
		}
	
	if(chdesc_weak_retain(top, &current_scope->top) < 0)
	{
		/* ... */
	}
	if(chdesc_weak_retain(bottom, &current_scope->bottom) < 0)
	{
		chdesc_weak_release(&current_scope->top);
		r = chdesc_weak_retain(save_top, &current_scope->top);
		assert(r >= 0);
		/* ... */
	}
	
	return 0;
}

int opgroup_engage(opgroup_t * opgroup)
{
	int r;
	opgroup_state_t * state;
	
	if(!current_scope)
		return -E_INVAL;
	if(!opgroup)
		return -E_INVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == opgroup);
	/* can't engage it if it has dependents */
	if(opgroup->has_dependents)
		return -E_INVAL;
	if(state->engaged)
		return 0;
	
	state->engaged = 1;
	
	r = opgroup_update_top_bottom();
	if(r < 0)
		state->engaged = 0;
	else
		/* mark it as having data since it is now engaged */
		state->opgroup->has_data = 1;
	return r;
}

int opgroup_disengage(opgroup_t * opgroup)
{
	int r;
	opgroup_state_t * state;
	
	if(!current_scope)
		return -E_INVAL;
	if(!opgroup)
		return -E_INVAL;
	state = hash_map_find_val(current_scope->id_map, (void *) opgroup->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == opgroup);
	if(!state->engaged)
		return 0;
	
	state->engaged = 0;
	
	r = opgroup_update_top_bottom();
	if(r < 0)
		state->engaged = 1;
	return r;
}

int opgroup_release(opgroup_t * opgroup)
{
	if(!opgroup)
		return -E_INVAL;
	if(opgroup->keep)
		chdesc_satisfy(&opgroup->keep);
	return 0;
}

int opgroup_abandon(opgroup_t ** opgroup)
{
	opgroup_state_t * state;
	if(!current_scope)
		return -E_INVAL;
	if(!opgroup)
		return -E_INVAL;
	state = hash_map_erase(current_scope->id_map, (void *) (*opgroup)->id);
	if(!state)
		return -E_NOT_FOUND;
	assert(state->opgroup == *opgroup);
	if(!--state->opgroup->references)
	{
		/* no more references to this opgroup */
		if(state->opgroup->keep)
			panic("Don't know how to roll back an abandoned opgroup!");
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
	return current_scope ? (opgroup_t *) hash_map_find_val(current_scope->id_map, (void *) id) : NULL;
}

opgroup_id_t opgroup_id(const opgroup_t * opgroup)
{
	if(!current_scope)
		return -E_UNSPECIFIED;
	if(!opgroup)
		return -E_INVAL;
	return opgroup->id;
}

int opgroup_insert_change(chdesc_t * head, chdesc_t * tail)
{
	int r;
	if(!current_scope)
		return 0;
	/* technically we could set has_data here instead of in engage... but is that better? */
	if(!current_scope->top)
	{
		/* TODO: recreate the top if it is gone */
		/* ... */
		return -1;
	}
	r = chdesc_add_depend(current_scope->top, head);
	if(r < 0)
		return r;
	if(current_scope->bottom)
	{
		r = chdesc_add_depend(tail, current_scope->bottom);
		if(r < 0)
		{
			chdesc_remove_depend(current_scope->top, head);
			return r;
		}
	}
	return 0;
}
