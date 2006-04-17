#include <inc/error.h>
#include <lib/assert.h>
#include <lib/kdprintf.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/memdup.h>
#include <lib/panic.h>

#include <kfs/debug.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

/* Change descriptor multigraphs allow more than one dependency between the same
 * two change descriptors. This currently saves us the trouble of making sure we
 * don't create a duplicate dependency between chdescs, though it also causes us
 * to allocate somewhat more memory in many cases where we would otherwise
 * detect the duplicate dependency. Allowing multigraphs results in a reasonable
 * speedup, even though we use more memory, so it is enabled by default. */
#define CHDESC_ALLOW_MULTIGRAPH 1

static chdesc_t * free_head = NULL;

static void chdesc_free_push(chdesc_t * chdesc)
{
	assert(free_head != chdesc && !chdesc->free_prev);
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc, free_head);
	chdesc->free_next = free_head;
	if(free_head)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, free_head, chdesc);
		free_head->free_prev = chdesc;
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_HEAD, chdesc);
	free_head = chdesc;
}

static void chdesc_free_remove(chdesc_t * chdesc)
{
	assert(chdesc->free_prev || free_head == chdesc);
	if(chdesc->free_prev)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc->free_prev, chdesc->free_next);
		chdesc->free_prev->free_next = chdesc->free_next;
	}
	else
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_HEAD, chdesc->free_next);
		free_head = chdesc->free_next;
	}
	if(chdesc->free_next)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, chdesc->free_next, chdesc->free_prev);
		chdesc->free_next->free_prev = chdesc->free_prev;
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, chdesc, NULL);
	chdesc->free_prev = NULL;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc, NULL);
	chdesc->free_next = NULL;
}

/* ensure bdesc->ddesc->changes has a noop chdesc */
static int ensure_bdesc_has_changes(bdesc_t * block)
{
	chdesc_t * chdesc;
	assert(block);
	
	if(block->ddesc->changes)
	{
		assert(block->ddesc->changes->type == NOOP);
		return 0;
	}
	
	chdesc = chdesc_create_noop(NULL, NULL);
	if(!chdesc)
		return -E_NO_MEM;
	
	if(chdesc_weak_retain(chdesc, &block->ddesc->changes) < 0)
	{
		chdesc_destroy(&chdesc);
		return -E_NO_MEM;
	}
	
	return 0;
}

/* ensure bdesc->ddesc->overlaps has a noop chdesc */
static int ensure_bdesc_has_overlaps(bdesc_t * block)
{
	chdesc_t * chdesc;
	assert(block);
	
	if(block->ddesc->overlaps)
	{
		assert(block->ddesc->overlaps->type == NOOP);
		return 0;
	}
	
	chdesc = chdesc_create_noop(NULL, NULL);
	if(!chdesc)
		return -E_NO_MEM;
	
	if(chdesc_weak_retain(chdesc, &block->ddesc->overlaps) < 0)
	{
		chdesc_destroy(&chdesc);
		return -E_NO_MEM;
	}
	
	return 0;
}

/* ensure bdesc->ddesc->bit_changes[offset] has a noop chdesc */
static chdesc_t * ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset)
{
	chdesc_t * chdesc;
	hash_map_elt_t * elt;
	void * key = (void *) (uint32_t) offset;
	assert(block);
	
	chdesc = (chdesc_t *) hash_map_find_val(block->ddesc->bit_changes, key);
	if(chdesc)
	{
		assert(chdesc->type == NOOP);
		return chdesc;
	}
	
	chdesc = chdesc_create_noop(NULL, NULL);
	if(!chdesc)
		return NULL;
	
	if(hash_map_insert(block->ddesc->bit_changes, key, chdesc) < 0)
	{
		chdesc_destroy(&chdesc);
		return NULL;
	}
	elt = hash_map_find_eltp(block->ddesc->bit_changes, key);
	assert(elt);
	
	/* we don't really need a flag for this, since we could just use the
	 * noop.bit_changes field to figure it out... but that would be error-prone */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_BIT_NOOP);
	chdesc->flags |= CHDESC_BIT_NOOP;
	chdesc->noop.bit_changes = block->ddesc->bit_changes;
	chdesc->noop.hash_key = key;
	
	if(chdesc_weak_retain(chdesc, (chdesc_t **) &elt->val) < 0)
	{
		hash_map_erase(block->ddesc->bit_changes, key);
		chdesc_destroy(&chdesc);
		return NULL;
	}
	
	return chdesc;
}

/* get bdesc->ddesc->bit_changes[offset] */
static chdesc_t * chdesc_bit_changes(bdesc_t * block, uint16_t offset)
{
	return hash_map_find_val(block->ddesc->bit_changes, (void *) (uint32_t) offset);
}

/* add a dependency to a change descriptor without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
#if !CHDESC_ALLOW_MULTIGRAPH
	/* make sure it's not already there */
	for(meta = dependent->dependencies; meta; meta = meta->next)
		if(meta->desc == dependency)
			return 0;
	/* shouldn't be there */
	for(meta = dependency->dependents; meta; meta = meta->next)
		assert(meta->desc != dependent);
