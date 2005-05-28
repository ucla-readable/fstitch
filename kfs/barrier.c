#include <inc/error.h>
#include <inc/assert.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/revision.h>
#include <kfs/barrier.h>

/* barrier_simple_forward() is the single-case version of
 * barrier_partial_forward(). However, barrier_simple_forward() is able
 * to recover on BD::synthetic_read_block() failure and does slightly fewer
 * pointer dereferences. More importantly, it will likely be easier to
 * optimize the revision_*() calls away for the !synthetic case in
 * barrier_simple_forward. */
int barrier_simple_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block)
{
	bool synthetic;
	bdesc_t * target_block;
	chmetadesc_t * chmetadesc;
	int r;

	target_block = CALL(target, synthetic_read_block, number, &synthetic);
	if (!target_block)
		return -E_UNSPECIFIED;

	/* prepare the block for chdesc forwarding */
	/* TODO: revision_* can be avoided if !synthetic */
	r = revision_tail_prepare(block, barrier);
	if (r < 0)
		return r;

	if (synthetic)
	{
		/* initialize synthetic target_block with what [we believe] is on
		 * the disk */
		assert(target_block->ddesc->length == block->ddesc->length);
		memcpy(target_block->ddesc->data, block->ddesc->data, block->ddesc->length);
	}

	/* transfer the barrier's bottom chdescs on block to target_block */
	for (chmetadesc = block->ddesc->changes->dependencies; chmetadesc; chmetadesc = chmetadesc->next)
	{
		chdesc_t * chdesc = chmetadesc->desc;
		if (chdesc->owner == barrier)
		{
			r = chdesc_move(chdesc, target_block);
			if (r < 0)
				panic("%s(): chdesc_move() failed (%e), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
		}
	}
	chdesc_finish_move(target_block);

	/* write the updated target_block */
	r = CALL(target, write_block, target_block);
	if (r < 0)
		panic("%s(): target->write_block() failed (%e), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);

	/* put block back into current state */
	r = revision_tail_revert(block, barrier);
	if (r < 0)
		panic("%s(): revision_tail_revert() failed (%e), but this function does not know what to do in case of failure", __FUNCTION__, r);

	return 0;
}

static bool chdesc_in_range(chdesc_t * chdesc, uint32_t offset, uint32_t size)
{
	uint32_t chd_offset, chd_end;
	/* note that we require that change descriptors do not cross the atomic disk unit
	 * size boundary, so that we will never have to fragment a change descriptor */
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
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return 0;
	}
	if(offset <= chd_offset && chd_end <= offset + size)
		return 1;
	if(chd_end <= offset || offset + size <= chd_offset)
		return 0;
	fprintf(STDERR_FILENO, "%s(): (%s:%d): invalid inter-atomic block change descriptor!\n", __FUNCTION__, __FILE__, __LINE__);
	return 0;
}

/* This function is almost exactly the function barrier_simple_forward().
 * See its comment. */
int barrier_partial_forward(partial_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block)
{
	int i, r;

	/* prepare the block for chdesc forwarding */
	/* TODO: revision_* can be avoided if !synthetic */
	r = revision_tail_prepare(block, barrier);
	if (r < 0)
		return r;

	for (i=0; i < nforwards; i++)
	{
		bool synthetic;
		bdesc_t * target_block;
		chmetadesc_t * chmetadesc;
		partial_forward_t * forward = &forwards[i];

		target_block = CALL(forward->target, synthetic_read_block, forward->number, &synthetic);
		if (!target_block)
			panic("%s(): forward->target->synthetic_read_block() failed, but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);

		if (synthetic)
		{
			/* initialize synthetic target_block with what [we believe] is on
			 * the disk */
			assert(target_block->ddesc->length == block->ddesc->length);
			memcpy(target_block->ddesc->data, block->ddesc->data, block->ddesc->length);
		}

		/* transfer the barrier's bottom chdescs on block to target_block */
		for (chmetadesc = block->ddesc->changes->dependencies; chmetadesc; chmetadesc = chmetadesc->next)
		{
			chdesc_t * chdesc = chmetadesc->desc;
			if (chdesc->owner == barrier && chdesc_in_range(chdesc, forward->offset, forward->size))
			{
				r = chdesc_move(chdesc, target_block);
				if (r < 0)
					panic("%s(): chdesc_move() failed (%e), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
			}
		}
		chdesc_finish_move(target_block);

		/* write the updated target_block */
		r = CALL(forward->target, write_block, target_block);
		if (r < 0)
			panic("%s(): target->write_block() failed (%e), but chdesc revert-move code for recovery is not implemented", __FUNCTION__, r);
	}

	/* put block back into current state */
	r = revision_tail_revert(block, barrier);
	if (r < 0)
		panic("%s(): revision_tail_revert() failed (%e), but this function does not know what to do in case of failure", __FUNCTION__, r);

	return 0;
}
