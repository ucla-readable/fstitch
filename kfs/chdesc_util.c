#include <lib/platform.h>

#include <kfs/debug.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

void chdesc_mark_graph(chdesc_t * root)
{
	chdepdesc_t * dep;
	root->flags |= CHDESC_MARKED;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, root, CHDESC_MARKED);
	for(dep = root->befores; dep; dep = dep->before.next)
		if(!(dep->before.desc->flags & CHDESC_MARKED))
			chdesc_mark_graph(dep->before.desc);
}

void chdesc_unmark_graph(chdesc_t * root)
{
	chdepdesc_t * dep;
	root->flags &= ~CHDESC_MARKED;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, root, CHDESC_MARKED);
	for(dep = root->befores; dep; dep = dep->before.next)
		if(dep->before.desc->flags & CHDESC_MARKED)
			chdesc_unmark_graph(dep->before.desc);
}

int chdesc_push_down(BD_t * current_bd, bdesc_t * current_block, BD_t * target_bd, bdesc_t * target_block)
{
	chdesc_dlist_t * dlist = current_block->ddesc->index_changes;
	if(target_block->ddesc != current_block->ddesc)
		return -EINVAL;
	if(dlist[current_bd->graph_index].head)
	{
		chdesc_t * chdesc;
		for (chdesc = dlist[current_bd->graph_index].head;
		     chdesc;
		     chdesc = chdesc->ddesc_index_next)
		{
			uint16_t prev_level = chdesc_level(chdesc);
			uint16_t new_level;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, chdesc, target_bd);
			/* don't unlink them from index here */
			chdesc_unlink_ready_changes(chdesc);
			chdesc->owner = target_bd;
			assert(chdesc->block);
			bdesc_retain(target_block);
			bdesc_release(&chdesc->block);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, chdesc, target_block);
			chdesc->block = target_block;
			chdesc_update_ready_changes(chdesc);
			/* don't link them to index here */
			
			new_level = chdesc_level(chdesc);
			if(prev_level != new_level)
				chdesc_propagate_level_change(chdesc, prev_level, new_level);
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

/* Write an entire block with new data, assuming that either A) no change
 * descriptors exist on the block or B) the entire block has a single BYTE
 * change descriptor on it. In case B, use chdesc_rewrite_byte() to rewrite
 * the existing change descriptor to reflect the new data, and return NULL
 * in *head. In case A, return the newly created change descriptor in *head. */
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	chdesc_t * rewrite = block->ddesc->index_changes[owner->graph_index].head;
	
	if(!rewrite || rewrite->type != BYTE || rewrite->byte.offset || rewrite->byte.length != block->ddesc->length || (rewrite->flags & CHDESC_INFLIGHT))
		return chdesc_create_full(block, owner, data, head);
	
	if(*head)
	{
		/* check to see whether *head is compatible with the existing chdesc */
		chdepdesc_t * befores;
		for(befores = rewrite->befores; befores; befores = befores->before.next)
			if(befores->before.desc == *head)
				break;
		if(!befores)
			/* we did not find *head among existing befores */
			return chdesc_create_full(block, owner, data, head);
	}
	
	*head = NULL;
	return chdesc_rewrite_byte(rewrite, 0, rewrite->byte.length, data);
}

/* FIXME: get rid of the olddata parameter here, and just use the block's data as the old data */
int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head)
{
	int r, start, end;
	uint8_t * old = (uint8_t *) olddata;
	uint8_t * new = (uint8_t *) newdata;

	if(!old || !new || !head || length < 1)
		return -EINVAL;

	for(start = 0; start < length && old[start] == new[start]; start++);
	if(start >= length)
		return 0;
	for(end = length - 1; end >= start && old[end] == new[end]; end--);
	assert(start <= end);
	
	r = chdesc_create_byte(block, owner, offset + start, end - start + 1, &new[start], head);
	if(r < 0)
		return r;
	return 1;
}
