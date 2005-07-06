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

/* chdesc_rollback_collection */
/* Roll back a collection of change descriptors on the same block. They will be
 * rolled back in proper dependency order. */
int chdesc_rollback_collection(int count, chdesc_t ** chdescs, void ** order)
{
	return -1;
}

/* chdesc_apply_collection */
/* Apply a collection of change descriptors on the same block. They will be
 * applied in proper dependency order. */
int chdesc_apply_collection(int count, chdesc_t ** chdescs, void ** order)
{
	return -1;
}

/* chdesc_duplicate */
/* Duplicate a change descriptor to two or more blocks. The original change
 * descriptor will be turned into a NOOP change descriptor which depends on all
 * the duplicates, each of which will be applied to a different block. */

/* chdesc_split */
/* Split a change descriptor into two or more change descriptors. The original
 * change descriptor will be turned into a NOOP change descriptor which depends
 * on all the fragments, the first of which will represent the original change
 * while the others are just NOOP change descriptors. */

/* chdesc_merge */
/* Merge many change descriptors into a small, nonoverlapping set of new ones.
 * The change descriptors must all be on the same block and have the same owner,
 * which should also be at the bottom of a barrier zone. The resulting change
 * descriptors will be byte change descriptors for the entire block. */
int chdesc_merge(int count, chdesc_t ** chdescs, chdesc_t ** head, chdesc_t ** tail)
{
	int i, r;
	void * data;
	
	/* we need at least 2 change descriptors */
	if(count < 1)
		return -E_INVAL;
	if(count == 1)
		return 0;
	
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
	
	/* copy the new data */
	data = memdup(chdescs[0]->block->ddesc->data, chdescs[0]->block->ddesc->length);
	
	/* now roll back the change descriptors */
	r = chdesc_rollback_collection(count, chdescs, NULL);
	if(r < 0)
		return r;
	
	r = chdesc_create_full(chdescs[0]->block, chdescs[0]->owner, data, head, tail);
	if(r < 0)
	{
		/* roll forward */
		chdesc_apply_collection(count, chdescs, NULL);
		return r;
	}
	
	/* FIXME not done yet... */
	return 0;
}
