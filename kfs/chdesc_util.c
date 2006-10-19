#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/memdup.h>
#include <lib/panic.h>

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
	if(target_block->ddesc != current_block->ddesc)
		return -E_INVAL;
	if(current_block->ddesc->all_changes)
	{
		chdesc_t * chdesc;
		for (chdesc = current_block->ddesc->all_changes;
		     chdesc;
		     chdesc = chdesc->ddesc_next)
		{
			if(chdesc->owner == current_bd)
			{
				uint16_t prev_level = chdesc_level(chdesc);
				uint16_t new_level;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, chdesc, target_bd);
				chdesc_unlink_ready_changes(chdesc);
				chdesc->owner = target_bd;
				assert(chdesc->block);
				bdesc_release(&chdesc->block);
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, chdesc, target_block);
				chdesc->block = target_block;
				chdesc_update_ready_changes(chdesc);
				bdesc_retain(target_block);

				new_level = chdesc_level(chdesc);
				if(prev_level != new_level)
					chdesc_propagate_level_change(chdesc, prev_level, new_level);
			}
		}
	}
	return 0;
}

/* Move a change descriptor from one block to another, with the target block
 * having a different data descriptor than the source block. This is intended
 * for use at barriers, by the barrier code. The target block's data is not
 * updated to reflect the presence of the change descriptor, and must be copied
 * manually from the source data descriptor. Also, the CHDESC_MOVED flag will be
 * set on the change descriptor if the destination block is non-NULL. */
int chdesc_move(chdesc_t * chdesc, bdesc_t * destination, BD_t * target_bd, uint16_t source_offset)
{
	chdesc_t * bit_changes = NULL;
	uint16_t * offset;
	int r;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_MOVE, chdesc, destination, target_bd, source_offset);
	
	/* source_offset is in bytes for all chdesc types */
	switch(chdesc->type)
	{
		case BIT:
			if(source_offset & 0x3)
				return -E_INVAL;
			source_offset >>= 2;
			offset = &chdesc->bit.offset;
			break;
		case BYTE:
			offset = &chdesc->byte.offset;
			break;
		case NOOP:
			offset = NULL;
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	if(offset && source_offset > *offset)
		return -E_INVAL;
	
	if(!destination && chdesc->type != NOOP)
		return -E_INVAL;
	
	if(destination)
	{
		if(chdesc->type == BYTE)
		{
			r = __ensure_bdesc_has_overlaps(destination);
			if(r < 0)
				return r;
			r = __chdesc_add_depend_fast(destination->ddesc->overlaps, chdesc);
			if(r < 0)
				return r;
		}
		else if(chdesc->type == BIT)
		{
			bit_changes = __ensure_bdesc_has_bit_changes(destination, *offset - source_offset);
			if(!bit_changes)
				return -E_NO_MEM;
			r = __chdesc_add_depend_fast(bit_changes, chdesc);
			if(r < 0)
				return r;
		}
		
		/* set CHDESC_MOVED here to prevent trying to overlap attach to ourselves */
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_MOVED);
		chdesc->flags |= CHDESC_MOVED;

		if(offset)
		{
			*offset -= source_offset;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OFFSET, chdesc, *offset);
		}

		r = __chdesc_overlap_multiattach(chdesc, destination);
		if(r < 0)
		{
			if(offset)
			{
				*offset += source_offset;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OFFSET, chdesc, *offset);
			}
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdesc, CHDESC_MOVED);
			chdesc->flags &= ~CHDESC_MOVED;
			if(chdesc->type == BYTE)
				chdesc_remove_depend(destination->ddesc->overlaps, chdesc);
			else if(chdesc->type == BIT)
				chdesc_remove_depend(bit_changes, chdesc);
			return r;
		}
	}
	
	/* at this point we have succeeded in moving the chdesc */
	chdesc_unlink_ready_changes(chdesc);
	chdesc_unlink_all_changes(chdesc);
	if(chdesc->block)
	{
		if(chdesc->type == BYTE)
			chdesc_remove_depend(chdesc->block->ddesc->overlaps, chdesc);
		else if(chdesc->type == BIT)
		{
			bit_changes = __chdesc_bit_changes(chdesc->block, chdesc->bit.offset + source_offset);
			chdesc_remove_depend(bit_changes, chdesc);
		}
		bdesc_release(&chdesc->block);
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, chdesc, target_bd);
	uint16_t prev_level = chdesc_level(chdesc);
	uint16_t new_level;
	chdesc->owner = target_bd;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, chdesc, destination);
	chdesc->block = destination;
	if(destination)
		bdesc_retain(destination);
	chdesc_link_all_changes(chdesc);
	chdesc_update_ready_changes(chdesc);

	panic("This function needs to be updated/tested to work with the ready_changes list");
	
	new_level = chdesc_level(chdesc);
	if(prev_level != new_level)
		chdesc_propagate_level_change(chdesc, prev_level, new_level);

	return 0;
}

