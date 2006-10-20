#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/panic.h>
#include <lib/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/revision.h>
#include <kfs/barrier.h>

#define BARRIER_DEBUG 0

#if BARRIER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

int barrier_simple_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block)
{
	bool synthetic;
	bool chdescs_moved = 0;
	bdesc_t * target_block;
	chdesc_t ** chptr;
	int r;

	if (!block->ddesc->all_changes)
		return 0;

	target_block = CALL(target, synthetic_read_block, number, 1, &synthetic);
	if (!target_block)
		return -E_UNSPECIFIED;

	if (block == target_block)
	{
		Dprintf("%s(): block == target_block (0x%08x)\n", __FUNCTION__, block);
		return 0;
	}

	/* prepare the block for chdesc forwarding */
	r = revision_tail_prepare(block, barrier);
	if (r < 0)
	{
		if(synthetic)
			CALL(target, cancel_block, number);
		return r;
	}

	/* transfer the barrier's bottom chdescs on block to target_block.
	 * this loop makes use of knowledge of how chdesc_move operates. */
	chptr = &block->ddesc->all_changes;
	while (*chptr)
	{
		chdesc_t * chdesc = *chptr;
		if (chdesc->owner == barrier && !(chdesc->flags & CHDESC_ROLLBACK))
		{
			chdescs_moved = 1;
			r = chdesc_move(chdesc, target_block, target, 0);
			if (r < 0)
				panic("%s(): chdesc_move() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
		}
		else
			chptr = &chdesc->ddesc_next;
	}
	if (chdescs_moved)
		chdesc_finish_move(target_block);

	if (!chdescs_moved && synthetic)
	{
		/* With no changes for this synthetic target_block, we might as well
		 * cancel the block */
		r = CALL(target, cancel_block, number);
		if (r < 0)
			panic("%s(): BD::cancel_block() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
	}
	else if (chdescs_moved)
	{
		/* Bring target_block's data blob up to date with the transferred
		 * chdescs */
		assert(target_block->ddesc->length == block->ddesc->length);
		memcpy(target_block->ddesc->data, block->ddesc->data, block->ddesc->length);

		/* write the updated target_block */
		r = CALL(target, write_block, target_block);
		if (r < 0)
			panic("%s(): target->write_block() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
	}

	/* put block back into current state */
	r = revision_tail_revert(block, barrier);
	if (r < 0)
		panic("%s(): revision_tail_revert() failed (%i), but this function does not know what to do in case of failure", __FUNCTION__, r);

	return 0;
}

int barrier_multiple_forward(multiple_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block)
{
	bool * synthetic;
	bool chdescs_moved = 0;
	bdesc_t ** target_block;
	chdesc_t ** chptr;
	int i, r;
	
	if(nforwards < 2)
		return -E_INVAL;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	synthetic = malloc(sizeof(*synthetic) * nforwards);
	if(!synthetic)
		return -E_NO_MEM;
	
	target_block = malloc(sizeof(*target_block) * nforwards);
	if(!target_block)
	{
		free(synthetic);
		return -E_NO_MEM;
	}
	
	for(i = 0; i != nforwards; i++)
	{
		target_block[i] = CALL(forwards[i].target, synthetic_read_block, forwards[i].number, 1, &synthetic[i]);
		if(!target_block[i])
		{
			while(i--)
				if(synthetic[i])
					CALL(forwards[i].target, cancel_block, forwards[i].number);
			free(target_block);
			free(synthetic);
			return -E_UNSPECIFIED;
		}
		if(block == target_block[i])
			panic("%s(): block == target_block[%d] (%p)", __FUNCTION__, i, block);
	}
	
	/* prepare the block for chdesc forwarding */
	r = revision_tail_prepare(block, barrier);
	if(r < 0)
	{
		for(i = 0; i != nforwards; i++)
			if(synthetic[i])
				CALL(forwards[i].target, cancel_block, forwards[i].number);
		free(target_block);
		free(synthetic);
		return r;
	}
	
	/* transfer the barrier's bottom chdescs on block to target_block.
	 * this loop makes use of knowledge of how chdesc_duplicate operates. */
	chptr = &block->ddesc->all_changes;
	while(*chptr)
	{
		chdesc_t * chdesc = *chptr;
		if(chdesc->owner == barrier && !(chdesc->flags & CHDESC_ROLLBACK))
		{
			chdescs_moved = 1;
			r = chdesc_duplicate(chdesc, nforwards, target_block);
			if (r < 0)
				panic("%s(): chdesc_duplicate() failed (%i), but chdesc revert-duplicate code for recovery is not implemented", __FUNCTION__, r);
		}
		else
			chptr = &chdesc->ddesc_next;
	}
	if(chdescs_moved)
	{
		for(i = 0; i != nforwards; i++)
		{
			chdesc_finish_move(target_block[i]);
			
			/* Bring the target_block's data blob up to date with the
			 * duplicated chdescs */
			assert(target_block[i]->ddesc->length == block->ddesc->length);
			memcpy(target_block[i]->ddesc->data, block->ddesc->data, block->ddesc->length);
			r = chdesc_push_down(barrier, target_block[i], forwards[i].target, target_block[i]);
			assert(r >= 0);
			
			/* write the updated target_block */
			r = CALL(forwards[i].target, write_block, target_block[i]);
			if (r < 0)
				panic("%s(): target->write_block() failed (%i), but chdesc revert-duplicate code for recovery is not implemented", __FUNCTION__, r);
		}
	}
	else
	{
		/* With no changes for a synthetic target_block, we might as
		 * well cancel the block */
		for(i = 0; i != nforwards; i++)
			if(synthetic[i])
			{
				r = CALL(forwards[i].target, cancel_block, forwards[i].number);
				if (r < 0)
					panic("%s(): BD::cancel_block() failed (%i), but chdesc revert-duplicate code for recovery is not implemented", __FUNCTION__, r);
			}
	}
	
	free(target_block);
	free(synthetic);
	
	/* put block back into current state */
	r = revision_tail_revert(block, barrier);
	if(r < 0)
		panic("%s(): revision_tail_revert() failed (%i), but this function does not know what to do in case of failure", __FUNCTION__, r);

	return 0;
}
