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

/* Write an entire block with new data, assuming that either A) no change
 * descriptors exist on the block or B) the entire block has a single BYTE
 * change descriptor on it. In case B, use chdesc_rewrite_byte() to rewrite
 * the existing change descriptor to reflect the new data, and return NULL
 * in *head. In case A, return the newly created change descriptor in *head. */
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	chdesc_t * rewrite = block->level_changes[owner->level].head;
	
	if(!rewrite || rewrite->type != BYTE || rewrite->byte.offset || rewrite->byte.length != block->length || (rewrite->flags & CHDESC_INFLIGHT))
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
int chdesc_create_diff_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** tail, chdesc_pass_set_t * befores)
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
	
	r = chdesc_create_byte_set(block, owner, offset + start, end - start + 1, &new[start], tail, befores);
	if(r < 0)
		return r;
	return 1;
}

int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return chdesc_create_diff_set(block, owner, offset, length, olddata, newdata, head, PASS_CHDESC_SET(set));
}
