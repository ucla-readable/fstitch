#include <inc/stdio.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/chdesc_util.h>

void chdesc_mark_graph(chdesc_t * root)
{
	chmetadesc_t * meta;
	root->flags |= CHDESC_MARKED;
	for(meta = root->dependencies; meta; meta = meta->next)
		if(!(meta->desc->flags & CHDESC_MARKED))
			chdesc_mark_graph(meta->desc);
}

void chdesc_unmark_graph(chdesc_t * root)
{
	chmetadesc_t * meta;
	root->flags &= ~CHDESC_MARKED;
	for(meta = root->dependencies; meta; meta = meta->next)
		if(meta->desc->flags & CHDESC_MARKED)
			chdesc_unmark_graph(meta->desc);
}

int chdesc_push_down(BD_t * current_bd, bdesc_t * current_block, BD_t * target_bd, bdesc_t * target_block)
{
	chdesc_t * root = current_block->ddesc->changes;
	if(target_block->ddesc != current_block->ddesc)
		return -E_INVAL;
	if(root)
	{
		chmetadesc_t * scan = root->dependencies;
		while(scan)
		{
			chdesc_t * chdesc = scan->desc;
			if(chdesc->owner == current_bd)
			{
				chdesc->owner = target_bd;
				assert(chdesc->block);
				bdesc_release(&chdesc->block);
				chdesc->block = target_block;
				bdesc_retain(target_block);
			}
			scan = scan->next;
		}
	}
	return 0;
}

/* Move a change descriptor from one block to another, with the target block
 * having a different data descriptor than the source block. This is intended
 * for use at barriers, by the barrier code. The target block's data is not
 * updated to reflect the presence of the change descriptor, and must be copied
 * manually from the source data descriptor. Also, the CHDESC_MOVED flag will be
 * set on the change descriptor. */
int chdesc_move(chdesc_t * chdesc, bdesc_t * destination, BD_t * target_bd, uint16_t source_offset)
{
	uint16_t * offset;
	int r;
	
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
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	if(offset && source_offset > *offset)
		return -E_INVAL;
	
	if(!destination && chdesc->type != NOOP)
		return -E_INVAL;
	
	if(destination)
	{
		r = __ensure_bdesc_has_changes(destination);
		if(r < 0)
			return r;
		
		r = __chdesc_add_depend_fast(destination->ddesc->changes, chdesc);
		if(r < 0)
		{
		    kill_stub:
			if(!destination->ddesc->changes->dependencies)
				chdesc_destroy(&(destination->ddesc->changes));
			return r;
		}
		
		r = __chdesc_overlap_multiattach(chdesc, destination);
		if(r < 0)
		{
			chdesc_remove_depend(destination->ddesc->changes, chdesc);
			goto kill_stub;
		}
	}
	
	/* at this point we have succeeded in moving the chdesc */
	
	if(offset)
		*offset -= source_offset;
	
	chdesc->flags |= CHDESC_MOVED;
	if(chdesc->block)
	{
		chdesc_remove_depend(chdesc->block->ddesc->changes, chdesc);
		bdesc_release(&chdesc->block);
	}
	chdesc->owner = target_bd;
	chdesc->block = destination;
	if(destination)
		bdesc_retain(destination);
	
	return 0;
}

void chdesc_finish_move(bdesc_t * destination)
{
	if(destination->ddesc->changes)
	{
		chmetadesc_t * scan = destination->ddesc->changes->dependencies;
		while(scan)
		{
			scan->desc->flags &= ~CHDESC_MOVED;
			scan = scan->next;
		}
	}
}

int chdesc_noop_reassign(chdesc_t * noop, bdesc_t * block)
{
	if(noop->type != NOOP)
		return -E_INVAL;
	if(block)
	{
		int r = __ensure_bdesc_has_changes(block);
		if(r < 0)
			return r;
		
		r = __chdesc_add_depend_fast(block->ddesc->changes, noop);
		if(r < 0)
		{
			if(!block->ddesc->changes->dependencies)
				chdesc_destroy(&(block->ddesc->changes));
			return r;
		}
	}
	if(noop->block)
	{
		chdesc_remove_depend(noop->block->ddesc->changes, noop);
		bdesc_release(&noop->block);
	}
	noop->block = block;
	if(block)
		bdesc_retain(block);
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
#warning write this
	/* a lot of the code for this should be similar to code in revision.c */
	return -1;
}