void chdesc_finish_move(bdesc_t * destination)
{
	if(destination->ddesc->all_changes)
	{
		chdesc_t * scan;
		for (scan = destination->ddesc->all_changes; scan; scan = scan->ddesc_next)
		{
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, scan, CHDESC_MOVED);
			scan->flags &= ~CHDESC_MOVED;
		}
	}
}

int chdesc_noop_reassign(chdesc_t * noop, bdesc_t * block)
{
	if(noop->type != NOOP)
		return -E_INVAL;
	
	/* special case for reassigning to the same ddesc */
	if(noop->block && block && noop->block->ddesc == block->ddesc)
	{
		if(noop->block != block)
		{
			bdesc_retain(block);
			bdesc_release(&noop->block);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, noop, block);
			noop->block = block;
		}
		return 0;
	}

#if BDESC_EXTERN_AFTER_COUNT	
	panic("NOOP ddesc change support needs bdesc extern_after_count support");
#endif
	
	if(noop->block)
	{
		chdesc_unlink_ready_changes(noop);
		chdesc_unlink_all_changes(noop);
		bdesc_release(&noop->block);
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, noop, block);
	noop->block = block;
	if(block)
	{
		bdesc_retain(block);
		chdesc_link_all_changes(noop);
		chdesc_update_ready_changes(noop);
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
	
	if(*head)
	{
		kdprintf(STDERR_FILENO, "%s:%d %s() called with non-null *head!\n", __FILE__, __LINE__, __FUNCTION__);
		return -E_PERM;
	}
	if(!block->ddesc->all_changes)
		return chdesc_create_full(block, owner, data, head);
	
	/* FIXME: check for some other cases when we should just fall back on chdesc_create_full() */
	
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
	{
		uint16_t offset;
		if(scan->type != BYTE)
			continue;
		offset = scan->byte.length;
		if(chdesc_rewrite_byte(scan, 0, offset, data + offset) < 0)
			continue;
		if(offset > range)
			panic("impossible change descriptor structure!");
		range -= offset;
	}
	
	/* if we didn't touch anything, go ahead and use chdesc_create_full() */
	if(range == block->ddesc->length)
		return chdesc_create_full(block, owner, data, head);
	
	/* if there's anything left, it is an error... */
	if(range)
		panic("%s() called on non-layered block!\n", __FUNCTION__);
	
	return 0;
}

/* Roll back a collection of change descriptors on the same block. They will be
 * rolled back in proper dependency order. */
/* If "order" is non-null, and it is a pointer to NULL, it will be filled in
 * with a pointer to information which can be used by chdesc_apply_collection()
 * to apply the same change descriptors again without having to recompute the
 * proper order. If it is non-null and a pointer to non-null, the order
 * specified will be used by chdesc_rollback_collection() itself. To free the
 * memory used by the structure, use chdesc_order_destroy(). When "order" is
 * NULL, the structure will be computed internally and freed automatically. */
int chdesc_rollback_collection(int count, chdesc_t ** chdescs, void ** order)
{
	/* a lot of the code for this should be similar to code in revision.c */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_ROLLBACK_COLLECTION, count, chdescs, order);
	panic("%s() is not written\n", __FUNCTION__);
	return -1;
}

