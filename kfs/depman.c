/* This file contains the magic DEP MAN! */

#include <inc/stdio.h>

#include <kfs/depman.h>
#include <kfs/bdesc.h>

/* Internally, the dependency manager keeps a hash table that maps block
 * descriptors to change descriptors. The entry in the hash table for each block
 * descriptor is an empty ("NOOP") change descriptor that depends on all the
 * change descriptors currently associated with that block. When the dependency
 * manager is queried, it can simply return these NOOP change descriptors, which
 * are effectively the roots of the DAG subgraphs that the block depends on. */

int depman_forward_chdesc(bdesc_t * from, bdesc_t * to)
{
	if(from == to)
		printf("DEP MAN NOTIFY: bdesc 0x%08x -> 0x%08x\n", from, to);
	else
		printf("DEP MAN FORWARD: bdesc 0x%08x -> 0x%08x\n", from, to);
	return 0;
}

int depman_add_chdesc(chdesc_t * chdesc)
{
	return -1;
}

int depman_remove_chdesc(chdesc_t * chdesc)
{
	return -1;
}

chdesc_t * depman_get_deps(bdesc_t * block)
{
	return NULL;
}