#endif
	
	/* add the dependency to the dependent */
	meta = malloc(sizeof(*meta));
	if(!meta)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENCY, dependent, dependency);
	meta->desc = dependency;
	meta->next = NULL;
	*dependent->dependencies_tail = meta;
	
	/* add the dependent to the dependency */
	meta = malloc(sizeof(*meta));
	if(!meta)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, dependent, dependency);
		meta = *dependent->dependencies_tail;
		*dependent->dependencies_tail = NULL;
		free(meta);
		return -E_NO_MEM;
	}
	/* do the last step of the dependency addition above only after we know we can succeed */
	dependent->dependencies_tail = &(*dependent->dependencies_tail)->next;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENT, dependency, dependent);
	meta->desc = dependent;
	meta->next = NULL;
	*dependency->dependents_tail = meta;
	dependency->dependents_tail = &meta->next;
	
	/* virgin NOOP chdesc getting its first dependency */
	if(free_head == dependent || dependent->free_prev)
	{
		assert(dependent->type == NOOP);
		assert(!(dependent->flags & CHDESC_WRITTEN));
		chdesc_free_remove(dependent);
	}
	
	return 0;
}

/* note that we don't check to see if these chdescs are for the same ddesc or not */
/* returns 0 for no overlap, 1 for overlap, and 2 for a overlaps b completely */
int chdesc_overlap_check(chdesc_t * a, chdesc_t * b)
{
	uint16_t a_start, a_len;
	uint16_t b_start, b_len;
	uint32_t start, end, tag;
	
	/* if either is a NOOP chdesc, they don't overlap */
	if(a->type == NOOP || b->type == NOOP)
		return 0;
	
	/* two bit chdescs overlap if they modify the same bits */
	if(a->type == BIT && b->type == BIT)
	{
		uint32_t shared;
		if(a->bit.offset != b->bit.offset)
			return 0;
		shared = a->bit.xor & b->bit.xor;
		if(!shared)
			return 0;
		/* check for complete overlap */
		return (shared == b->bit.offset) ? 2 : 1;
	}
	
	if(a->type == BIT)
	{
		a_len = sizeof(a->bit.xor);
		a_start = a->bit.offset * a_len;
	}
	else
	{
		a_len = a->byte.length;
		a_start = a->byte.offset;
	}
	if(b->type == BIT)
	{
		b_len = sizeof(b->bit.xor);
		b_start = b->bit.offset * b_len;
	}
	else
	{
		b_len = b->byte.length;
		b_start = b->byte.offset;
	}
	
	start = b_start;
	end = start + b_len + a_len;
	tag = a_start + a_len;
	if(tag <= start || end <= tag)
		return 0;
	return (a_start <= b_start && start + b_len <= tag) ? 2 : 1;
}

/* make the recent chdesc depend on the given earlier chdesc in the same block if it overlaps */
static int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original)
{
	int r, overlap;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_ATTACH, recent, original);
	
	/* if either is a NOOP chdesc, warn about it */
	if(recent->type == NOOP || original->type == NOOP)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Unexpected NOOP chdesc\n", __FUNCTION__, __FILE__, __LINE__);
	
	/* if they don't overlap, we are done */
	overlap = chdesc_overlap_check(recent, original);
	if(!overlap)
		return 0;
	
	if(original->flags & CHDESC_ROLLBACK)
	{
		/* it's not clear what to do in this case... just fail with a warning for now */
		kdprintf(STDERR_FILENO, "Attempt to overlap a new chdesc with a rolled-back chdesc!\n");
		return -E_BUSY;
	}
	
	r = chdesc_add_depend(recent, original);
	if(r < 0)
		return r;
	
	/* if it overlaps completely, remove original from ddesc->overlaps or ddesc->bit_changes */
	if(overlap == 2)
	{
		if(original->type == BYTE)
			chdesc_remove_depend(original->block->ddesc->overlaps, original);
		else if(original->type == BIT)
		{
			chdesc_t * bit_changes = chdesc_bit_changes(original->block, original->bit.offset);
			assert(bit_changes);
			chdesc_remove_depend(bit_changes, original);
		}
		else
			kdprintf(STDERR_FILENO, "Complete overlap of unhandled chdesc type!\n");
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, recent, CHDESC_OVERLAP);
		recent->flags |= CHDESC_OVERLAP;
	}
	
	return 0;
}