/* Apply a collection of change descriptors on the same block. They will be
 * applied in proper dependency order. */
/* See the comment above about chdesc_rollback_collection() for a description of
 * the "order" parameter. It behaves in the same way for this function. */
int chdesc_apply_collection(int count, chdesc_t ** chdescs, void ** order)
{
	/* a lot of the code for this should be similar to code in revision.c */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_APPLY_COLLECTION, count, chdescs, order);
	panic("%s() is not written\n", __FUNCTION__);
	return -1;
}

void chdesc_order_destroy(void ** order)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_ORDER_DESTROY, order);
	panic("%s() is not written\n", __FUNCTION__);
	free(*order);
	*order = NULL;
}

/* Split a change descriptor into two change descriptors, such that the original
 * change descriptor depends only on a new NOOP change descriptor which has all
 * the befores of the original change descriptor. If the original change
 * descriptor has no befores, or only one before, this function does
 * nothing. */
int chdesc_detach_befores(chdesc_t * chdesc)
{
	int r;
	chdesc_t * tail;
#if BDESC_EXTERN_AFTER_COUNT
	panic("This function needs to be checked for working with ddesc->extern_after_count");
#endif
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_DETACH_BEFORES, chdesc);
	if(!chdesc->befores || !chdesc->befores->before.next)
		return 0;
	r = chdesc_create_noop_list(chdesc->block, chdesc->owner, &tail, NULL);
	if(r < 0)
		return r;
	r = __chdesc_add_depend_fast(chdesc, tail);
	if(r < 0)
	{
		chdesc_destroy(&tail);
		return r;
	}
	while(chdesc->befores->before.desc != tail)
	{
		chdepdesc_t * dep = chdesc->befores;
		chdesc->befores = dep->before.next;
		dep->before.next->before.ptr = &chdesc->befores;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_BEFORE, chdesc, dep->before.desc);
		__propagate_depend_remove(chdesc, dep->before.desc);
		
		dep->before.next = NULL;
		dep->before.ptr = tail->befores_tail;
		*tail->befores_tail = dep;
		tail->befores_tail = &dep->before.next;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_BEFORE, tail, dep->before.desc);
		__propagate_depend_add(tail, dep->before.desc);
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_AFTER, dep->after.desc, chdesc);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_AFTER, dep->after.desc, tail);
		dep->after.desc = tail;
	}
	assert(!chdesc->befores->before.next);
	return 0;
}

/* Split a change descriptor into two change descriptors, such that the orignal
 * change descriptor is depended on only by a new NOOP change descriptor which
 * has all the afters of the original change descriptor. */
int chdesc_detach_afters(chdesc_t * chdesc)
{
	/* this function is a little bit more complicated than the above,
	 * because of the automatic dependencies generated by blocks */
	int r;
	chdepdesc_t ** scan;
	chdesc_t * head;
#if BDESC_EXTERN_AFTER_COUNT
	panic("This function needs to be checked for working with ddesc->extern_after_count");
#endif
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_DETACH_AFTERS, chdesc);
	r = chdesc_create_noop_list(chdesc->block, chdesc->owner, &head, NULL);
	if(r < 0)
		return r;
	r = __chdesc_add_depend_fast(head, chdesc);
	if(r < 0)
	{
		chdesc_destroy(&head);
		return r;
	}
	assert(chdesc->afters->after.desc == head);
	scan = &chdesc->afters;
	while((*scan)->after.desc != head)
	{
		chdepdesc_t * dep = *scan;
		*scan = dep->after.next;
		dep->after.next->after.ptr = scan;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_AFTER, chdesc, dep->after.desc);
		__propagate_depend_remove(dep->after.desc, chdesc);
		
		dep->after.next = NULL;
		dep->after.ptr = head->afters_tail;
		*head->afters_tail = dep;
		head->afters_tail = &dep->after.next;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_AFTER, head, dep->after.desc);
		__propagate_depend_add(dep->after.desc, head);
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_BEFORE, dep->before.desc, chdesc);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_BEFORE, dep->before.desc, head);
		dep->before.desc = head;
	}
	return 0;
}