/* Apply a collection of change descriptors on the same block. They will be
 * applied in proper dependency order. */
/* See the comment above about chdesc_rollback_collection() for a description of
 * the "order" parameter. It behaves in the same way for this function. */
int chdesc_apply_collection(int count, chdesc_t ** chdescs, void ** order)
{
#warning write this
	/* a lot of the code for this should be similar to code in revision.c */
	return -1;
}

void chdesc_order_destroy(void ** order)
{
#warning finish this
	free(*order);
	*order = NULL;
}

/* Split a change descriptor into two change descriptors, such that the original
 * change descriptor depends only on a new NOOP change descriptor which has all
 * the dependencies of the original change descriptor. */
int chdesc_detach_dependencies(chdesc_t * chdesc)
{
	int r;
	chdesc_t * tail = chdesc_create_noop(chdesc->block, chdesc->owner);
	if(!tail)
		return -E_NO_MEM;
	r = __chdesc_add_depend_fast(chdesc, tail);
	if(r < 0)
	{
		chdesc_destroy(&tail);
		return r;
	}
	assert(chdesc->dependencies->desc == tail);
	while(chdesc->dependencies->next)
	{
		chmetadesc_t * meta = chdesc->dependencies->next;
		chdesc->dependencies->next = meta->next;
		meta->next = tail->dependencies;
		tail->dependencies = meta;
		for(meta = meta->desc->dependents; meta; meta = meta->next)
			if(meta->desc == chdesc)
			{
				meta->desc = tail;
				break;
			}
		assert(meta);
	}
	return 0;
}

/* Split a change descriptor into two change descriptors, such that the orignal
 * change descriptor is depended on only by a new NOOP change descriptor which
 * has all the dependents of the original change descriptor. */
int chdesc_detach_dependents(chdesc_t * chdesc)
{
	/* this function is a little bit more complicated than the above,
	 * because of the automatic dependencies generated by blocks */
	int r;
	chmetadesc_t * scan;
	chdesc_t * skip_noop = NULL;
	chdesc_t * head = chdesc_create_noop(chdesc->block, chdesc->owner);
	if(!head)
		return -E_NO_MEM;
	r = __chdesc_add_depend_fast(head, chdesc);
	if(r < 0)
	{
		chdesc_destroy(&head);
		return r;
	}
	assert(chdesc->dependents->desc == head);
	/* be careful - some NOOP chdescs might not have a block */
	if(chdesc->block)
		skip_noop = chdesc->block->ddesc->changes;
	scan = chdesc->dependents;
	while(scan->next)
	{
		chmetadesc_t * meta = scan->next;
		if(meta->desc == skip_noop)
		{
			scan = meta;
			continue;
		}
		scan->next = meta->next;
		meta->next = head->dependents;
		head->dependents = meta;
		for(meta = meta->desc->dependencies; meta; meta = meta->next)
			if(meta->desc == chdesc)
			{
				meta->desc = head;
				break;
			}
		assert(meta);
	}
	return 0;
}