static int __chdesc_overlap_multiattach_slip(chdesc_t * chdesc, chmetadesc_t * list, bool slip_under)
{
	chmetadesc_t * next = list;
	while((list = next))
	{
		int r;
		
		/* this loop is tricky, because we might remove the item we're
		 * looking at currently if we overlap it entirely - so we
		 * prefetch the next pointer at the top of the loop */
		next = list->next;
		
		/* skip moved chdescs - they have just been added to this block
		 * by chdesc_move() and already have proper overlap dependency
		 * information with respect to the chdesc now arriving */
		if(list->desc->flags & CHDESC_MOVED || list->desc == chdesc)
			continue;
		
		/* "Slip Under" allows us to create change descriptors
		 * underneath existing ones. (That is, existing chdescs will
		 * depend on the new one, not the other way around.) This is a
		 * hidden feature for internal use only. */
		if(slip_under)
			r = chdesc_overlap_attach(list->desc, chdesc);
		else
			r = chdesc_overlap_attach(chdesc, list->desc);
		if(r < 0)
			return r;
	}
	return 0;
}

static int chdesc_overlap_multiattach_slip(chdesc_t * chdesc, bdesc_t * block, bool slip_under)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_MULTIATTACH, chdesc, block, slip_under);
	
	if(chdesc->type == BIT)
	{
		chdesc_t * bit_changes = chdesc_bit_changes(block, chdesc->bit.offset);
		if(bit_changes)
		{
			int r = __chdesc_overlap_multiattach_slip(chdesc, bit_changes->dependencies, slip_under);
			if(r < 0)
				return r;
		}
	}
	
	if(!block->ddesc->overlaps)
		return 0;
	
	return __chdesc_overlap_multiattach_slip(chdesc, block->ddesc->overlaps->dependencies, slip_under);
}

static int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
{
	return chdesc_overlap_multiattach_slip(chdesc, block, 0);
}

int __ensure_bdesc_has_changes(bdesc_t * block)
#if defined(__MACH__)
{
	return ensure_bdesc_has_changes(block);
}
#else
	__attribute__ ((alias("ensure_bdesc_has_changes")));
#endif

int __ensure_bdesc_has_overlaps(bdesc_t * block)
#if defined(__MACH__)
{
	return ensure_bdesc_has_overlaps(block);
}
#else
	__attribute__ ((alias("ensure_bdesc_has_overlaps")));
#endif

chdesc_t * __ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset)
#if defined(__MACH__)
{
	return ensure_bdesc_has_bit_changes(block, offset);
}
#else
	__attribute__ ((alias("ensure_bdesc_has_bit_changes")));
#endif

chdesc_t * __chdesc_bit_changes(bdesc_t * block, uint16_t offset)
#if defined(__MACH__)
{
	return chdesc_bit_changes(block, offset);
}
#else
	__attribute__ ((alias("chdesc_bit_changes")));
#endif

int __chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
#if defined(__MACH__)
{
	return chdesc_add_depend_fast(dependent, dependency);
}
#else
	__attribute__((alias("chdesc_add_depend_fast")));
#endif

int __chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
#if defined(__MACH__)
{
	return chdesc_overlap_multiattach(chdesc, block);
}
#else
	__attribute__((alias("chdesc_overlap_multiattach")));
#endif

chdesc_t * chdesc_create_noop(bdesc_t * block, BD_t * owner)
{
	chdesc_t * chdesc;
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_NOOP, chdesc, block, owner);
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependencies_tail = &chdesc->dependencies;
	chdesc->dependents = NULL;
	chdesc->dependents_tail = &chdesc->dependents;
	chdesc->weak_refs = NULL;
	chdesc->free_prev = NULL;
	chdesc->free_next = NULL;
	chdesc->stamps = 0;
	chdesc->ready_epoch = 0;
	
	/* NOOP chdescs start applied */
	chdesc->flags = 0;
	
	if(block)
	{
		/* add chdesc to block's dependencies */
		if(ensure_bdesc_has_changes(block) < 0)
		{
			free(chdesc);
			return NULL;
		}
		if(chdesc_add_depend_fast(block->ddesc->changes, chdesc) < 0)
		{
			free(chdesc);
			return NULL;
		}
		
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	chdesc_free_push(chdesc);
	
	return chdesc;
}

chdesc_t * chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor)
{
	chdesc_t * chdesc;
	chdesc_t * bit_changes;
	int r;
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BIT, chdesc, block, owner, offset, xor);
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	chdesc->dependencies = NULL;
	chdesc->dependencies_tail = &chdesc->dependencies;
	chdesc->dependents = NULL;
	chdesc->dependents_tail = &chdesc->dependents;
	chdesc->weak_refs = NULL;
	chdesc->free_prev = NULL;
	chdesc->free_next = NULL;
	chdesc->stamps = 0;
	chdesc->ready_epoch = 0;
	
	/* start rolled back so we can apply it */
	chdesc->flags = CHDESC_ROLLBACK;
	
	/* make sure it is dependent upon any pre-existing chdescs */
	if(chdesc_overlap_multiattach(chdesc, block))
		goto error;
	
	/* make sure it applies cleanly */
	if(chdesc_apply(chdesc))
		goto error;
	
	/* add chdesc to block's dependencies */
	if((r = ensure_bdesc_has_changes(block)) < 0)
		goto error;
	if((r = chdesc_add_depend_fast(block->ddesc->changes, chdesc)) < 0)
		goto error;
	if(!(bit_changes = ensure_bdesc_has_bit_changes(block, offset)))
		goto error;
	if((r = chdesc_add_depend_fast(bit_changes, chdesc)) < 0)
		goto error;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	return chdesc;
	
  error:
	chdesc_destroy(&chdesc);
	return NULL;
}

