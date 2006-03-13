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

/* barrier_simple_forward() is the single-case version of
 * barrier_partial_forward(). However, barrier_simple_forward() is able
 * to recover on BD::synthetic_read_block() failure and does slightly fewer
 * pointer dereferences. More importantly, it will likely be easier to
 * optimize the revision_*() calls away for the !synthetic case in
 * barrier_simple_forward. */
int barrier_simple_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block)
{
	bool synthetic;
	bool chdescs_moved = 0;
	bdesc_t * target_block;
	chmetadesc_t ** chmetadesc;
	int r;

	if (!block->ddesc->changes)
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
	chmetadesc = &block->ddesc->changes->dependencies;
	while (block->ddesc->changes && *chmetadesc)
	{
		chdesc_t * chdesc = (*chmetadesc)->desc;
		if (chdesc->owner == barrier && !(chdesc->flags & CHDESC_ROLLBACK))
		{
			chdescs_moved = 1;
			r = chdesc_move(chdesc, target_block, target, 0);
			if (r < 0)
				panic("%s(): chdesc_move() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
		}
		else
			chmetadesc = &(*chmetadesc)->next;
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

static bool chdesc_in_range(chdesc_t * chdesc, uint16_t offset, uint16_t size)
{
	uint16_t chd_offset, chd_end;
	/* note that we require that change descriptors do not cross the
	 * atomic disk unit size boundary, so that we will never have to
	 * fragment a change descriptor */
	switch(chdesc->type)
	{
		case BIT:
			chd_offset = chdesc->bit.offset * sizeof(chdesc->bit.xor);
			chd_end = chd_offset + sizeof(chdesc->bit.xor);
			break;
		case BYTE:
			chd_offset = chdesc->byte.offset;
			chd_end = chd_offset + chdesc->byte.length;
			break;
		case NOOP:
			printf("%s(): translating NOOP chdesc\n", __FUNCTION__);
			/* assume in range */
			return 1;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return 0;
	}
	if(offset <= chd_offset && chd_end <= offset + size)
		return 1;
	if(chd_end <= offset || offset + size <= chd_offset)
		return 0;
	kdprintf(STDERR_FILENO, "%s(): (%s:%d): invalid inter-atomic block change descriptor!\n", __FUNCTION__, __FILE__, __LINE__);
	return 0;
}

/* This function is similar to the function barrier_simple_forward(), but it has
 * provisions for handling dependencies between the new sub-blocks that weren't
 * a problem with a single block. */
int barrier_partial_forward(partial_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block)
{
	int i, r;

	if (!block->ddesc->changes)
		return 0;

	/* prepare the block for chdesc forwarding */
	r = revision_tail_prepare(block, barrier);
	if (r < 0)
		return r;

	for (i=0; i < nforwards; i++)
	{
		bool synthetic, chdescs_moved = 0;
		bdesc_t * target_block;
		chmetadesc_t ** chmetadesc;
		partial_forward_t * forward = &forwards[i];
		
		/* block->ddesc->changes can become NULL after a chdesc_move(),
		 * so check NULLness each iteration */
		if (!block->ddesc->changes)
		{
			forward->block = NULL;
			continue;
		}

		target_block = CALL(forward->target, synthetic_read_block, forward->number, 1, &synthetic);
		if (!target_block)
			panic("%s(): forward->target->synthetic_read_block() failed, but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);

		if (block == target_block)
			panic("%s(): block == target_block (0x%08x), offset %d, size %d", __FUNCTION__, block, forward->offset, forward->size);

		/* transfer the barrier's bottom chdescs on block to target_block.
		 * this loop makes use of knowledge of how chdesc_move operates. */
		chmetadesc = &block->ddesc->changes->dependencies;
		while (block->ddesc->changes && *chmetadesc)
		{
			chdesc_t * chdesc = (*chmetadesc)->desc;
			if (chdesc->owner == barrier && !(chdesc->flags & CHDESC_ROLLBACK) && chdesc_in_range(chdesc, forward->offset, forward->size))
			{
				chdescs_moved = 1;
				/* keep it at the barrier for a bit... we need this to create the revision_slice */
				r = chdesc_move(chdesc, target_block, barrier, forward->offset);
				if (r < 0)
					panic("%s(): chdesc_move() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
			}
			else
				chmetadesc = &(*chmetadesc)->next;
		}
		if (chdescs_moved)
			chdesc_finish_move(target_block);

		if (!chdescs_moved && synthetic)
		{
			/* With no changes for this synthetic target_block, we might
			 * as well cancel the block */
			r = CALL(forward->target, cancel_block, forward->number);
			if (r < 0)
				panic("%s(): BD::cancel_block() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
			forward->block = NULL;
			continue;
		}

		if (chdescs_moved)
		{
			/* Bring target_block's data blob up to date with the transferred
			 * chdescs */
			assert(target_block->ddesc->length <= block->ddesc->length);
			assert(forward->offset + forward->size <= block->ddesc->length);
			assert(forward->size <= target_block->ddesc->length);
			memcpy(target_block->ddesc->data, block->ddesc->data + forward->offset, forward->size);
		}
		forward->block = bdesc_retain(target_block);
	}

	for (;;)
	{
		bool again = 0;
		for (i=0; i < nforwards; i++)
		{
			partial_forward_t * forward = &forwards[i];
			bdesc_t * target_block = forward->block;

			if (forward->block)
			{
				/* create an internal slice */
				revision_slice_t * slice = revision_slice_create(forward->block, barrier, forward->target, 0);
				if (!slice)
					panic("%s(): revision_slice_create() failed, but chdesc revert-move code for recovery is not implemented", __FUNCTION__);

				if (slice->ready_size)
				{
					revision_slice_push_down(slice);

					/* write the updated target_block */
					r = CALL(forward->target, write_block, target_block);
					if (r < 0)
						panic("%s(): target->write_block() failed (%i), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
				}

				if (slice->ready_size == slice->full_size)
					bdesc_release(&forward->block);
				else
					again = 1;
				revision_slice_destroy(slice);
			}
		}
		if (!again)
			break;
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
	chmetadesc_t ** chmetadesc;
	int i, r;
	
	if(nforwards < 2)
		return -E_INVAL;
	
	if(!block->ddesc->changes)
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
			panic("%s(): block == target_block[%d] (0x%08x)", __FUNCTION__, i, block);
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
	chmetadesc = &block->ddesc->changes->dependencies;
	while(block->ddesc->changes && *chmetadesc)
	{
		chdesc_t * chdesc = (*chmetadesc)->desc;
		if(chdesc->owner == barrier && !(chdesc->flags & CHDESC_ROLLBACK))
		{
			chdescs_moved = 1;
			r = chdesc_duplicate(chdesc, nforwards, target_block);
			if (r < 0)
				panic("%s(): chdesc_duplicate() failed (%i), but chdesc revert-duplicate code for recovery is not implemented", __FUNCTION__, r);
		}
		else
			chmetadesc = &(*chmetadesc)->next;
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
