#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/debug.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

/* ensure bdesc->ddesc->changes has a noop chdesc */
static int ensure_bdesc_has_changes(bdesc_t * block)
{
	assert(block);

	if(block->ddesc->changes)
	{
		assert(block->ddesc->changes->type == NOOP);
		return 0;
	}
	
	block->ddesc->changes = malloc(sizeof(*block->ddesc->changes));
	if(!block->ddesc->changes)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_NOOP, block->ddesc->changes, NULL, NULL);
	
	block->ddesc->changes->owner = NULL;
	block->ddesc->changes->block = NULL;
	block->ddesc->changes->type = NOOP;
	block->ddesc->changes->dependencies = NULL;
	block->ddesc->changes->dependents = NULL;
	block->ddesc->changes->weak_refs = NULL;
	block->ddesc->changes->stamps = 0;
	
	/* NOOP chdescs start applied */
	block->ddesc->changes->flags = 0;
	
	if(chdesc_weak_retain(block->ddesc->changes, &block->ddesc->changes))
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_DESTROY, block->ddesc->changes);
		free(block->ddesc->changes);
		block->ddesc->changes = NULL;
		return -E_NO_MEM;
	}
	
	return 0;
}

/* add a dependency to a change descriptor without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
	/* add the dependency to the dependent */
	meta = malloc(sizeof(*meta));
	if(!meta)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENCY, dependent, dependency);
	meta->desc = dependency;
	meta->next = dependent->dependencies;
	dependent->dependencies = meta;
	
	/* add the dependent to the dependency */
	meta = malloc(sizeof(*meta));
	if(!meta)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, dependent, dependency);
		meta = dependent->dependencies;
		dependent->dependencies = meta->next;
		free(meta);
		return -E_NO_MEM;
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENT, dependency, dependent);
	meta->desc = dependent;
	meta->next = dependency->dependents;
	dependency->dependents = meta;
	
	return 0;
}

/* make the recent chdesc depend on the given earlier chdesc in the same block if it overlaps */
/* note that we don't check to see if these chdescs are for the same ddesc or not */
static int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original)
{
	uint16_t r_start, r_len;
	uint16_t o_start, o_len;
	uint32_t start, end, tag;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_ATTACH, recent, original);
	
	/* if either is a NOOP chdesc, they don't conflict */
	if(recent->type == NOOP || original->type == NOOP)
	{
		printf("Unexpected NOOP chdesc in %s()\n", __FUNCTION__);
		return 0;
	}
	
	/* two bit chdescs can't conflict due to xor representation... */
	if(recent->type == BIT && original->type == BIT)
	{
		/* ...but make sure they depend if they modify the same bits */
		if(recent->bit.offset == original->bit.offset && (recent->bit.xor & original->bit.xor))
			chdesc_add_depend(recent, original);
		return 0;
	}
	
	if(recent->type == BIT)
	{
		r_len = sizeof(recent->bit.xor);
		r_start = recent->bit.offset * r_len;
	}
	else
	{
		r_len = recent->byte.length;
		r_start = recent->byte.offset;
	}
	if(original->type == BIT)
	{
		o_len = sizeof(original->bit.xor);
		o_start = original->bit.offset * o_len;
	}
	else
	{
		o_len = original->byte.length;
		o_start = original->byte.offset;
	}
	
	start = o_start;
	end = start + o_len + r_len;
	tag = r_start + r_len;
	if(tag <= start || end <= tag)
		return 0;
	
	if(original->flags & CHDESC_ROLLBACK)
	{
		/* it's not clear what to do in this case... just fail with a warning for now */
		fprintf(STDERR_FILENO, "Attempt to overlap a new chdesc with a rolled-back chdesc!\n");
		return -E_BUSY;
	}
	return chdesc_add_depend(recent, original);
}