#if CHDESC_BYTE_SUM
/* stupid little checksum, just to try and make sure we get the same data */
static uint16_t chdesc_byte_sum(uint8_t * data, size_t length)
{
	uint16_t sum = 0x5AFE;
	while(length-- > 0)
	{
		/* ROL 3 */
		sum = (sum << 3) | (sum >> 13);
		sum ^= *(data++);
	}
	return sum;
}
#endif

int chdesc_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t init_offset = offset % atomic_size;
	uint16_t count = (length + init_offset + atomic_size - 1) / atomic_size;
	
	if(count == 1)
		return chdesc_create_byte(block, owner, offset, length, data, head);
	return -E_INVAL;
}

int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t init_offset = offset % atomic_size;
	uint16_t index = offset / atomic_size;
	uint16_t count = (length + init_offset + atomic_size - 1) / atomic_size;
	uint16_t copied = 0;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i, r;
	
	if(!chdescs)
		return -E_NO_MEM;
	if(&block->ddesc->data[offset] == data)
		panic("Cannot create a change descriptor in place!");
	
	if((r = ensure_bdesc_has_changes(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	if((r = ensure_bdesc_has_overlaps(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
		chdescs[i]->owner = owner;		
		chdescs[i]->block = block;
		chdescs[i]->type = BYTE;
		chdescs[i]->byte.offset = (index + i) * atomic_size + (i ? 0 : init_offset);
		if(count == 1)
			chdescs[i]->byte.length = length;
		else if(i == count - 1)
		{
			chdescs[i]->byte.length = (init_offset + length) % atomic_size;
			if(!chdescs[i]->byte.length)
				chdescs[i]->byte.length = atomic_size;
		}
		else
			chdescs[i]->byte.length = atomic_size - (i ? 0 : init_offset);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependencies_tail = &chdescs[i]->dependencies;
		chdescs[i]->dependents = NULL;
		chdescs[i]->dependents_tail = &chdescs[i]->dependents;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->free_prev = NULL;
		chdescs[i]->free_next = NULL;
		chdescs[i]->stamps = 0;
		chdescs[i]->ready_epoch = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		chdescs[i]->byte.data = data ? memdup(&((uint8_t *) data)[copied], chdescs[i]->byte.length) : calloc(1, chdescs[i]->byte.length);
		if(!chdescs[i]->byte.data)
			goto destroy;
#if CHDESC_BYTE_SUM
		chdescs[i]->byte.old_sum = chdesc_byte_sum(&block->ddesc->data[chdescs[i]->byte.offset], chdescs[i]->byte.length);
		chdescs[i]->byte.new_sum = chdesc_byte_sum(chdescs[i]->byte.data, chdescs[i]->byte.length);
#endif
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, chdescs[i]->byte.offset, chdescs[i]->byte.length);
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		if((r = chdesc_add_depend_fast(block->ddesc->overlaps, chdescs[i])) < 0)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		/* we are creating all new chdescs, so we don't need to check for loops */
		/* but we should check to make sure *head has not already been written */
		if((i || (*head && !((*head)->flags & CHDESC_WRITTEN))) && chdesc_add_depend_fast(chdescs[i], i ? chdescs[i - 1] : *head))
			goto destroy;
		
		copied += chdescs[i]->byte.length;
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], i ? chdescs[i - 1] : *head);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		return -E_NO_MEM;
	}
	
	assert(copied == length);
	
	for(i = 0; i != count; i++)
	{
		if(chdesc_apply(chdescs[i]))
			break;
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
		{
			/* we don't need the block after all... */
			bdesc_t * temp = block;
			bdesc_release(&temp);
			chdesc_rollback(chdescs[i]);
		}
		for(i = 0; i != count; i++)
			chdesc_destroy(&chdescs[i]);
		free(chdescs);
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	
	free(chdescs);
	
	return 0;
}

int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t count = block->ddesc->length / atomic_size;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i, r;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	if((r = ensure_bdesc_has_changes(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	if((r = ensure_bdesc_has_overlaps(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
		chdescs[i]->owner = owner;		
		chdescs[i]->block = block;
		chdescs[i]->type = BYTE;
		chdescs[i]->byte.offset = i * atomic_size;
		chdescs[i]->byte.length = atomic_size;
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependencies_tail = &chdescs[i]->dependencies;
		chdescs[i]->dependents = NULL;
		chdescs[i]->dependents_tail = &chdescs[i]->dependents;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->free_prev = NULL;
		chdescs[i]->free_next = NULL;
		chdescs[i]->stamps = 0;
		chdescs[i]->ready_epoch = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		chdescs[i]->byte.data = calloc(1, atomic_size);
		if(!chdescs[i]->byte.data)
			goto destroy;
#if CHDESC_BYTE_SUM
		chdescs[i]->byte.old_sum = chdesc_byte_sum(&block->ddesc->data[chdescs[i]->byte.offset], atomic_size);
		chdescs[i]->byte.new_sum = chdesc_byte_sum(chdescs[i]->byte.data, atomic_size);
#endif
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, i * atomic_size, atomic_size);
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		if((r = chdesc_add_depend_fast(block->ddesc->overlaps, chdescs[i])) < 0)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		/* we are creating all new chdescs, so we don't need to check for loops */
		/* but we should check to make sure *head has not already been written */
		if((i || (*head && !((*head)->flags & CHDESC_WRITTEN))) && chdesc_add_depend_fast(chdescs[i], i ? chdescs[i - 1] : *head))
			goto destroy;
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], i ? chdescs[i - 1] : *head);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		return -E_NO_MEM;
	}
	
	for(i = 0; i != count; i++)
	{
		if(chdesc_apply(chdescs[i]))
			break;
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
		{
			/* we don't need the block after all... */
			bdesc_t * temp = block;
			bdesc_release(&temp);
			chdesc_rollback(chdescs[i]);
		}
		for(i = 0; i != count; i++)
			chdesc_destroy(&chdescs[i]);
		free(chdescs);
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	
	free(chdescs);
	
	return 0;
}

int __chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, bool slip_under)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t count = block->ddesc->length / atomic_size;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i, r;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	if((r = ensure_bdesc_has_changes(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	if((r = ensure_bdesc_has_overlaps(block)) < 0)
	{
		free(chdescs);
		return r;
	}
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
		chdescs[i]->owner = owner;
		chdescs[i]->block = block;
		chdescs[i]->type = BYTE;
		chdescs[i]->byte.offset = i * atomic_size;
		chdescs[i]->byte.length = atomic_size;
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependencies_tail = &chdescs[i]->dependencies;
		chdescs[i]->dependents = NULL;
		chdescs[i]->dependents_tail = &chdescs[i]->dependents;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->free_prev = NULL;
		chdescs[i]->free_next = NULL;
		chdescs[i]->stamps = 0;
		chdescs[i]->ready_epoch = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		chdescs[i]->byte.data = memdup(&((uint8_t *) data)[i * atomic_size], atomic_size);
		if(!chdescs[i]->byte.data)
			goto destroy;
#if CHDESC_BYTE_SUM
		chdescs[i]->byte.old_sum = chdesc_byte_sum(&block->ddesc->data[chdescs[i]->byte.offset], atomic_size);
		chdescs[i]->byte.new_sum = chdesc_byte_sum(chdescs[i]->byte.data, atomic_size);
#endif
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, i * atomic_size, atomic_size);
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		if((r = chdesc_add_depend_fast(block->ddesc->overlaps, chdescs[i])) < 0)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach_slip(chdescs[i], block, slip_under))
			goto destroy;
		
		/* we are creating all new chdescs, so we don't need to check for loops */
		/* but we should check to make sure *head has not already been written */
		if((i || (*head && !((*head)->flags & CHDESC_WRITTEN))) && chdesc_add_depend_fast(chdescs[i], i ? chdescs[i - 1] : *head))
			goto destroy;
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], i ? chdescs[i - 1] : *head);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		return -E_NO_MEM;
	}
	
	for(i = 0; i != count; i++)
	{
		if(chdesc_apply(chdescs[i]))
			break;
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
		{
			/* we don't need the block after all... */
			bdesc_t * temp = block;
			bdesc_release(&temp);
			chdesc_rollback(chdescs[i]);
		}
		for(i = 0; i != count; i++)
			chdesc_destroy(&chdescs[i]);
		free(chdescs);
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	
	free(chdescs);
	
	return 0;
}

int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	return __chdesc_create_full(block, owner, data, head, 0);
}

