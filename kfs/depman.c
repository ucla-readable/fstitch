/* This file contains the magic DEP MAN! */

#include <inc/error.h>
#include <inc/stdio.h>
#include <inc/assert.h>
#include <inc/hash_map.h>

#include <kfs/depman.h>
#include <kfs/bdesc.h>

/* Internally, the dependency manager keeps a hash table that maps block
 * descriptors to change descriptors. The entry in the hash table for each block
 * descriptor is an empty ("NOOP") change descriptor that depends on all the
 * change descriptors currently associated with that block. When the dependency
 * manager is queried, it can simply return these NOOP change descriptors, which
 * are effectively the roots of the DAG subgraphs that the block depends on. */

static hash_map_t * bdesc_hash = NULL;

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
	int r;
	
	if(from == to)
	{
		/* do we need to do anything here? */
		printf("DEP MAN NOTIFY: bdesc 0x%08x -> 0x%08x\n", from, to);
		return 0;
	}
	printf("DEP MAN FORWARD: bdesc 0x%08x -> 0x%08x\n", from, to);
	
	value = (chdesc_t *) hash_map_find_val(bdesc_hash, from);
	if(value)
	{
		r = hash_map_change_key(bdesc_hash, from, to);
		if(r < 0)
			return r;
		value->block = to;
	}
	
	return 0;
}

/* explicitly translate a chdesc when necessary, like for block size alterations that do not happen automatically in bdesc_retain() */
int depman_translate_chdesc(bdesc_t * from, bdesc_t * to, uint32_t offset, uint32_t size)
{
	printf("DEP MAN TRANSLATE: bdesc 0x%08x -> 0x%08x, offset %d, size %d\n", from, to, offset, size);
	return -1;
}

/* add a chdesc subgraph to the dependency manager - this and all reachable chdescs with reference count 0 */
int depman_add_chdesc(chdesc_t * root)
{
	chmetadesc_t * scan;
	chdesc_t * value;
	int r;
	
	for(scan = root->dependencies; scan; scan = scan->next)
		if(!scan->desc->refs)
			if((r = depman_add_chdesc(scan->desc)) < 0)
				return r;
	
	value = (chdesc_t *) hash_map_find_val(bdesc_hash, root->block);
	if(!value)
	{
		value = chdesc_alloc(root->block);
		if(!value)
			return -E_NO_MEM;
		chdesc_retain(value);
		r = hash_map_insert(bdesc_hash, root->block, value);
		if(r < 0)
		{
			chdesc_release(&value);
			return r;
		}
	}
	
	if((r = chdesc_add_depend(value, root)) < 0)
		return r;
	
	chdesc_retain(root);
	return 0;
}

/* remove an individual chdesc from the dependency manager */
int depman_remove_chdesc(chdesc_t * chdesc)
{
	int r;
	chdesc_t * value = (chdesc_t *) hash_map_find_val(bdesc_hash, chdesc->block);
	if(!value)
		return -E_NOT_FOUND;
	chdesc_satisfy(chdesc);
	chdesc_release(&chdesc);
	if(!value->dependencies)
	{
		r = hash_map_erase(bdesc_hash, value->block);
		if(r < 0)
			fprintf(STDERR_FILENO, "%s: hash_map_erase returned %e. TODO: do what?\n", __FUNCTION__, r);
		chdesc_release(&value);
	}
	return 0;
}

/* query the dependency manager */
const chdesc_t * depman_get_deps(bdesc_t * block)
{
	return (chdesc_t *) hash_map_find_val(bdesc_hash, block);
}
