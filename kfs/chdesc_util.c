#include <inc/stdio.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/chdesc_util.h>

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

/* chdesc_apply_collection */
/* Apply a collection of change descriptors on the same block. They will be
 * applied in proper dependency order. */

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
/* Merge many change descriptors into a single new one. The change descriptors
 * must all be on the same block. The resulting change descriptor will be a byte
 * change descriptor for the entire block. */