/* Duplicate a change descriptor to two or more blocks. The original change
 * descriptor will be turned into a NOOP change descriptor which depends on all
 * the duplicates, each of which will be attached to a different block. Just as
 * in chdesc_move(), the data descriptors will not be updated to reflect this
 * addition, and the CHDESC_MOVED flag will be set on the duplicate change
 * descriptors. If this function fails, the change descriptor graph may be left
 * altered in a form which is semantically equivalent to the original state. It
 * is assumed by this function that the caller has verified that the atomic
 * sizes will work out. */
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
	chdesc_t * tail;
	chdesc_t ** descs;
	
	if(count < 2)
		return -E_INVAL;
	
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
	
	descs = malloc(sizeof(*descs) * count);
	if(!descs)
		return -E_NO_MEM;
	
	/* first detach the dependencies */
	r = chdesc_detach_dependencies(original);
	if(r < 0)
	{
		free(descs);
		return r;
	}
	
	tail = original->dependencies->desc;
	
	/* can't fail when assigning to NULL */
	r = chdesc_noop_reassign(tail, NULL);
	assert(r >= 0);
	
	/* then create duplicates, depended on by the original and depending on the tail */
	switch(original->type)
	{
		case BIT:
			for(i = 0; i != count; i++)
			{
				descs[i] = chdesc_create_noop(blocks[i], original->owner);
				if(!descs[i])
				{
					r = -E_NO_MEM;
					goto fail_first;
				}
				descs[i]->bit.xor = original->bit.xor;
				descs[i]->bit.offset = original->bit.offset;
				descs[i]->type = BIT;
				r = __chdesc_overlap_multiattach(descs[i], blocks[i]);
				if(r < 0)
					goto fail_later;
				descs[i]->flags |= CHDESC_MOVED;
				r = chdesc_add_depend(original, descs[i]);
				if(r < 0)
					goto fail_later;
				r = chdesc_add_depend(descs[i], tail);
				if(r < 0)
					goto fail_later;
			}
			/* change the original to a NOOP with no block */
			original->type = NOOP;
			r = chdesc_noop_reassign(original, NULL);
			assert(r >= 0);
			break;
		case BYTE:
			for(i = 0; i != count; i++)
			{
				descs[i] = chdesc_create_noop(blocks[i], original->owner);
				if(!descs[i])
				{
					r = -E_NO_MEM;
					goto fail_first;
				}
				descs[i]->byte.offset = original->byte.offset;
				descs[i]->byte.length = original->byte.length;
				if(original->byte.olddata)
				{
					descs[i]->byte.olddata = memdup(original->byte.olddata, original->byte.length);
					if(!descs[i]->byte.olddata)
					{
						r = -E_NO_MEM;
						goto fail_later;
					}
				}
				else
					descs[i]->byte.olddata = NULL;
				if(original->byte.newdata)
				{
					descs[i]->byte.newdata = memdup(original->byte.newdata, original->byte.length);
					if(!descs[i]->byte.newdata)
					{
						free(descs[i]->byte.olddata);
						r = -E_NO_MEM;
						goto fail_later;
					}
				}
				else
					descs[i]->byte.newdata = NULL;
				descs[i]->type = BYTE;
				r = __chdesc_overlap_multiattach(descs[i], blocks[i]);
				if(r < 0)
					goto fail_later;
				descs[i]->flags |= CHDESC_MOVED;
				r = chdesc_add_depend(original, descs[i]);
				if(r < 0)
					goto fail_later;
				r = chdesc_add_depend(descs[i], tail);
				if(r < 0)
					goto fail_later;
			}
			/* change the original to a NOOP with no block */
			if(original->byte.olddata)
				free(original->byte.olddata);
			if(original->byte.newdata)
				free(original->byte.newdata);
			original->type = NOOP;
			r = chdesc_noop_reassign(original, NULL);
			assert(r >= 0);
			break;
		default:
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, original->type);
			free(descs);
			return -E_INVAL;
		fail_first:
			while(i--)
			    fail_later:
				chdesc_destroy(&descs[i]);
			free(descs);
			return r;
	}
	
	/* finally unhook the original from the tail */
	chdesc_remove_depend(original, tail);
	
	return 0;
}

/* chdesc_morph */
/* Morph a change descriptor while moving it from one barrier zone to another.
 * The expected use of this function is after a chdesc_merge() in barrier
 * modules that change the data as it passes through them, like encryption. */