static int chdesc_overlap_multiattach_slip(chdesc_t * chdesc, bdesc_t * block, bool slip_under)
{
	chmetadesc_t * scan;
	const chdesc_t * deps = block->ddesc->changes;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_MULTIATTACH, chdesc, block, slip_under);
	
	if(!deps)
		return 0;
	
	for(scan = deps->dependencies; scan; scan = scan->next)
	{
		int r;
		/* skip moved chdescs - they have just been added to this block
		 * by chdesc_move() and already have proper overlap dependency
		 * information with respect to the chdesc now arriving */
		if(scan->desc->flags & CHDESC_MOVED)
			continue;
		/* "Slip Under" allows us to create change descriptors
		 * underneath existing ones. (That is, existing chdescs will
		 * depend on the new one, not the other way around.) This is a
		 * hidden feature for internal use only. */
		if(slip_under)
			r = chdesc_overlap_attach(scan->desc, chdesc);
		else
			r = chdesc_overlap_attach(chdesc, scan->desc);
		if(r < 0)
			return r;
	}
	
	return 0;
}

static int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
{
	return chdesc_overlap_multiattach_slip(chdesc, block, 0);
}

int __ensure_bdesc_has_changes(bdesc_t * block) __attribute__ ((alias("ensure_bdesc_has_changes")));
int __chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency) __attribute__((alias("chdesc_add_depend_fast")));
int __chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block) __attribute__((alias("chdesc_overlap_multiattach")));

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
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	chdesc->stamps = 0;
	
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
			if(!block->ddesc->changes->dependencies)
				chdesc_destroy(&block->ddesc->changes);
			free(chdesc);
			return NULL;
		}
		
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	return chdesc;
}

chdesc_t * chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor)
{
	chdesc_t * chdesc;
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
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	chdesc->stamps = 0;
	
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
		goto error_ensure;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	return chdesc;
	
  error_ensure:
	if(!block->ddesc->changes->dependencies)
		chdesc_destroy(&(block->ddesc->changes));
  error:
	chdesc_destroy(&chdesc);
	return NULL;
}

#warning FIXME provide notification and/or specification of whether this change is/should be a single chdesc
int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head, chdesc_t ** tail)
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
	
	if((r = ensure_bdesc_has_changes(block)) < 0)
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
		
		chdescs[i]->byte.olddata = memdup(&block->ddesc->data[chdescs[i]->byte.offset], chdescs[i]->byte.length);
		chdescs[i]->byte.newdata = memdup(&((uint8_t *) data)[copied], chdescs[i]->byte.length);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependents = NULL;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->stamps = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, chdescs[i]->byte.offset, chdescs[i]->byte.length);
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		copied += chdescs[i]->byte.length;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
			if(!block->ddesc->changes->dependencies)
				chdesc_destroy(&(block->ddesc->changes));
			
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		
		/* FIXME: change this to _fast, but split apart the *head case */
		if((i || *head) && chdesc_add_depend(chdescs[i], i ? chdescs[i - 1] : *head))
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	free(chdescs);
	
	return 0;
}

int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head, chdesc_t ** tail)
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
		
		chdescs[i]->byte.olddata = memdup(&block->ddesc->data[i * atomic_size], atomic_size);
		chdescs[i]->byte.newdata = calloc(1, atomic_size);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependents = NULL;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->stamps = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, i * atomic_size, atomic_size);
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
			if(!block->ddesc->changes->dependencies)
				chdesc_destroy(&(block->ddesc->changes));
			
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		
		/* FIXME: change this to _fast, but split apart the *head case */
		if((i || *head) && chdesc_add_depend(chdescs[i], i ? chdescs[i - 1] : *head))
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	free(chdescs);
	
	return 0;
}

#warning FIXME provide notification and/or specification of whether this change is/should be a single chdesc
int __chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail, bool slip_under)
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
		
		chdescs[i]->byte.olddata = memdup(&block->ddesc->data[i * atomic_size], atomic_size);
		chdescs[i]->byte.newdata = memdup(&((uint8_t *) data)[i * atomic_size], atomic_size);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependents = NULL;
		chdescs[i]->weak_refs = NULL;
		chdescs[i]->stamps = 0;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdescs[i], block, owner, i * atomic_size, atomic_size);
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach_slip(chdescs[i], block, slip_under))
			goto destroy;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
			if(!block->ddesc->changes->dependencies)
				chdesc_destroy(&(block->ddesc->changes));
			
		    destroy:
			chdesc_destroy(&chdescs[i]);
			break;
		}
		
		/* FIXME: change this to _fast, but split apart the *head case */
		if((i || *head) && chdesc_add_depend(chdescs[i], i ? chdescs[i - 1] : *head))
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
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
		
		chdesc_destroy(&(block->ddesc->changes));
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	free(chdescs);
	
	return 0;
}

