#include <inc/error.h>
#include <inc/stdio.h>
#include <inc/assert.h>
#include <inc/hash_map.h>

#include <kfs/depman.h>
#include <kfs/bdesc.h>

#define DEPMAN_DEBUG 0

#if DEPMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* Internally, the dependency manager keeps a hash table that maps block
 * descriptors to change descriptors. The entry in the hash table for each block
 * descriptor is an empty ("NOOP") change descriptor that depends on all the
 * change descriptors currently associated with that block. When the dependency
 * manager is queried, it can simply return these NOOP change descriptors, which
 * are effectively the roots of the DAG subgraphs that the block depends on. */

static hash_map_t * bdesc_hash = NULL;
static chdesc_t null_noops = {block: NULL, type: NOOP, dependencies: NULL, dependents: NULL, weak_refs: NULL, flags: CHDESC_IN_DEPMAN};

/* initialize the dependency manager */
int depman_init(void)
{
	/* check that depman_init() is not called more than once */
	assert(!bdesc_hash);

	bdesc_hash = hash_map_create_size(64, 1);
	if(!bdesc_hash)
		return -E_NO_MEM;
	return 0;
}

/* forward a chdesc through bdesc translation automatically, from bdesc_retain() */
int depman_forward_chdesc(bdesc_t * from, bdesc_t * to)
{
	chdesc_t * value;
	chdesc_t * dest;
	
	if(from == to)
	{
		/* do we need to do anything here? */
		Dprintf("DEP MAN NOTIFY: bdesc 0x%08x -> 0x%08x\n", from, to);
		return 0;
	}
	Dprintf("DEP MAN FORWARD: bdesc 0x%08x -> 0x%08x\n", from, to);
	
	value = (chdesc_t *) hash_map_find_val(bdesc_hash, from);
	dest = (chdesc_t *) hash_map_find_val(bdesc_hash, to);
	if(value)
	{
		if(dest)
		{
			/* already chdescs for the new bdesc */
			while(value->dependencies)
			{
				chdesc_t * desc = value->dependencies->desc;
				desc->block = to;
				/* FIXME check for errors here? */
				chdesc_overlap_multiattach(desc, dest->block);
				/* FIXME reuse the memory for the metadesc? */
				/* chdesc_move_depend(value, dest, desc); */
				chdesc_remove_depend(value, desc);
				chdesc_add_depend(dest, desc);
			}
			hash_map_erase(bdesc_hash, value->block);
			value->flags &= ~CHDESC_IN_DEPMAN;
			chdesc_destroy(&value);
		}
		else
		{
			chmetadesc_t * scan;
			int r = hash_map_change_key(bdesc_hash, from, to);
			if(r < 0)
				return r;
			value->block = to;
			for(scan = value->dependencies; scan; scan = scan->next)
				scan->desc->block = to;
		}
	}
	
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

/* explicitly translate a chdesc when necessary, like for block size alterations that do not happen automatically in bdesc_retain() */
int depman_translate_chdesc(bdesc_t * from, bdesc_t * to, uint32_t offset, uint32_t size)
{
	chdesc_t * value;
	int r;
	
	if(from == to)
	{
		/* do we need to do anything here? */
		Dprintf("DEP MAN NOTIFY RANGE: bdesc 0x%08x -> 0x%08x, offset %d, size %d\n", from, to, offset, size);
		return 0;
	}
	Dprintf("DEP MAN TRANSLATE: bdesc 0x%08x -> 0x%08x, offset %d, size %d\n", from, to, offset, size);
	
	value = (chdesc_t *) hash_map_find_val(bdesc_hash, from);
	if(value)
	{
		/* this code interacts in a complicated way with the chdesc functions it calls... */
		chdesc_t * dest = (chdesc_t *) hash_map_find_val(bdesc_hash, to);
		chmetadesc_t ** list = &value->dependencies;
		chmetadesc_t * scan = *list;
		while(scan)
		{
			chdesc_t * desc = scan->desc;
			if(chdesc_in_range(desc, offset, size))
			{
				if(!dest)
				{
					dest = chdesc_create_noop(to);
					if(!dest)
						return -E_NO_MEM;
					if((r = hash_map_insert(bdesc_hash, to, dest)) < 0)
					{
						chdesc_destroy(&dest);
						return r;
					}
					dest->flags |= CHDESC_IN_DEPMAN;
				}
				desc->block = to;
				/* set the MARKED flag to make sure chdesc_overlap_multiattach()
				 * below does not attach later chdescs during this translation to
				 * chdescs we just moved, because we are moving them in reverse
				 * order and otherwise we'd get cyclic dependencies */
				assert(!(desc->flags & CHDESC_MARKED));
				desc->flags |= CHDESC_MARKED;
				switch(desc->type)
				{
					case BIT:
						desc->bit.offset -= offset / sizeof(desc->bit.xor);
						break;
					case BYTE:
						desc->byte.offset -= offset;
						break;
					case NOOP:
						break;
					default:
						printf("%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, desc->type);
				}
				/* FIXME check for errors here? */
				chdesc_overlap_multiattach(desc, dest->block);
				/* FIXME reuse the memory for the metadesc? */
				/* chdesc_move_depend(value, dest, desc); */
				chdesc_remove_depend(value, desc);
				chdesc_add_depend(dest, desc);
			}
			else
				list = &scan->next;
			scan = *list;
		}
		/* if there are no more chdescs for this bdesc, remove the stub NOOP chdesc */
		if(!value->dependencies)
		{
			chdesc_t * value_erase = hash_map_erase(bdesc_hash, value->block);
			assert(value == value_erase);
			assert(!value->dependents);
			value->flags &= ~CHDESC_IN_DEPMAN;
			chdesc_destroy(&value);
		}
		/* unmark all the chdescs marked above (the ones we just moved, if any) */
		if(dest)
			for(scan = dest->dependencies; scan; scan = scan->next)
				scan->desc->flags &= ~CHDESC_MARKED;
	}
	
	return 0;
}

/* add a chdesc subgraph to the dependency manager */
int depman_add_chdesc(chdesc_t * root)
{
	chmetadesc_t * scan;
	chdesc_t * value;
	int r;
	
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!(scan->desc->flags & CHDESC_IN_DEPMAN))
			if((r = depman_add_chdesc(scan->desc)) < 0)
				return r;
	
	if(!root->block)
		value = &null_noops;
	else
	{
		value = (chdesc_t *) hash_map_find_val(bdesc_hash, root->block);
		if(!value)
		{
			value = chdesc_create_noop(root->block);
			if(!value)
				return -E_NO_MEM;
			r = hash_map_insert(bdesc_hash, root->block, value);
			if(r < 0)
			{
				/* can't fail */
				chdesc_destroy(&value);
				return r;
			}
			value->flags |= CHDESC_IN_DEPMAN;
		}
	}
	
	if((r = chdesc_add_depend(value, root)) < 0)
	{
		if(!value->dependencies && value->block)
		{
			chdesc_t * value_erase = hash_map_erase(bdesc_hash, value->block);
			assert(value == value_erase);
			value->flags &= ~CHDESC_IN_DEPMAN;
			/* can't fail */
			chdesc_destroy(&value);
		}
		return r;
	}
	
	root->flags |= CHDESC_IN_DEPMAN;
	return 0;
}

/* remove an individual chdesc from the dependency manager */
int depman_remove_chdesc(chdesc_t * chdesc)
{
	chdesc_t * value = &null_noops;
	
	if(chdesc->block)
		value = (chdesc_t *) hash_map_find_val(bdesc_hash, chdesc->block);
	if(!value)
		return -E_NOT_FOUND;
	chdesc_satisfy(chdesc);
	chdesc->flags &= ~CHDESC_IN_DEPMAN;
	/* can't fail after chdesc_satisfy() */
	chdesc_destroy(&chdesc);
	/* if there are no more chdescs for this bdesc, remove the stub NOOP chdesc */
	if(!value->dependencies && value->block)
	{
		chdesc_t * value_erase = hash_map_erase(bdesc_hash, value->block);
		assert(value == value_erase);
		value->flags &= ~CHDESC_IN_DEPMAN;
		/* can't fail */
		chdesc_destroy(&value);
	}
	return 0;
}

/* query the dependency manager */
const chdesc_t * depman_get_deps(bdesc_t * block)
{
	return (chdesc_t *) hash_map_find_val(bdesc_hash, block);
}