/* Rewrite a byte change descriptor to have an updated "new data" field,
 * avoiding the need to create layers of byte change descriptors if the previous
 * changes are no longer relevant (e.g. if they are being overwritten and will
 * never need to be rolled back independently from the new data). The change
 * descriptor must not be overlapped by any other change descriptors. The offset
 * and length parameters are relative to the change descriptor itself. */
int chdesc_rewrite_byte(chdesc_t * chdesc, uint16_t offset, uint16_t length, void * data)
{
	/* sanity checks */
	if(chdesc->type != BYTE)
		return -E_INVAL;
	if(offset + length > chdesc->byte.offset + chdesc->byte.length)
		return -E_INVAL;
	
	/* scan for overlapping chdescs - they will all depend on us, or at
	 * least, if there are any, at least one will depend directly on us */
	if(chdesc->dependents)
	{
		chmetadesc_t * meta;
		for(meta = chdesc->dependents; meta; meta = meta->next)
		{
			/* no block? doesn't overlap */
			if(!meta->desc->block)
				continue;
			/* not the same block? doesn't overlap */
			if(meta->desc->block->ddesc != chdesc->block->ddesc)
				continue;
			/* chdesc_overlap_check doesn't check that the block is
			 * the same, which is why we just checked it by hand */
			if(!chdesc_overlap_check(meta->desc, chdesc))
				continue;
			/* overlap detected! */
			return -E_PERM;
		}
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REWRITE_BYTE, chdesc);
	
	/* no overlaps */
	if(chdesc->flags & CHDESC_ROLLBACK)
	{
		memcpy(&chdesc->byte.data[offset], data, length);
#if CHDESC_BYTE_SUM
		chdesc->byte.new_sum = chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length);
#endif
	}
	else
	{
		memcpy(&chdesc->block->ddesc->data[chdesc->byte.offset + offset], data, length);
#if CHDESC_BYTE_SUM
		chdesc->byte.new_sum = chdesc_byte_sum(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.length);
#endif
	}
	return 0;
}

