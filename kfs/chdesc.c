#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

/* ensure bdesc->ddesc->changes has a noop chdesc */
static int ensure_bdesc_changes(bdesc_t * block);

/* perform overlap attachment */
static int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original);
static int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block);

/* add a dependency to a change descriptor without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency);

static int ensure_bdesc_changes(bdesc_t * block)
{
	if(block->ddesc->changes)
	{
		assert(block->ddesc->changes->type == NOOP);
		return 0;
	}
	
	block->ddesc->changes = malloc(sizeof(*block->ddesc->changes));
	if(!block->ddesc->changes)
		return -E_NO_MEM;
	
	block->ddesc->changes->owner = NULL;
	block->ddesc->changes->block = NULL;
	block->ddesc->changes->type = NOOP;
	block->ddesc->changes->dependencies = NULL;
	block->ddesc->changes->dependents = NULL;
	block->ddesc->changes->weak_refs = NULL;
	/* NOOP chdescs start applied */
	block->ddesc->changes->flags = 0;
	
	if(chdesc_weak_retain(block->ddesc->changes, &block->ddesc->changes))
	{
		free(block->ddesc->changes);
		block->ddesc->changes = NULL;
		return -E_NO_MEM;
	}
	
	return 0;
}

chdesc_t * chdesc_create_noop(bdesc_t * block, BD_t * owner)
{
	chdesc_t * chdesc;
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	/* NOOP chdescs start applied */
	chdesc->flags = 0;
	
	/* add chdesc to block's dependencies */
	if(ensure_bdesc_changes(block) < 0)
	{
		free(chdesc);
		return NULL;
	}
	if(chdesc_add_depend_fast(block->ddesc->changes, chdesc) < 0)
	{
		const int r = chdesc_destroy(&block->ddesc->changes);
		assert(r >= 0);
		free(chdesc);
		return NULL;
	}
	
	/* make sure our block sticks around */
	if(block)
		bdesc_retain(block);
	
	return chdesc;
}

chdesc_t * chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor)
{
	chdesc_t * chdesc;
	int r;
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	
	/* start rolled back so we can apply it */
	chdesc->flags = CHDESC_ROLLBACK;
	
	/* make sure it is dependent upon any pre-existing chdescs */
	if(chdesc_overlap_multiattach(chdesc, block))
		goto error;
	
	/* make sure it applies cleanly */
	if(chdesc_apply(chdesc))
		goto error;
	
	/* add chdesc to block's dependencies */
	if((r = ensure_bdesc_changes(block)) < 0)
		goto error;
	if((r = chdesc_add_depend_fast(block->ddesc->changes, chdesc)) < 0)
		goto error_ensure;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	return chdesc;
	
  error_ensure:
	r = chdesc_destroy(&(block->ddesc->changes));
	assert(r >= 0);
  error:
	chdesc_destroy(&chdesc);
	return NULL;
}

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
	
	if((r = ensure_bdesc_changes(block)) < 0)
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
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		copied += chdescs[i]->byte.length;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
			goto destroy;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
		    destroy:
			chdesc_destroy(&chdescs[i]);
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
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
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
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
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
	
	if((r = ensure_bdesc_changes(block)) < 0)
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
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
			goto destroy;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
			r = chdesc_destroy(&(block->ddesc->changes));
			assert(r >= 0);
		    destroy:
			chdesc_destroy(&chdescs[i]);
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
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
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
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	free(chdescs);
	
	return 0;
}

int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t count = block->ddesc->length / atomic_size;
	chdesc_t ** chdescs = malloc(sizeof(*chdescs) * count);
	int i, r;
	
	if(!chdescs)
		return -E_NO_MEM;
	
	if((r = ensure_bdesc_changes(block)) < 0)
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
		
		/* start rolled back so we can apply it */
		chdescs[i]->flags = CHDESC_ROLLBACK;
		
		if(!chdescs[i]->byte.olddata || !chdescs[i]->byte.newdata)
			goto destroy;
		
		/* make sure it is dependent upon any pre-existing chdescs */
		if(chdesc_overlap_multiattach(chdescs[i], block))
			goto destroy;
		
		if(i && chdesc_add_depend(chdescs[i], chdescs[i - 1]))
			goto destroy;
		
		if((r = chdesc_add_depend_fast(block->ddesc->changes, chdescs[i])) < 0)
		{
			r = chdesc_destroy(&(block->ddesc->changes));
			assert(r >= 0);
			
		    destroy:
			chdesc_destroy(&chdescs[i]);
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
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
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
		{
			chdesc_satisfy(chdescs[i]);
			chdesc_destroy(&chdescs[i]);
		}
		free(chdescs);
		
		r = chdesc_destroy(&(block->ddesc->changes));
		assert(r >= 0);
		
		return -E_INVAL;
	}
	
	*head = chdescs[count - 1];
	*tail = chdescs[0];
	
	free(chdescs);
	
	return 0;
}

/* make the recent chdesc depend on the given earlier chdesc in the same block if it overlaps */
/* note that we don't check to see if these chdescs are for the same ddesc or not */
static int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original)
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

static int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
{
	chmetadesc_t * scan;
	const chdesc_t * deps = block->ddesc->changes;
	
	if(!deps)
		return 0;
	
	for(scan = deps->dependencies; scan; scan = scan->next)
	{
		int r;
		/* skip moved chdescs - they have just been added to this block
		 * by chdesc_move() and already have proper overlap dependency
		 * information with respect to the chdesc now arriving */
		if(!(scan->desc->flags & CHDESC_MOVED))
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

int chdesc_move(chdesc_t * chdesc, bdesc_t * destination, uint16_t source_offset)
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
	
	r = ensure_bdesc_changes(destination);
	if(r < 0)
		return r;
	
	r = chdesc_add_depend_fast(destination->ddesc->changes, chdesc);
	if(r < 0)
	{
	    kill_stub:
		if(!destination->ddesc->changes->dependencies)
		{
			/* can't fail */
			const int s = chdesc_destroy(&(destination->ddesc->changes));
			assert(s >= 0);
		}
		return r;
	}
	
	r = chdesc_overlap_multiattach(chdesc, destination);
	if(r < 0)
	{
		chdesc_remove_depend(destination->ddesc->changes, chdesc);
		goto kill_stub;
	}
	
	/* at this point we have succeeded in moving the chdesc */
	
	if(offset)
		*offset -= source_offset;
	
	chdesc->flags |= CHDESC_MOVED;
	if(chdesc->block)
	{
		/* shouldn't fail... */
		r = chdesc_remove_depend(chdesc->block->ddesc->changes, chdesc);
		assert(r >= 0);
		bdesc_release(&chdesc->block);
	}
	chdesc->block = destination;
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

/* add a dependency to a change descriptor without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
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
	if(dependent->type == NOOP && !dependent->dependencies)
		/* we just removed the last dependency of a NOOP chdesc, so free it */
		chdesc_destroy(&dependent);
	return 0;
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
	return 0;
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc)
{
	while(chdesc->dependents)
	{
		chmetadesc_t * meta = chdesc->dependents;
		chdesc_t * dependent = meta->desc;
		chdesc->dependents = meta->next;
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
	if((*chdesc)->dependents)
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
