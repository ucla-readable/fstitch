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

int barrier_single_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block, barrier_mangler_t mangle, void * mangle_data)
{
	bdesc_t * target_block;
	chdesc_t * head = NULL;
	chdesc_t * scan;
	chrefdesc_t * ref;
	int r;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	target_block = CALL(target, synthetic_read_block, number, 1);
	if(!target_block)
		return -E_UNSPECIFIED;
	
	if(block == target_block)
	{
		Dprintf("%s(): block == target_block (0x%08x)\n", __FUNCTION__, block);
		return 0;
	}
	assert(target_block->ddesc->length == block->ddesc->length);
	
	/* prepare the block */
	r = revision_tail_prepare(block, barrier);
	if(r < 0)
		goto error_block;
	
	if(mangle)
	{
		r = mangle(block, mangle_data, 1);
		assert(r >= 0);
	}
	r = chdesc_create_full(target_block, target, block->ddesc->data, &head);
	if(r < 0)
		goto error_block;
	if(mangle)
	{
		r = mangle(block, mangle_data, 0);
		assert(r >= 0);
	}
	
	/* now set the afters of all the old chdescs to depend on head */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
	{
		assert(scan->owner == barrier || (scan->flags & CHDESC_ROLLBACK));
		if(!(scan->flags & CHDESC_ROLLBACK))
		{
			chdepdesc_t * after;
			for(after = scan->afters; after; after = after->after.next)
			{
				r = chdesc_add_depend(after->after.desc, head);
				if(r < 0)
					goto error_chdesc;
			}
		}
		/* move the weak refs too */
		while(scan->weak_refs)
		{
			ref = scan->weak_refs;
			scan->weak_refs = ref->next;
			/* we deliberately do not update the pointer yet */
			assert(*ref->desc == scan);
			ref->next = head->weak_refs;
			head->weak_refs = ref;
		}
	}
	/* we must commit to the new weak references before we do the write */
	for(ref = head->weak_refs; ref; ref = ref->next)
		*ref->desc = head;
	
	/* write the updated target_block */
	r = CALL(target, write_block, target_block);
	if(r < 0)
		/* can't use error_chdesc because the weak references are changed now */
		panic("%s(): write_block() failed (%i)!", __FUNCTION__, r);
	
	/* put block back into current state */
	r = revision_tail_acknowledge(block, barrier);
	if(r < 0)
		panic("%s(): revision_tail_acknowledge() failed (%i)!", __FUNCTION__, r);

	return 0;
	
error_chdesc:
	/* fix the weak references */
	while(head->weak_refs)
	{
		ref = head->weak_refs;
		head->weak_refs = ref->next;
		ref->next = (*ref->desc)->weak_refs;
		(*ref->desc)->weak_refs = ref;
	}
	chdesc_destroy(&head);
error_block:
	return r;
}

int barrier_multiple_forward(multiple_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block)
{
#define target_block forwards[i]._block
	chdesc_t ** heads;
	chdesc_t * head;
	chdesc_t * scan;
	chrefdesc_t * ref;
	int i, r = -E_UNSPECIFIED;
	
	if(nforwards < 2)
		return -E_INVAL;
	
	if(!block->ddesc->all_changes)
		return 0;
	
	heads = malloc(nforwards * sizeof(*heads));
	if(!heads)
		return -E_NO_MEM;
	
	for(i = 0; i != nforwards; i++)
	{
		target_block = CALL(forwards[i].target, synthetic_read_block, forwards[i].number, 1);
		if(!target_block)
			goto error_block;
		if(block == target_block)
			panic("%s(): block == target_block[%d] (%p)", __FUNCTION__, i, block);
		assert(target_block->ddesc->length == block->ddesc->length);
	}
	
	/* prepare the block */
	r = revision_tail_prepare(block, barrier);
	if(r < 0)
		goto error_block;
	
	for(i = 0; i != nforwards; i++)
	{
		r = chdesc_create_full(target_block, forwards[i].target, block->ddesc->data, &heads[i]);
		if(r < 0)
			goto error_chdesc;
	}
	
	r = chdesc_create_noop_array(NULL, NULL, &head, nforwards, heads);
	if(r < 0)
		goto error_chdescs;
	
	/* now set the afters of all the old chdescs to depend on head */
	for(scan = block->ddesc->all_changes; scan; scan = scan->ddesc_next)
	{
		assert(scan->owner == barrier || (scan->flags & CHDESC_ROLLBACK));
		if(!(scan->flags & CHDESC_ROLLBACK))
		{
			chdepdesc_t * after;
			for(after = scan->afters; after; after = after->after.next)
			{
				r = chdesc_add_depend(after->after.desc, head);
				if(r < 0)
					goto error_after;
			}
		}
		/* move the weak refs too */
		while(scan->weak_refs)
		{
			ref = scan->weak_refs;
			scan->weak_refs = ref->next;
			/* we deliberately do not update the pointer yet */
			assert(*ref->desc == scan);
			ref->next = head->weak_refs;
			head->weak_refs = ref;
		}
	}
	/* we must commit to the new weak references before we do the write */
	for(ref = head->weak_refs; ref; ref = ref->next)
		*ref->desc = head;
	
	for(i = 0; i != nforwards; i++)
	{
		/* write the updated target_block */
		r = CALL(forwards[i].target, write_block, target_block);
		if(r < 0)
			panic("%s(): write_block() failed (%i)!", __FUNCTION__, r);
	}
	
	/* put block back into current state */
	r = revision_tail_acknowledge(block, barrier);
	if(r < 0)
		panic("%s(): revision_tail_acknowledge() failed (%i)!", __FUNCTION__, r);
	
	free(heads);
	return 0;
	
error_after:
	/* fix the weak references */
	while(head->weak_refs)
	{
		ref = head->weak_refs;
		head->weak_refs = ref->next;
		ref->next = (*ref->desc)->weak_refs;
		(*ref->desc)->weak_refs = ref;
	}
	chdesc_destroy(&head);
error_chdescs:
	i = nforwards;
error_chdesc:
	while(i--)
		chdesc_destroy(&heads[i]);
error_block:
	free(heads);
	return r;
}

int barrier_lock_block(bdesc_t * block, BD_t * owner)
{
	if(block->ddesc->lock_owner && block->ddesc->lock_owner != owner)
		return -E_BUSY;
	block->ddesc->lock_owner = owner;
	block->ddesc->lock_count++;
	bdesc_retain(block);
	return 0;
}

int barrier_unlock_block(bdesc_t * block, BD_t * owner)
{
	if(!block->ddesc->lock_owner || block->ddesc->lock_owner != owner)
		return -E_INVAL;
	if(!--block->ddesc->lock_count)
		block->ddesc->lock_owner = NULL;
	bdesc_release(&block);
	return 0;
}
