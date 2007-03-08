#include <lib/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/memdup.h>
#include <lib/vector.h>

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
		return -E_INVAL;
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
 * descriptors exist on the block or B) the entire block has a single layer of
 * BYTE change descriptors on it. In case B, use chdesc_rewrite_byte() to
 * rewrite the existing change descriptors to reflect the new data, and return
 * NULL in *head. In case A, return the newly created change descriptors in
 * *head. */
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	chdesc_t * scan;
	uint16_t range = block->ddesc->length;
	
	if(!block->ddesc->all_changes)
		return chdesc_create_full(block, owner, data, head);
	
	if(*head)
		/* check to see whether *head is compatible with the existing chdescs */
		for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		{
			chdepdesc_t * befores;
			if(scan->type != BYTE)
				continue;
			for(befores = scan->befores; befores; befores = befores->before.next)
				if(befores->before.desc == *head)
					break;
			if(!befores)
				/* we did not find *head among existing befores */
				return chdesc_create_full(block, owner, data, head);
		}
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
	{
		uint16_t offset;
		if(scan->type != BYTE)
			continue;
		offset = scan->byte.length;
		if(chdesc_rewrite_byte(scan, 0, offset, data + offset) < 0)
			continue;
		if(offset > range)
			kpanic("impossible change descriptor structure!");
		range -= offset;
	}
	
	/* if we didn't touch anything, go ahead and use chdesc_create_full() */
	if(range == block->ddesc->length)
		return chdesc_create_full(block, owner, data, head);
	
	/* if there's anything left, it is an error... */
	if(range)
		kpanic("%s() called on non-layered block!\n", __FUNCTION__);
	
	return 0;
}

/* Take two byte arrays of size 'length' and create byte chdescs for
 * non-consecutive ranges that differ. */
/* FIXME: get rid of the olddata parameter here, and just use the block's data as the old data */
int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head)
{
	int i = 0, r, start;
	uint8_t * old = (uint8_t *) olddata;
	uint8_t * new = (uint8_t *) newdata;
	vector_t * oldheads;

	if(!old || !new || !head || length < 1)
		return -E_INVAL;

	oldheads = vector_create();
	if(!oldheads)
		return -E_NO_MEM;

	while(i < length)
	{
		chdesc_t * oldhead = *head; /* use the original head */

		if(old[i] == new[i])
		{
			i++;
			continue;
		}
		
		start = i;
		while(i < length && old[i] != new[i])
			i++;

		r = chdesc_create_byte(block, owner, offset + start, i - start, new + start, &oldhead);
		if(r < 0)
			goto chdesc_create_diff_failed;

		if(oldhead)
		{
			r = vector_push_back(oldheads, oldhead);
			if(r < 0)
			{
				chdesc_remove_depend(oldhead, *head);
				chdesc_destroy(&oldhead);
				goto chdesc_create_diff_failed;
			}
		}
	}

	i = vector_size(oldheads);
	if(i == 1)
		*head = (chdesc_t *) vector_elt_front(oldheads);
	else if(i > 1)
	{
		/* *head depends on all created chdescs */
		r = chdesc_create_noop_array(NULL, head, i, (chdesc_t **) oldheads->elts);
		if(r < 0)
			goto chdesc_create_diff_failed;
	}
	vector_destroy(oldheads);
	return i;

chdesc_create_diff_failed:
	for(i = 0; i < vector_size(oldheads); i++)
	{
		chdesc_t * oldhead = (chdesc_t *) vector_elt(oldheads, i);
		chdesc_remove_depend(oldhead, *head);
		chdesc_destroy(&oldhead);
	}
	vector_destroy(oldheads);
	return r;
}
