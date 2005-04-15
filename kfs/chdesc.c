#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>
#include <kfs/depman.h>

chdesc_t * chdesc_create_noop(bdesc_t * block)
{
	chdesc_t * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	/* NOOP chdescs start applied */
	chdesc->flags = 0;
	return chdesc;
}

chdesc_t * chdesc_create_bit(bdesc_t * block, uint16_t offset, uint32_t xor)
{
	chdesc_t * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	
	/* start rolled back so we can apply it */
	chdesc->flags = CHDESC_ROLLBACK;
	
	/* make sure it is dependent upon any pre-existing chdescs in depman */
	if(chdesc_overlap_multiattach(chdesc, block))
		goto destroy;
	
	/* make sure it applies cleanly */
	if(chdesc_apply(chdesc))
		destroy: chdesc_destroy(&chdesc);
	
	return chdesc;
}

int chdesc_create_byte(bdesc_t * block, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	uint16_t atomic_size = CALL(block->bd, get_atomicsize);
	uint16_t init_offset = offset % atomic_size;
	uint16_t index = offset / atomic_size;
	uint16_t count = (length + init_offset + atomic_size - 1) / atomic_size;
	uint16_t copied = 0;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
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
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs in depman */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		copied += chdescs[i]->byte.length;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
		{
			destroy: chdesc_destroy(&chdescs[i]);
			break;
		}
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], chdescs[i - 1]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_NO_MEM;
	}
	
	assert(copied == length);
	
	for(i = 0; i != count; i++)
		if(chdesc_apply(chdescs[i]))
			break;
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
			chdesc_rollback(chdescs[i]);
		for(i = 0; i != count; i++)
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	return 0;
}

int chdesc_create_init(bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	uint16_t atomic_size = CALL(block->bd, get_atomicsize);
	uint16_t count = block->ddesc->length / atomic_size;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
		chdescs[i]->block = block;
		chdescs[i]->type = BYTE;
		chdescs[i]->byte.offset = i * atomic_size;
		chdescs[i]->byte.length = atomic_size;
		
		chdescs[i]->byte.olddata = memdup(&block->ddesc->data[i * atomic_size], atomic_size);
		chdescs[i]->byte.newdata = calloc(1, atomic_size);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependents = NULL;
		chdescs[i]->weak_refs = NULL;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs in depman */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
		{
			destroy: chdesc_destroy(&chdescs[i]);
			break;
		}
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], chdescs[i - 1]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_NO_MEM;
	}
	
	for(i = 0; i != count; i++)
		if(chdesc_apply(chdescs[i]))
			break;
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
			chdesc_rollback(chdescs[i]);
		for(i = 0; i != count; i++)
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	return 0;
}