#warning write this

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
	chdesc_t ** descs;
	chdesc_t * tail;
	
	if(count < 2)
		return -E_INVAL;
	
	descs = malloc(sizeof(*descs) * count);
	if(!descs)
		return -E_NO_MEM;
	
	/* First detach the dependencies of the original change descriptor */
	r = chdesc_detach_dependencies(original);
	if(r < 0)
	{
		free(descs);
		return r;
	}
	
	/* this always exists - we just detached the dependencies */
	tail = original->dependencies->desc;
	
	/* Now we want to insert the fragments between "original" and "tail" */
	
	for(i = 0; i != count; i++)
	{
		descs[i] = chdesc_create_noop(original->block, original->owner);
		if(!descs[i])
		{
			r = -E_NO_MEM;
			goto fail;
		}
		r = __chdesc_add_depend_fast(original, descs[i]);
		if(r >= 0)
			r = __chdesc_add_depend_fast(descs[i], tail);
		if(r < 0)
		{
			chdesc_destroy(&descs[i]);
		fail:
			while(i--)
				chdesc_destroy(&descs[i]);
			free(descs);
			return r;
		}
	}
	
	chdesc_remove_depend(original, tail);
	
	/* Last we want to switch the original with the first fragment */
	descs[0]->type = original->type;
	/* "byte" is larger than "bit" */
	descs[0]->byte = original->byte;
	
	original->type = NOOP;
	
	free(descs);
	return 0;
}

/* Merge many change descriptors into a small, nonoverlapping set of new ones.
 * The change descriptors must all be on the same block and have the same owner,
 * which should also be at the bottom of a barrier zone. It is expected that the
 * change descriptors being merged have no eventual dependencies on any other
 * change descriptors on the same block. The resulting change descriptors will
 * be byte change descriptors for the entire block. */