/* Duplicate a change descriptor to two or more blocks. The original change
 * descriptor will be turned into a NOOP change descriptor which depends on all
 * the duplicates, each of which will be attached to a different block. Just as
 * in chdesc_move(), the data descriptors will not be updated to reflect this
 * addition, and the CHDESC_MOVED flag will be set on the duplicate change
 * descriptors. Note that the calling BD still owns the duplicate change
 * descriptors, and must push them to the appropriate new owners itself. If this
 * function fails, the change descriptor graph may be left altered in a form
 * which is semantically equivalent to the original state. It is assumed by this
 * function that the caller has verified that the atomic sizes will work out. */
/*  From:      To:                    Or:
 *   -> X       -> M1 \      -> X            -> X
 *  /          /       \    /               /
 * W -> Y     w -> M2 ---> p -> Y     W -> p -> Y
 *  \          \       /    \               \
 *   -> Z       -> M3 /      -> Z            -> Z
 * */
int chdesc_duplicate(chdesc_t * original, int count, bdesc_t ** blocks)
{
	int i, r;
	chdesc_t * tail = NULL;
	size_t descs_size;
	chdesc_t ** descs;
	
	panic("This function needs to be updated to work with ddesc->overlaps and ddesc->bit_changes");
	panic("This function needs to be updated to work with chdesc->nbefores");
	
	if(count < 2)
		return -E_INVAL;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_DUPLICATE, original, count, blocks);
	
	/* if the original is a NOOP, we don't need to duplicate it...
	 * just make sure it has no block and return successfully */
	if(original->type == NOOP)
	{
		chdesc_noop_reassign(original, NULL);
		return 0;
	}
	
	/* make sure the blocks are all the same size */
	for(i = 0; i != count; i++)
		if(blocks[i]->ddesc->length != original->block->ddesc->length)
			return -E_INVAL;
	
	descs_size = sizeof(*descs) * count;
	descs = smalloc(descs_size);
	if(!descs)
		return -E_NO_MEM;
	
	/* first detach the befores */
	r = chdesc_detach_befores(original);
	if(r < 0)
	{
		sfree(descs, descs_size);
		return r;
	}
	
	if(original->befores)
	{
		tail = original->befores->before.desc;
		/* can't fail when assigning to NULL */
		r = chdesc_noop_reassign(tail, NULL);
		assert(r >= 0);
	}
	
	/* then create duplicates, depended on by the original and depending on the tail */
	switch(original->type)
	{
		case BIT:
			for(i = 0; i != count; i++)
			{
				r = chdesc_create_noop_list(blocks[i], original->owner, &descs[i], NULL);
				if(r < 0)
					goto fail_first;
				descs[i]->bit.offset = original->bit.offset;
				descs[i]->bit.xor = original->bit.xor;
				descs[i]->type = BIT;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_BIT, descs[i], descs[i]->bit.offset, descs[i]->bit.xor);
				r = __chdesc_overlap_multiattach(descs[i], blocks[i]);
				if(r < 0)
					goto fail_later;
				descs[i]->flags |= CHDESC_MOVED;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, descs[i], CHDESC_MOVED);
				r = chdesc_add_depend(original, descs[i]);
				if(r < 0)
					goto fail_later;
				if(tail)
				{
					r = chdesc_add_depend(descs[i], tail);
					if(r < 0)
						goto fail_later;
				}
			}
			/* change the original to a NOOP with no block */
			original->type = NOOP;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, original);
			r = chdesc_noop_reassign(original, NULL);
			assert(r >= 0);
			break;
		case BYTE:
			for(i = 0; i != count; i++)
			{
				r = chdesc_create_noop_list(blocks[i], original->owner, &descs[i], NULL);
				if(r < 0)
					goto fail_first;
				descs[i]->byte.offset = original->byte.offset;
				descs[i]->byte.length = original->byte.length;
				if(original->byte.data)
				{
					descs[i]->byte.data = memdup(original->byte.data, original->byte.length);
					if(!descs[i]->byte.data)
					{
						r = -E_NO_MEM;
						goto fail_later;
					}
				}
				else
					descs[i]->byte.data = NULL;
				descs[i]->type = BYTE;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_BYTE, descs[i], descs[i]->byte.offset, descs[i]->byte.length);
				descs[i]->flags |= CHDESC_MOVED;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, descs[i], CHDESC_MOVED);
				r = __chdesc_overlap_multiattach(descs[i], blocks[i]);
				if(r < 0)
					goto fail_later;
				r = chdesc_add_depend(original, descs[i]);
				if(r < 0)
					goto fail_later;
				if(tail)
				{
					r = chdesc_add_depend(descs[i], tail);
					if(r < 0)
						goto fail_later;
				}
			}
			/* change the original to a NOOP with no block */
			if(original->byte.data)
			{
				free(original->byte.data);
				original->byte.data = NULL;
			}
			original->type = NOOP;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, original);
			r = chdesc_noop_reassign(original, NULL);
			assert(r >= 0);
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, original->type);
			sfree(descs, descs_size);
			return -E_INVAL;
		fail_first:
			while(i--)
			    fail_later:
				chdesc_destroy(&descs[i]);
			sfree(descs, descs_size);
			return r;
	}
	
	/* finally unhook the original from the tail */
	if(tail)
		chdesc_remove_depend(original, tail);
	
	return 0;
}

