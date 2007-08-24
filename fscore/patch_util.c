#include <lib/platform.h>

#include <fscore/debug.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>

void patch_mark_graph(patch_t * root)
{
	chdepdesc_t * dep;
	root->flags |= PATCH_MARKED;
	FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, root, PATCH_MARKED);
	for(dep = root->befores; dep; dep = dep->before.next)
		if(!(dep->before.desc->flags & PATCH_MARKED))
			patch_mark_graph(dep->before.desc);
}

void patch_unmark_graph(patch_t * root)
{
	chdepdesc_t * dep;
	root->flags &= ~PATCH_MARKED;
	FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_CLEAR_FLAGS, root, PATCH_MARKED);
	for(dep = root->befores; dep; dep = dep->before.next)
		if(dep->before.desc->flags & PATCH_MARKED)
			patch_unmark_graph(dep->before.desc);
}

int patch_push_down(bdesc_t * block, BD_t * current_bd, BD_t * target_bd)
{
	patch_dlist_t * dlist = block->index_changes;
	assert(current_bd && target_bd);
	assert(current_bd->level == target_bd->level);
	if(dlist[current_bd->graph_index].head)
	{
		patch_t * patch;
		for (patch = dlist[current_bd->graph_index].head;
		     patch;
		     patch = patch->ddesc_index_next)
		{
			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_OWNER, patch, target_bd);
			/* don't unlink them from index here */
			patch->owner = target_bd;
		}
		
		/* append the target index list to ours */
		*dlist[current_bd->graph_index].tail = dlist[target_bd->graph_index].head;
		if(dlist[target_bd->graph_index].head)
			dlist[target_bd->graph_index].head->ddesc_index_pprev = dlist[current_bd->graph_index].tail;
		else
			dlist[target_bd->graph_index].tail = dlist[current_bd->graph_index].tail;
		
		/* make target index point at our list */
		dlist[target_bd->graph_index].head = dlist[current_bd->graph_index].head;
		dlist[current_bd->graph_index].head->ddesc_index_pprev = &dlist[target_bd->graph_index].head;
		
		/* make current index empty */
		dlist[current_bd->graph_index].head = NULL;
		dlist[current_bd->graph_index].tail = &dlist[current_bd->graph_index].head;
	}
	return 0;
}

/* FIXME: get rid of the olddata parameter here, and just use the block's data as the old data */
int patch_create_diff_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, patch_t ** tail, patch_pass_set_t * befores)
{
	int r, start, end;
	uint8_t * old = (uint8_t *) olddata;
	uint8_t * new = (uint8_t *) newdata;

	if(!old || !new || !tail || length < 1)
		return -EINVAL;

	for(start = 0; start < length && old[start] == new[start]; start++);
	if(start >= length)
		return 0;
	for(end = length - 1; end >= start && old[end] == new[end]; end--);
	assert(start <= end);
	
	r = patch_create_byte_set(block, owner, offset + start, end - start + 1, &new[start], tail, befores);
	if(r < 0)
		return r;
	return 1;
}

int patch_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, patch_t ** head)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return patch_create_diff_set(block, owner, offset, length, olddata, newdata, head, PASS_PATCH_SET(set));
}