#if CHDESC_CYCLE_CHECK
static int chdesc_has_dependency(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, dependent, CHDESC_MARKED);
	dependent->flags |= CHDESC_MARKED;
	for(meta = dependent->dependencies; meta; meta = meta->next)
	{
		if(meta->desc == dependency)
			return 1;
		if(!(meta->desc->flags & CHDESC_MARKED))
			if(chdesc_has_dependency(meta->desc, dependency))
				return 1;
	}
	/* the chdesc graph is a DAG, so unmarking here would defeat the purpose */
	return 0;
}
#endif

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	/* compensate for Heisenberg's uncertainty principle */
	if(!dependent || !dependency)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided use of NULL pointer!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	/* make sure we're not fiddling with chdescs that are already written */
	if(dependent->flags & CHDESC_WRITTEN)
	{
		if(dependency->flags & CHDESC_WRITTEN)
			return 0;
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Attempt to add dependency to already written data!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	if(dependency->flags & CHDESC_WRITTEN)
		return 0;
	
	/* avoid creating a dependency loop */
#if CHDESC_CYCLE_CHECK
	if(dependent == dependency || chdesc_has_dependency(dependency, dependent))
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	/* chdesc_has_dependency() marks the DAG rooted at "dependency" so we must unmark it */
	chdesc_unmark_graph(dependency);
#endif
	
	return chdesc_add_depend_fast(dependent, dependency);
}

static void chdesc_meta_remove(chmetadesc_t ** list, chmetadesc_t *** tail, chdesc_t * chdesc)
{
	chmetadesc_t * scan = *list;
	while(scan)
	{
		if(scan->desc == chdesc)
		{
			*list = scan->next;
			if(!scan->next)
				*tail = list;
			free(scan);
			scan = *list;
#if CHDESC_ALLOW_MULTIGRAPH
			/* if we break here, we'll remove only one edge... this
			 * is required for multigraphs, and OK for standard
			 * graphs (which should have only one anyway), but safer
			 * to leave off in the latter case */
			break;
#endif
		}
		else
		{
			list = &scan->next;
			scan = scan->next;
		}
	}
}

void chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, dependent, dependency);
	chdesc_meta_remove(&dependent->dependencies, &dependent->dependencies_tail, dependency);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENT, dependency, dependent);
	chdesc_meta_remove(&dependency->dependents, &dependency->dependents_tail, dependent);
	
	if(dependent->type == NOOP && !dependent->dependencies)
		/* we just removed the last dependency of a NOOP chdesc, so satisfy it */
		chdesc_satisfy(&dependent);
}

static void memxchg(void * p, void * q, size_t n)
{
	/* align at least p on 32-bit boundary */
	while((uint32_t) p % 4 && n > 0)
	{
		uint8_t c = *(uint8_t *) p;
		*(uint8_t *) p++ = *(uint8_t *) q;
		*(uint8_t *) q++ = c;
		n--;
	}
	while(n > 3)
	{
		uint32_t d = *(uint32_t *) p;
		*(uint32_t *) p = *(uint32_t *) q;
		*(uint32_t *) q = d;
		p += 4;
		q += 4;
		n -= 4;
	}
	while(n > 0)
	{
		uint8_t c = *(uint8_t *) p;
		*(uint8_t *) p++ = *(uint8_t *) q;
		*(uint8_t *) q++ = c;
		n--;
	}
}

int chdesc_apply(chdesc_t * chdesc)
{
	if(!(chdesc->flags & CHDESC_ROLLBACK))
		return -E_INVAL;
	switch(chdesc->type)
	{
		case BIT:
			((uint32_t *) chdesc->block->ddesc->data)[chdesc->bit.offset] ^= chdesc->bit.xor;
			break;
		case BYTE:
			if(!chdesc->byte.data)
				return -E_INVAL;
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.new_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			memxchg(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.data, chdesc->byte.length);
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.old_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			break;
		case NOOP:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): applying NOOP chdesc\n", __FUNCTION__, __FILE__, __LINE__);
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	chdesc->flags &= ~CHDESC_ROLLBACK;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_APPLY, chdesc);
	return 0;
}