/* Split a change descriptor into two or more change descriptors. The original
 * change descriptor will be turned into a NOOP change descriptor which depends
 * on all the fragments, the first of which will represent the original change
 * while the others are just NOOP change descriptors. If this function fails,
 * the change descriptor graph may be left altered in a form which is
 * semantically equivalent to the original state. */
/*  From:      To:                    Or:
 *   -> X       -> M \      -> X            -> X
 *  /          /      \    /               /
 * W -> Y     w -> n ---> p -> Y     W -> p -> Y
 *  \          \      /    \               \
 *   -> Z       -> o /      -> Z            -> Z
 * */
int chdesc_split(chdesc_t * original, int count)
{
	int i, r;
	chdesc_t * tail = NULL;
	size_t descs_size;
	chdesc_t ** descs;
	
	panic("This function needs to be updated to work with ddesc->overlaps and ddesc->bit_changes");
	panic("This function needs to be updated to work with chdesc->nbefores");
	
	if(count < 2)
		return -E_INVAL;
	
	descs_size = sizeof(*descs) * count;
	descs = smalloc(descs_size);
	if(!descs)
		return -E_NO_MEM;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_SPLIT, original, count);
	
	/* First detach the befores of the original change descriptor */
	r = chdesc_detach_befores(original);
	if(r < 0)
	{
		sfree(descs, descs_size);
		return r;
	}
	
	if(original->befores)
		tail = original->befores->before.desc;
	
	/* Now we want to insert the fragments between "original" and "tail" */
	
	for(i = 0; i != count; i++)
	{
		r = chdesc_create_noop_list(original->block, original->owner, &descs[i], NULL);
		if(r < 0)
			goto fail;
		r = __chdesc_add_depend_fast(original, descs[i]);
		if(r >= 0 && tail)
			r = __chdesc_add_depend_fast(descs[i], tail);
		if(r < 0)
		{
			chdesc_destroy(&descs[i]);
		fail:
			while(i--)
				chdesc_destroy(&descs[i]);
			sfree(descs, descs_size);
			return r;
		}
	}
	
	if(tail)
		chdesc_remove_depend(original, tail);
	
	/* Last we want to switch the original with the first fragment */
	descs[0]->type = original->type;
	/* "byte" is larger than "bit" */
	descs[0]->byte = original->byte;
	static_assert(sizeof(original->byte) >= sizeof(original->bit));
	
