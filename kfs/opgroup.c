#include <lib/hash_map.h>
#include <inc/error.h>

#include <kfs/opgroup.h>

static opgroup_id_t next_opgroup_id = 0;
static hash_map_t * id_map = NULL;

int opgroup_init(void)
{
	if(id_map)
		return -E_BUSY;
	id_map = hash_map_create();
	return id_map ? 0 : -E_NO_MEM;
}

opgroup_t * opgroup_create(int flags)
{
	opgroup_t * op;
	if(flags)
		goto error_1;
	if(!(op = malloc(sizeof(*op))))
		goto error_1;
	op->id = next_opgroup_id++;
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
	if(hash_map_insert(id_map, (void *) op->id, op) < 0)
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
	/* TODO: check state first */
	free(*opgroup);
	*opgroup = NULL;
	return 0;
}

opgroup_t * opgroup_lookup(opgroup_id_t id)
{
	return (opgroup_t *) hash_map_find_val(id_map, (void *) id);
}