int chdesc_create_full(bdesc_t * block, void * data, chdesc_t ** head, chdesc_t ** tail)
{
	uint16_t atomic_size = CALL(block->bd, get_atomicsize);
	uint16_t count = block->ddesc->length / atomic_size;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	for(i = 0; i != count; i++)
	{
		chdescs[i] = malloc(sizeof(*chdescs[i]));
		if(!chdescs[i])
			break;
		
		chdescs[i]->block = block;
		chdescs[i]->type = BYTE;
		chdescs[i]->byte.offset = i * atomic_size;
		chdescs[i]->byte.length = atomic_size;
		
		chdescs[i]->byte.olddata = memdup(&block->ddesc->data[i * atomic_size], atomic_size);
		chdescs[i]->byte.newdata = memdup(&((uint8_t *) data)[i * atomic_size], atomic_size);
		
		chdescs[i]->dependencies = NULL;
		chdescs[i]->dependents = NULL;
		chdescs[i]->weak_refs = NULL;
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs in depman */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
		{
			destroy: chdesc_destroy(&chdescs[i]);
			break;
		}
	}
	
	/* failed to create the chdescs */
	if(i != count)
	{
		while(i--)
		{
			if(chdescs[i]->dependencies)
				chdesc_remove_depend(chdescs[i], chdescs[i - 1]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_NO_MEM;
	}
	
	for(i = 0; i != count; i++)
		if(chdesc_apply(chdescs[i]))
			break;
	
	/* failed to apply the chdescs */
	if(i != count)
	{
		while(i--)
			chdesc_rollback(chdescs[i]);
		for(i = 0; i != count; i++)
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	return 0;
}

/* make the recent chdesc depend on the given earlier chdesc in the same block if it overlaps */
/* note that we don't check to see if these chdescs are for the same bdesc or not */
int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original)
{
	uint16_t r_start, r_len;
	uint16_t o_start, o_len;
	uint32_t start, end, tag;
	
	/* if either is a NOOP chdesc, they don't conflict */
	if(recent->type == NOOP || original->type == NOOP)
	{
		printf("Unexpected NOOP chdesc in %s()\n", __FUNCTION__);
		return 0;
	}
	/* two bit chdescs can't conflict due to xor representation */
	if(recent->type == BIT && original->type == BIT)
		return 0;
	
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
	
	return chdesc_add_depend(recent, original);
}

int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
{
	chmetadesc_t * scan;
	const chdesc_t * deps = depman_get_deps(block);
	
	if(!deps)
		return 0;
	
	for(scan = deps->dependencies; scan; scan = scan->next)
	{
		int r;
		/* skip marked chdescs - they have just been added to this block during a
		 * depman_translate_chdesc() and already have proper overlap dependency
		 * information with respect to the chdesc now arriving */
		if(!(scan->desc->flags & CHDESC_MARKED))
			continue;
		r = chdesc_overlap_attach(chdesc, scan->desc);
		if(r < 0)
			return r;
	}
	
	return 0;
}

static int chdesc_has_dependency(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
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

void chdesc_unmark_graph(chdesc_t * root)
{
	chmetadesc_t * meta;
	root->flags &= ~CHDESC_MARKED;
	for(meta = root->dependencies; meta; meta = meta->next)
		if(meta->desc->flags & CHDESC_MARKED)
			chdesc_unmark_graph(meta->desc);
}

/* add a dependency to a change descriptor without checking for cycles */
int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
	/* add the dependency to the dependent */
	meta = malloc(sizeof(*meta));
	if(!meta)
		return -E_NO_MEM;
	meta->desc = dependency;
	meta->next = dependent->dependencies;
	dependent->dependencies = meta;
	
	/* add the dependent to the dependency */
	meta = malloc(sizeof(*meta));
	if(!meta)
	{
		meta = dependent->dependencies;
		dependent->dependencies = meta->next;
		free(meta);
		return -E_NO_MEM;
	}
	meta->desc = dependent;
	meta->next = dependency->dependents;
	dependency->dependents = meta;
	
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

int chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chdesc_meta_remove(&dependent->dependencies, dependency);
	chdesc_meta_remove(&dependency->dependents, dependent);
	return 0;
}

int chdesc_apply(chdesc_t * chdesc)
{
	int r;
	if(!(chdesc->flags & CHDESC_ROLLBACK))
		return -E_INVAL;
	if(chdesc->block && (r = bdesc_touch(chdesc->block)) < 0)
		return r;
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
	return 0;
}

int chdesc_rollback(chdesc_t * chdesc)
{
	int r;
	if(chdesc->flags & CHDESC_ROLLBACK)
		return -E_INVAL;
	if(chdesc->block && (r = bdesc_touch(chdesc->block)) < 0)
		return r;
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
	return 0;
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc)
{
	while(chdesc->dependents)
	{
		chmetadesc_t * meta = chdesc->dependents;
		chdesc->dependents = meta->next;
		chdesc_meta_remove(&meta->desc->dependencies, chdesc);
		free(meta);
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
	}
}

void chdesc_weak_release(chdesc_t ** location)
{
	chdesc_weak_forget(location);
	*location = NULL;
}

static void chdesc_weak_collect(chdesc_t * chdesc)
{
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

int chdesc_destroy(chdesc_t ** chdesc)
{
	if((*chdesc)->dependents || (*chdesc)->flags & CHDESC_IN_DEPMAN)
		return -E_INVAL;
	
	while((*chdesc)->dependencies)
		chdesc_remove_depend(*chdesc, (*chdesc)->dependencies->desc);
	
	chdesc_weak_collect(*chdesc);
	
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
	
	memset(*chdesc, 0, sizeof(**chdesc));
	free(*chdesc);
	*chdesc = NULL;
	
	return 0;
}
