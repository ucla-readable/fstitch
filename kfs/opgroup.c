#include <lib/hash_map.h>
#include <inc/error.h>

#include <kfs/opgroup.h>

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
	opgroup_scope_t * copy = malloc(sizeof(*copy));
	if(copy)
	{
		*copy = *scope;
		if(chdesc_weak_retain(copy->top, &copy->top) < 0)
			goto error_1;
		if(chdesc_weak_retain(copy->bottom, &copy->bottom) < 0)
			goto error_2;
		copy->id_map = hash_map_copy(scope->id_map);
		if(!copy->id_map)
		{
			chdesc_weak_release(&copy->bottom);
		error_2:
			chdesc_weak_release(&copy->top);
		error_1:
			free(copy);
			copy = NULL;
		}
		else
		{
			hash_map_it_t it;
			opgroup_t * op;
			
			/* iterate over opgroups and increase reference counts */
			hash_map_it_init(&it, copy->id_map);
			while((op = hash_map_val_next(&it)))
				op->references++;
		}
	}
	return copy;
}

void opgroup_scope_destroy(opgroup_scope_t * scope)
{
	hash_map_it_t it;
	opgroup_t * op;
	opgroup_scope_t * old_scope = current_scope;
	
	current_scope = scope;
	/* iterate over opgroups and abandon them */
	hash_map_it_init(&it, scope->id_map);
	while((op = hash_map_val_next(&it)))
		opgroup_abandon(&op);
	current_scope = (old_scope == scope) ? NULL : old_scope;
	
	chdesc_weak_release(&scope->top);
	chdesc_weak_release(&scope->bottom);
	hash_map_destroy(scope->id_map);
	free(scope);
}

void opgroup_scope_set_current(opgroup_scope_t * scope)
{
	current_scope = scope;
}

opgroup_t * opgroup_create(int flags)
{
	opgroup_t * op;
	
	if(!current_scope)
		return NULL;
	if(flags)
		return NULL;
	
	if(!(op = malloc(sizeof(*op))))
		goto error_1;
	op->id = current_scope->next_id++;
	op->references = 1;
	if(!(op->head = chdesc_create_noop(NULL, NULL)))
		goto error_2;
	if(!(op->tail = chdesc_create_noop(NULL, NULL)))
		goto error_3;
	if(chdesc_add_depend(op->head, op->tail) < 0)
		goto error_4;
	if(!(op->keep = chdesc_create_noop(NULL, NULL)))
		goto error_5;
	chdesc_claim_noop(op->keep);
	if(chdesc_add_depend(op->tail, op->keep) < 0)
		goto error_6;
	if(hash_map_insert(current_scope->id_map, (void *) op->id, op) < 0)
		goto error_7;
	return op;
	
	/* TODO: fix the error recovery code */
error_7:
	chdesc_remove_depend(op->tail, op->keep);
error_6:
	chdesc_destroy(&op->keep);
error_5:
	chdesc_remove_depend(op->head, op->tail);
error_4:
	chdesc_destroy(&op->tail);
error_3:
	chdesc_destroy(&op->head);
error_2:
	free(op);
error_1:
	return NULL;
}

int opgroup_add_depend(opgroup_t * dependent, opgroup_t * dependency)
{
	/* TODO: check state first */
	/* from dependent's perspective, we are adding a dependency */
	/* from dependency's perspective, we are adding a dependent */
	return chdesc_add_depend(dependent->tail, dependency->head);
}

int opgroup_engage(opgroup_t * opgroup)
{
	/* TODO: write this function */
	return -1;
}

int opgroup_disengage(opgroup_t * opgroup)
{
	/* TODO: write this function */
	return -1;
}

int opgroup_release(opgroup_t * opgroup)
{
	if(opgroup->keep)
		chdesc_satisfy(&opgroup->keep);
	return 0;
}

int opgroup_abandon(opgroup_t ** opgroup)
{
	opgroup_t * op;
	if(!current_scope)
		return -E_INVAL;
	op = (opgroup_t *) hash_map_erase(current_scope->id_map, (void *) (*opgroup)->id);
	if(op != *opgroup)
		return -E_NOT_FOUND;
	if(!--op->references)
	{
		/* no more references to this opgroup */
		if(op->keep)
			panic("Don't know how to roll back an abandoned opgroup!");
		chdesc_weak_release(&op->head);
		chdesc_weak_release(&op->tail);
		free(op);
		*opgroup = NULL;
	}
	return 0;
}

opgroup_t * opgroup_lookup(opgroup_id_t id)
{
	return current_scope ? (opgroup_t *) hash_map_find_val(current_scope->id_map, (void *) id) : NULL;
}

chdesc_t * opgroup_get_engaged_top(void)
{
	return current_scope ? current_scope->top : NULL;
}

chdesc_t * opgroup_get_engaged_bottom(void)
{
	return current_scope ? current_scope->bottom : NULL;
}