#if KFS_DEBUG
	switch(descs[0]->type)
	{
		case NOOP:
			/* we switched it with another noop, so there is nothing to report */
			break;
		case BIT:
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_BIT, descs[0], descs[0]->bit.offset, descs[0]->bit.xor);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, original);
			break;
		case BYTE:
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_BYTE, descs[0], descs[0]->byte.offset, descs[0]->byte.length);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, original);
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, descs[0]->type);
	}
#endif
	
	original->type = NOOP;
	
	sfree(descs, descs_size);
	return 0;
}

/* Merge many change descriptors into a small, nonoverlapping set of new ones.
 * The change descriptors must all be on the same block and have the same owner,
 * which should also be at the bottom of a barrier zone. It is expected that the
 * change descriptors being merged have no eventual befores on any other
 * change descriptors on the same block. The resulting change descriptors will
 * be byte change descriptors for the entire block. */
int chdesc_merge(int count, chdesc_t ** chdescs, chdesc_t ** head)
{
	int i, r, newnoop = 0;
	void * data;
	chdepdesc_t * dep;
	chdesc_t * scan;
	chdesc_t * tail;
	
	panic("This function needs to be updated to work with ddesc->overlaps and ddesc->bit_changes");
	panic("This function needs to be updated to work with chdesc before list tails");
	panic("This function needs to be updated to work with chdesc->nbefores");
	
	/* we need at least 2 change descriptors */
	if(count < 1)
		return -E_INVAL;
	if(count == 1)
		return 0;
	
	if(!head)
		return -E_INVAL;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_MERGE, count, chdescs, head);
	
	/* FIXME allow NOOP change descriptors with different blocks */
	/* FIXME allow all NOOP change descriptors? (just merge them) */
	/* make sure the change descriptors are all on the same block */
	for(i = 1; i != count; i++)
		if(chdescs[i - 1]->block->ddesc != chdescs[i]->block->ddesc)
			return -E_INVAL;
	
	/* Now make sure this would not create a loop: as long as none of the
	 * change descriptors are an eventual before of any of the others,
	 * merging them will not create a loop. However, if the entire
	 * before path from one change descriptor to another is going to be
	 * merged anyway, a loop will not result from the merge even if one is
	 * an eventual before of another. To add this exception, we simply
	 * start marking from the befores of the change descriptors in the
	 * set to be merged which are not themselves in the set. This basically
	 * forces any path being considered to contain at least one change
	 * descriptor which will not be merged. */
	
	/* mark all the roots as in the set */
	for(i = 0; i != count; i++)
	{
		chdescs[i]->flags |= CHDESC_INSET;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdescs[i], CHDESC_INSET);
	}
	
	/* start marking change descriptors at the roots */
	for(i = 0; i != count; i++)
	{
		chdepdesc_t * dep;
		/* if one of the roots is already marked, it is an eventual
		 * before of one of the earlier roots */
		if(chdescs[i]->flags & CHDESC_MARKED)
			break;
		for(dep = chdescs[i]->befores; dep; dep = dep->before.next)
			if(!(dep->before.desc->flags & CHDESC_INSET))
				chdesc_mark_graph(dep->before.desc);
	}
	if(i != count)
		/* loop detected... unmark everything and fail */
		goto unmark_fail;
	/* now check them once more, in case an earlier root is an eventual
	 * before of a later root */
	for(i = 0; i != count; i++)
		if(chdescs[i]->flags & CHDESC_MARKED)
			break;
	/* check all change descriptors on the block, to make sure the underlap
	 * below will not create a cycle */
	for(scan = chdescs[0]->block->ddesc->all_changes; scan; scan = scan->ddesc_next)
		if(scan->flags & CHDESC_MARKED)
		{
			/* cause the error check below to catch this */
			i = 0;
			break;
		}
	if(i != count)
	{
		/* loop detected... unmark everything and fail */
	unmark_fail:
		for(i = 0; i != count; i++)
		{
			chdescs[i]->flags &= ~CHDESC_INSET;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdescs[i], CHDESC_INSET);
			chdescs[i]->flags |= CHDESC_MARKED;
			/* we are just about to unmark chdescs[i], so elide reporting it */
			chdesc_unmark_graph(chdescs[i]);
		}
		return -E_INVAL;
	}
	/* no loops, unmark everything and proceed */
	for(i = 0; i != count; i++)
	{
		chdescs[i]->flags &= ~CHDESC_INSET;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdescs[i], CHDESC_INSET);
		/* mark the roots as moved so that the create_full below will
		 * not create befores on them */
		chdescs[i]->flags |= CHDESC_MARKED | CHDESC_MOVED;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdescs[i], CHDESC_MOVED);
		chdesc_unmark_graph(chdescs[i]);
	}
	
	/* copy the new data */
	data = memdup(chdescs[0]->block->ddesc->data, chdescs[0]->block->ddesc->length);
	if(!data)
		/* FIXME unset MOVED? */
		return -E_NO_MEM;
	
	/* now roll back the change descriptors */
	r = chdesc_rollback_collection(count, chdescs, NULL);
	if(r < 0)
	{
		/* FIXME unset MOVED? */
		free(data);
		return r;
	}
	
	if(*head)
		tail = *head;
	else
	{
		r = chdesc_create_noop_list(NULL, NULL, &tail, NULL);
		if(r < 0)
			goto merge_chdesc_create_failed;
		chdesc_claim_noop(tail);
		*head = tail;
		newnoop = 1;
	}

	panic("This code used to call __chdesc_create_full() with 'slip_under' requested. 'slip_under' added to caller and __chdesc_create_full() in r1192. However, slip_under was never used by callee and so __chdesc_create_full() was removed in r2693. TODO: is slip_under needed here?");
	r = chdesc_create_full(chdescs[0]->block, chdescs[0]->owner, data, head);
	free(data);
	if(r < 0)
	{
	merge_chdesc_create_failed:
		/* roll forward */
		chdesc_apply_collection(count, chdescs, NULL);
		for(i = 0; i != count; i++)
		{
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdescs[i], CHDESC_MOVED);
			chdescs[i]->flags &= ~CHDESC_MOVED;
		}
		return r;
	}
	
	assert(*head);
	assert(tail);
	
	/* we have now finished all operations that could potentially fail */
	
	/* now add all the befores of the input descriptors to the tail, and the afters of the input descriptors to the head */
	for(i = 0; i != count; i++)
	{
		chdepdesc_t ** scan;
		
		/* add the befores to tail */
		while(chdescs[i]->befores)
		{
			for(dep = tail->befores; dep; dep = dep->before.next)
				if(dep->before.desc == chdescs[i]->befores->before.desc)
					break;
			if(dep)
				/* we already have this before, so free the duplicate */
				chdesc_remove_depend(chdescs[i], chdescs[i]->befores->before.desc);
			else
			{
				/* move the before pointer */
				dep = chdescs[i]->befores;
				chdescs[i]->befores = dep->before.next;
				if(dep->before.next)
					dep->before.next->before.ptr = &chdescs[i]->befores;
				else
					chdescs[i]->befores_tail = &chdescs[i]->befores;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_BEFORE, chdescs[i], dep->before.desc);
				dep->before.next = NULL;
				dep->before.ptr = tail->befores_tail;
				*tail->befores_tail = dep;
				tail->befores_tail = &dep->before.next;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_BEFORE, tail, dep->before.desc);
				/* set the after pointer */
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_AFTER, dep->after.desc, chdescs[i]);
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_AFTER, dep->after.desc, tail);
				dep->after.desc = tail;
			}
		}
		if(newnoop)
			chdesc_autorelease_noop(tail);

		/* add the afters to head */
		scan = &chdescs[i]->afters;
		while(*scan)
		{
			for(dep = (*head)->afters; dep; dep = dep->after.next)
				if(dep->after.desc == (*scan)->after.desc)
					break;
			if(dep)
				/* we already have this after, so free the duplicate */
				chdesc_remove_depend((*scan)->after.desc, chdescs[i]);
			else
			{
				/* move the after pointer */
				dep = *scan;
				*scan = dep->after.next;
				if(dep->after.next)
					dep->after.next->after.ptr = scan;
				else
					dep->before.desc->afters_tail = scan;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_AFTER, chdescs[i], dep->after.desc);
				dep->after.next = NULL;
				dep->after.ptr = (*head)->afters_tail;
				*(*head)->afters_tail = dep;
				(*head)->afters_tail = &dep->after.next;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_AFTER, *head, dep->after.desc);
				/* set the before pointer */
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_BEFORE, dep->before.desc, chdescs[i]);
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_BEFORE, dep->before.desc, *head);
				dep->before.desc = *head;
			}
		}
		
		/* add all the weak references to head (except our own, which is the first) */
		assert(chdescs[i]->weak_refs);
		assert(chdescs[i]->weak_refs->desc == &chdescs[i]);
		while(chdescs[i]->weak_refs->next)
		{
			chrefdesc_t * ref = chdescs[i]->weak_refs->next;
			chdescs[i]->weak_refs->next = ref->next;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_FORGET, chdescs[i], ref->desc);
			/* this test should not really be necessary, but for safety... */
			if(*ref->desc == chdescs[i])
				*ref->desc = *head;
			ref->next = (*head)->weak_refs;
			(*head)->weak_refs = ref;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_RETAIN, *head, ref->desc);
		}
	}
	
	/* FIXME we may not know about all pointers to these, so make them all NOOPs that depend on the new merged chdesc: note that this can fail, so we have to move it up... */
	/* finally delete the original change descriptors */
	for(i = 0; i != count; i++)
		if(!(chdescs[i]->flags & CHDESC_WRITTEN))
			chdesc_destroy(&chdescs[i]);
	
	return 0;
}