int chdesc_rollback(chdesc_t * chdesc)
{
	if(chdesc->flags & CHDESC_ROLLBACK)
		return -E_INVAL;
	switch(chdesc->type)
	{
		case BIT:
			((uint32_t *) chdesc->block->ddesc->data)[chdesc->bit.offset] ^= chdesc->bit.xor;
			break;
		case BYTE:
			if(!chdesc->byte.data)
				return -E_INVAL;
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.old_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			memxchg(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.data, chdesc->byte.length);
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.new_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			break;
		case NOOP:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): rolling back NOOP chdesc\n", __FUNCTION__, __FILE__, __LINE__);
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	chdesc->flags |= CHDESC_ROLLBACK;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ROLLBACK, chdesc);
	return 0;
}

static void chdesc_weak_collect(chdesc_t * chdesc)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_WEAK_COLLECT, chdesc);
	while(chdesc->weak_refs)
	{
		/* in theory, this is all that is necessary... */
		if(*chdesc->weak_refs->desc == chdesc)
			chdesc_weak_release(chdesc->weak_refs->desc);
		else
		{
			/* ...but check for this anyway */
			chrefdesc_t * next = chdesc->weak_refs;
			kdprintf(STDERR_FILENO, "%s: (%s:%d): dangling chdesc weak reference!\n", __FUNCTION__, __FILE__, __LINE__);
			chdesc->weak_refs = next->next;
			free(next);
		}
	}
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it and add it to the list of written chdescs */
int chdesc_satisfy(chdesc_t ** chdesc)
{
	if((*chdesc)->flags & CHDESC_WRITTEN)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfaction of already satisfied chdesc!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_SATISFY, *chdesc);
	
	if((*chdesc)->dependencies)
	{
		chdesc_t * bit_changes;
		/* We are trying to satisfy a chdesc with dependencies, which
		 * can happen if we have modules generating out-of-order chdescs
		 * but no write-back caches. We need to convert it to a NOOP so
		 * that any of its dependents will still have the indirect
		 * dependencies on the dependencies of this chdesc. However, we
		 * still need to collect any weak references to it in case
		 * anybody was watching it to see when it got satisfied. */
		if((*chdesc)->type != NOOP)
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfying chdesc %p of type %d with dependencies!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc, (*chdesc)->type);
		switch((*chdesc)->type)
		{
			case BYTE:
				if((*chdesc)->byte.data)
				{
					free((*chdesc)->byte.data);
					(*chdesc)->byte.data = NULL;
				}
				chdesc_remove_depend((*chdesc)->block->ddesc->overlaps, *chdesc);
				(*chdesc)->type = NOOP;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, *chdesc);
				break;
			case BIT:
				bit_changes = chdesc_bit_changes((*chdesc)->block, (*chdesc)->bit.offset);
				assert(bit_changes);
				chdesc_remove_depend(bit_changes, *chdesc);
				(*chdesc)->type = NOOP;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, *chdesc);
				/* fall through */
			case NOOP:
				break;
			default:
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
				return -E_INVAL;
		}
		
	}
	else
	{
		while((*chdesc)->dependents)
		{
			chmetadesc_t * meta = (*chdesc)->dependents;
			chdesc_t * dependent = meta->desc;
			(*chdesc)->dependents = meta->next;
			if(!meta->next)
				(*chdesc)->dependents_tail = &(*chdesc)->dependents;
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENT, *chdesc, meta->desc);
			
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, meta->desc, *chdesc);
			chdesc_meta_remove(&meta->desc->dependencies, &meta->desc->dependencies_tail, *chdesc);
			
			free(meta);
			if(dependent->type == NOOP && !dependent->dependencies)
				/* we just removed the last dependency of a NOOP chdesc, so free it */
				chdesc_satisfy(&dependent);
		}
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, *chdesc, CHDESC_WRITTEN);
		(*chdesc)->flags |= CHDESC_WRITTEN;
		
		/* we don't need the data in byte change descriptors anymore */
		if((*chdesc)->type == BYTE)
		{
			if((*chdesc)->byte.data)
			{
				free((*chdesc)->byte.data);
				(*chdesc)->byte.data = NULL;
			}
		}
		
		/* make sure we're not already destroying this chdesc */
		if(!((*chdesc)->flags & CHDESC_FREEING))
		{
			assert(!(*chdesc)->free_prev && !(*chdesc)->free_next);
			chdesc_free_push(*chdesc);
		}
	}
	
	chdesc_weak_collect(*chdesc);
	
	if((*chdesc)->type == NOOP)
	{
		if((*chdesc)->flags & CHDESC_BIT_NOOP)
		{
			assert((*chdesc)->noop.bit_changes);
			/* it should already be NULL from the weak reference */
			assert(!hash_map_find_val((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key));
			hash_map_erase((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdesc, CHDESC_BIT_NOOP);
			(*chdesc)->flags &= ~CHDESC_BIT_NOOP;
		}
	}
	
	*chdesc = NULL;
	return 0;
}

int chdesc_weak_retain(chdesc_t * chdesc, chdesc_t ** location)
{
	if(chdesc)
	{
		chrefdesc_t * ref = malloc(sizeof(*ref));
		if(!ref)
			return -E_NO_MEM;
		
		ref->desc = location;
		ref->next = chdesc->weak_refs;
		chdesc->weak_refs = ref;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_RETAIN, chdesc, location);
	}
	
	if(*location && *location != chdesc)
		chdesc_weak_release(location);
	*location = chdesc;
	
	return 0;
}