int chdesc_merge(int count, chdesc_t ** chdescs, chdesc_t ** head, chdesc_t ** tail)
{
	int i, r;
	void * data;
	chmetadesc_t * meta;
	
	/* we need at least 2 change descriptors */
	if(count < 1)
		return -E_INVAL;
	if(count == 1)
		return 0;
	
	/* FIXME allow NOOP change descriptors with different blocks */
	/* FIXME allow all NOOP change descriptors? (just merge them) */
	/* make sure the change descriptors are all on the same block */
	for(i = 1; i != count; i++)
		if(chdescs[i - 1]->block->ddesc != chdescs[i]->block->ddesc)
			return -E_INVAL;
	
	/* Now make sure this would not create a loop: as long as none of the
	 * change descriptors are an eventual dependency of any of the others,
	 * merging them will not create a loop. However, if the entire
	 * dependency path from one change descriptor to another is going to be
	 * merged anyway, a loop will not result from the merge even if one is
	 * an eventual dependency of another. To add this exception, we simply
	 * start marking from the dependencies of the change descriptors in the
	 * set to be merged which are not themselves in the set. This basically
	 * forces any path being considered to contain at least one change
	 * descriptor which will not be merged. */
	
	/* mark all the roots as in the set */
	for(i = 0; i != count; i++)
		chdescs[i]->flags |= CHDESC_INSET;
	
	/* start marking change descriptors at the roots */
	for(i = 0; i != count; i++)
	{
		chmetadesc_t * meta;
		/* if one of the roots is already marked, it is an eventual
		 * dependency of one of the earlier roots */
		if(chdescs[i]->flags & CHDESC_MARKED)
			break;
		for(meta = chdescs[i]->dependencies; meta; meta = meta->next)
			if(!(meta->desc->flags & CHDESC_INSET))
				chdesc_mark_graph(meta->desc);
	}
	if(i != count)
		/* loop detected... unmark everything and fail */
		goto unmark_fail;
	/* now check them once more, in case an earlier root is an eventual
	 * dependency of a later root */
	for(i = 0; i != count; i++)
		if(chdescs[i]->flags & CHDESC_MARKED)
			break;
	/* check all change descriptors on the block, to make sure the underlap
	 * below will not create a cycle */
	for(meta = chdescs[0]->block->ddesc->changes->dependencies; meta; meta = meta->next)
		if(meta->desc->flags & CHDESC_MARKED)
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
			chdescs[i]->flags |= CHDESC_MARKED;
			chdesc_unmark_graph(chdescs[i]);
		}
		return -E_INVAL;
	}
	/* no loops, unmark everything and proceed */
	for(i = 0; i != count; i++)
	{
		chdescs[i]->flags &= ~CHDESC_INSET;
		/* mark the roots as moved so that the create_full below will
		 * not create dependencies on them */
		chdescs[i]->flags |= CHDESC_MARKED | CHDESC_MOVED;
		chdesc_unmark_graph(chdescs[i]);
	}
	
	/* weak retain all the change descriptors, so that when we destroy them
	 * later we won't choke on NOOP chdescs that get automatically freed */
	for(i = 0; i != count; i++)
	{
		r = chdesc_weak_retain(chdescs[i], &chdescs[i]);
		if(r < 0)
		{
			while(i--)
				chdesc_weak_forget(&chdescs[i]);
			return r;
		}
	}
	
	/* copy the new data */
	data = memdup(chdescs[0]->block->ddesc->data, chdescs[0]->block->ddesc->length);
	if(!data)
	{
		for(i = 0; i != count; i++)
			chdesc_weak_forget(&chdescs[i]);
		return -E_NO_MEM;
	}
	
	/* now roll back the change descriptors */
	r = chdesc_rollback_collection(count, chdescs, NULL);
	if(r < 0)
	{
		for(i = 0; i != count; i++)
			chdesc_weak_forget(&chdescs[i]);
		free(data);
		return r;
	}
	
	/* use the hidden "slip under" feature */
	r = __chdesc_create_full(chdescs[0]->block, chdescs[0]->owner, data, head, tail, 1);
	free(data);
	if(r < 0)
	{
		/* roll forward */
		chdesc_apply_collection(count, chdescs, NULL);
		for(i = 0; i != count; i++)
		{
			chdescs[i]->flags &= ~CHDESC_MOVED;
			chdesc_weak_forget(&chdescs[i]);
		}
		return r;
	}
	
	assert(*head);
	assert(*tail);
	
	/* we have now finished all operations that could potentially fail */
	
	/* now add all the dependencies of the input descriptors to the tail, and the dependents of the input descriptors to the head */
	for(i = 0; i != count; i++)
	{
		chmetadesc_t ** scan;
		
		/* add the dependencies to tail */
		while(chdescs[i]->dependencies)
		{
			for(meta = (*tail)->dependencies; meta; meta = meta->next)
				if(meta->desc == chdescs[i]->dependencies->desc)
					break;
			if(meta)
				/* we already have this dependency, so free the duplicate */
				chdesc_remove_depend(chdescs[i], chdescs[i]->dependencies->desc);
			else
			{
				/* move the dependency pointer */
				meta = chdescs[i]->dependencies;
				chdescs[i]->dependencies = meta->next;
				meta->next = (*tail)->dependencies;
				(*tail)->dependencies = meta;
				/* move the dependent pointer */
				for(meta = meta->desc->dependents; meta; meta = meta->next)
					if(meta->desc == chdescs[i])
					{
						meta->desc = *tail;
						break;
					}
			}
			
		}
		
		/* add the dependents to head */
		scan = &chdescs[i]->dependents;
		while(*scan)
		{
			if((*scan)->desc == chdescs[i]->block->ddesc->changes)
				scan = &(*scan)->next;
			else
			{
				for(meta = (*head)->dependents; meta; meta = meta->next)
					if(meta->desc == (*scan)->desc)
						break;
				if(meta)
					/* we already have this dependent, so free the duplicate */
					chdesc_remove_depend((*scan)->desc, chdescs[i]);
				else
				{
					/* move the dependent pointer */
					meta = *scan;
					*scan = meta->next;
					meta->next = (*head)->dependents;
					(*head)->dependents = meta;
					/* move the dependency pointer */
					for(meta = meta->desc->dependencies; meta; meta = meta->next)
						if(meta->desc == chdescs[i])
						{
							meta->desc = *head;
							break;
						}
				}
			}
		}
		
		/* add all the weak references to head (except our own, which is the first) */
		assert(chdescs[0]->weak_refs);
		assert(chdescs[0]->weak_refs->desc == &chdescs[i]);
		while(chdescs[0]->weak_refs->next)
		{
			chrefdesc_t * ref = chdescs[0]->weak_refs->next;
			chdescs[0]->weak_refs->next = ref->next;
			/* this test should not really be necessary, but for safety... */
			if(*ref->desc == chdescs[0])
				*ref->desc = *head;
			ref->next = (*head)->weak_refs;
			(*head)->weak_refs = ref;
		}
	}
	
	/* finally delete the original change descriptors */
	for(i = 0; i != count; i++)
		if(chdescs[i])
			chdesc_destroy(&chdescs[i]);
	
	return 0;
}