/* Take two byte arrays of size 'length' and create byte chdescs for
 * non-consecutive ranges that differ. */
/* FIXME: get rid of the olddata parameter here, and just use the block's data as the old data */
int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head)
{
	int count = 0, i = 0, r, start;
	uint8_t * old = (uint8_t *) olddata;
	uint8_t * new = (uint8_t *) newdata;
	chdesc_t * oldhead;
	chdesc_t * newhead;
	
	if(!old || !new || !head || length < 1)
		return -E_INVAL;

	/* newhead will depend on all created chdescs */
	r = chdesc_create_noop_list(NULL, NULL, &newhead, NULL);
	if(r < 0)
		return r;

	while(i < length)
	{
		if(old[i] == new[i])
		{
			i++;
			continue;
		}
		
		start = i;
		while(i < length && old[i] != new[i])
			i++;

		/* use the original head */
		oldhead = *head;
		r = chdesc_create_byte(block, owner, offset + start, i - start, new + start, &oldhead);
		if(r < 0)
			goto chdesc_create_diff_failed;

		/* add before to newly created chdesc */
		r = __chdesc_add_depend_fast(newhead, oldhead);
		if(r < 0)
			goto chdesc_create_diff_failed;
		count++;
	}

	if(count == 1)
	{
		chdesc_remove_depend(newhead, oldhead); /* no longer needed */
		*head = oldhead;
	}
	else if (count > 1)
		*head = newhead;
	return 0;

chdesc_create_diff_failed:
	/* we can't just use chdesc_destroy(), because some of the
	 * regions above may have crossed atomic boundaries... */
	panic("%s() failed, and we don't know how to recover!\n", __FUNCTION__);
	return r;
}

/* Create two noops, one of which prevents the other from being satisfied. */
int chdesc_create_blocked_noop(chdesc_t ** noophead, chdesc_t ** drain_plug)
{
	int r;

	if (!noophead || !drain_plug)
		return -E_INVAL;

	r = chdesc_create_noop_list(NULL, NULL, noophead, NULL);
	if (r < 0)
		return r;
	r = chdesc_create_noop_list(NULL, NULL, drain_plug, NULL);
	if (r < 0)
		return r;
	chdesc_claim_noop(*drain_plug);
	r = chdesc_add_depend(*noophead, *drain_plug);
	if (r < 0) {
		chdesc_autorelease_noop(*drain_plug);
		return r;
	}

	return 0;
}