int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return __chdesc_create_full(block, owner, data, head, tail, 0);
}

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

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
	/* compensate for Heisenberg's uncertainty principle */
	if(!dependent || !dependency)
		return 0;
	
	/* first make sure it's not already there */
	for(meta = dependent->dependencies; meta; meta = meta->next)
		if(meta->desc == dependency)
			return 0;
	
	/* avoid creating a dependency loop */
	if(dependent == dependency || chdesc_has_dependency(dependency, dependent))
	{
		printf("%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	/* chdesc_has_dependency() marks the DAG rooted at "dependency" so we must unmark it */
	chdesc_unmark_graph(dependency);
	
	return chdesc_add_depend_fast(dependent, dependency);
}

static void chdesc_meta_remove(chmetadesc_t ** list, chdesc_t * chdesc)
{
	chmetadesc_t * scan = *list;
	while(scan)
	{
		if(scan->desc == chdesc)
		{
			*list = scan->next;
			free(scan);
			scan = *list;
			/* could return here, but keep going just to be sure */
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
	chdesc_meta_remove(&dependent->dependencies, dependency);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENT, dependency, dependent);
	chdesc_meta_remove(&dependency->dependents, dependent);
	
	if(dependent->type == NOOP && !dependent->dependencies)
		/* we just removed the last dependency of a NOOP chdesc, so free it */
		chdesc_destroy(&dependent);
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
			if(!chdesc->byte.newdata)
				return -E_INVAL;
			memcpy(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.newdata, chdesc->byte.length);
			break;
		case NOOP:
			printf("%s(): applying NOOP chdesc\n", __FUNCTION__);
			break;
		default:
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
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
			if(!chdesc->byte.olddata)
				return -E_INVAL;
			memcpy(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.olddata, chdesc->byte.length);
			break;
		case NOOP:
			printf("%s(): rolling back NOOP chdesc\n", __FUNCTION__);
			break;
		default:
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	chdesc->flags |= CHDESC_ROLLBACK;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ROLLBACK, chdesc);
	return 0;
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_SATISFY, chdesc);
	while(chdesc->dependents)
	{
		chmetadesc_t * meta = chdesc->dependents;
		chdesc_t * dependent = meta->desc;
		chdesc->dependents = meta->next;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENT, chdesc, meta->desc);
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, meta->desc, chdesc);
		chdesc_meta_remove(&meta->desc->dependencies, chdesc);
		
		free(meta);
		if(dependent->type == NOOP && !dependent->dependencies)
			/* we just removed the last dependency of a NOOP chdesc, so free it */
			chdesc_destroy(&dependent);
	}
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
			fprintf(STDERR_FILENO, "%s: weak release/forget of non-weak chdesc pointer!\n", __FUNCTION__);
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
			fprintf(STDERR_FILENO, "%s: dangling chdesc weak reference!\n", __FUNCTION__);
			chdesc->weak_refs = next->next;
			free(next);
		}
	}
}

void chdesc_destroy(chdesc_t ** chdesc)
{
	/* were we recursively called by chdesc_remove_depend()? */
	if((*chdesc)->flags & CHDESC_FREEING)
		return;
	(*chdesc)->flags |= CHDESC_FREEING;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, *chdesc, CHDESC_FREEING);
	
	if((*chdesc)->dependents)
		chdesc_satisfy(*chdesc);
	
	while((*chdesc)->dependencies)
		chdesc_remove_depend(*chdesc, (*chdesc)->dependencies->desc);
	
	chdesc_weak_collect(*chdesc);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_DESTROY, *chdesc);
	
	switch((*chdesc)->type)
	{
		case BIT:
			break;
		case BYTE:
			if((*chdesc)->byte.olddata)
				free((*chdesc)->byte.olddata);
			if((*chdesc)->byte.newdata)
				free((*chdesc)->byte.newdata);
			break;
		case NOOP:
			break;
		default:
			fprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
	}
	
	if((*chdesc)->block)
		bdesc_release(&(*chdesc)->block);
	
	memset(*chdesc, 0, sizeof(**chdesc));
	free(*chdesc);
	*chdesc = NULL;
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