void chdesc_weak_forget(chdesc_t ** location)
{
	if(*location)
	{
		chrefdesc_t ** prev = &(*location)->weak_refs;
		chrefdesc_t * scan = (*location)->weak_refs;
		while(scan && scan->desc != location)
		{
			prev = &scan->next;
			scan = scan->next;
		}
		if(!scan)
		{
			kdprintf(STDERR_FILENO, "%s: (%s:%d) weak release/forget of non-weak chdesc pointer!\n", __FUNCTION__, __FILE__, __LINE__);
			return;
		}
		*prev = scan->next;
		free(scan);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_FORGET, *location, location);
	}
}

void chdesc_weak_release(chdesc_t ** location)
{
	chdesc_weak_forget(location);
	*location = NULL;
}

void chdesc_destroy(chdesc_t ** chdesc)
{
	/* were we recursively called by chdesc_remove_depend()? */
	if((*chdesc)->flags & CHDESC_FREEING)
		return;
	(*chdesc)->flags |= CHDESC_FREEING;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, *chdesc, CHDESC_FREEING);
	
	if((*chdesc)->flags & CHDESC_WRITTEN)
	{
		assert(!(*chdesc)->dependents && !(*chdesc)->dependencies);
		if(free_head == *chdesc || (*chdesc)->free_prev)
			chdesc_free_remove(*chdesc);
	}
	else
	{
		/* this is perfectly allowed, but while we are switching to this new system, print a warning */
		if((*chdesc)->type != NOOP)
		{
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
			if((*chdesc)->flags & CHDESC_OVERLAP)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying completely overlapping unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
		}
		else if(free_head == *chdesc || (*chdesc)->free_prev)
		{
			assert(!(*chdesc)->dependencies);
			chdesc_free_remove(*chdesc);
		}
	}
	
	if((*chdesc)->dependencies && (*chdesc)->dependents)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying chdesc with both dependents and dependencies!\n", __FUNCTION__, __FILE__, __LINE__);
	/* remove dependencies first, so chdesc_satisfy() won't just turn it to a NOOP */
	while((*chdesc)->dependencies)
		chdesc_remove_depend(*chdesc, (*chdesc)->dependencies->desc);
	if((*chdesc)->dependents)
	{
		/* chdesc_satisfy will set it to NULL */
		chdesc_t * desc = *chdesc;
		chdesc_satisfy(&desc);
	}
	
	chdesc_weak_collect(*chdesc);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_DESTROY, *chdesc);
	
	switch((*chdesc)->type)
	{
		case BYTE:
			if((*chdesc)->byte.data)
				free((*chdesc)->byte.data);
			break;
		case NOOP:
			if((*chdesc)->flags & CHDESC_BIT_NOOP)
			{
				assert((*chdesc)->noop.bit_changes);
				/* it should already be NULL from the weak reference */
				assert(!hash_map_find_val((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key));
				hash_map_erase((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key);
			}
			/* fall through */
		case BIT:
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
	}
	
	if((*chdesc)->block)
		bdesc_release(&(*chdesc)->block);
	
	memset(*chdesc, 0, sizeof(**chdesc));
	free(*chdesc);
	*chdesc = NULL;
}

void chdesc_claim_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->dependencies);
	if(chdesc->free_prev || free_head == chdesc)
		chdesc_free_remove(chdesc);
}

void chdesc_autorelease_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->dependencies && !(chdesc->flags & CHDESC_WRITTEN));
	while(chdesc->dependents)
		chdesc_remove_depend(chdesc->dependents->desc, chdesc);
	if(!chdesc->free_prev && free_head != chdesc)
		chdesc_free_push(chdesc);
}

void chdesc_reclaim_written(void)
{
	while(free_head)
	{
		chdesc_t * first = free_head;
		chdesc_free_remove(first);
		chdesc_destroy(&first);
	}
}

static BD_t * stamps[32] = {0};

uint32_t chdesc_register_stamp(BD_t * bd)
{
	int i;
	for(i = 0; i != 32; i++)
		if(!stamps[i])
		{
			stamps[i] = bd;
			return 1 << i;
		}
	return 0;
}

void chdesc_release_stamp(uint32_t stamp)
{
	if(stamp)
	{
		int i;
		for(i = -1; stamp; i++)
			stamp >>= 1;
		stamps[i] = NULL;
	}
}
