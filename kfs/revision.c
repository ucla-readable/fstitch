#include <inc/fixed_max_heap.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/revision.h>

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MARKED is set to 1 for each chdesc in graph,
 * distance is set to 0 for each chdesc in graph.
 *
 * side effect: returns number of chdescs in the graph. these two
 * functionalities were merged to avoid traversing the graph an extra
 * time.
 */
static int reset_distance(chdesc_t * ch)
{
	chmetadesc_t * p;
	int num = 1;
	if(ch->flags & CHDESC_MARKED)
		return 0;
	ch->flags |= CHDESC_MARKED;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, ch, CHDESC_MARKED);
	ch->distance = 0;
	for(p = ch->dependencies; p; p = p->next)
		num += reset_distance(p->desc);
	return num;
}

/*
 * precondition: all nodes have distance set to zero.
 *
 * postconditions: distance of each chdesc is set to (maximum distance
 * from ch) + num.
 *
 * to get the distance from each node to the root, do
 * calculate_distance(root, 0);
 */
static void calculate_distance(chdesc_t * ch, int num)
{
	chmetadesc_t * p;
	if(num && ch->distance >= num)
		return;
	ch->distance = num;
	for(p = ch->dependencies; p; p = p->next)
		calculate_distance(p->desc, num + 1);
}

int revision_tail_prepare(bdesc_t * block, BD_t * bd)
{
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		// we really want a min heap, so use negative distance
		fixed_max_heap_insert(heap, d->desc, -d->desc->distance);
	// pop & rollback
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(c->owner == bd)
			continue;		
		r = chdesc_rollback(c);
		if(r < 0)
			panic("can't rollback!\n");
	}
	fixed_max_heap_free(heap);
	
	return 0;
}

int revision_tail_revert(bdesc_t * block, BD_t * bd)
{
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(c->owner == bd)
			continue;
		r = chdesc_apply(c);
		if(r < 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	
	return 0;
}

int revision_tail_acknowledge(bdesc_t * block, BD_t * bd)
{
	chdesc_t * root;
	chmetadesc_t * d;
	fixed_max_heap_t * heap;
	int r, i, count = 0;
	
	root = block->ddesc->changes;
	if(!root)
		// XXX handle this?
		return 0;
	
	reset_distance(root);
	chdesc_unmark_graph(root);
	// calculate the distance for all chdescs
	calculate_distance(root, 0);
	
	// find out how many chdescs are in the block
	for(d = root->dependencies; d; d = d->next)
		count++;
	
	// heapify
	heap = fixed_max_heap_create(count);
	if(!heap)
		panic("out of memory\n");
	for(d = root->dependencies; d; d = d->next)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for(i = 0; i < count; i++)
	{
		chdesc_t * c = (chdesc_t *) fixed_max_heap_pop(heap);
		if(c->owner == bd)
		{
			chdesc_destroy(&c);
			continue;
		}
		r = chdesc_apply(c);
		if(r < 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	
	return 0;
}

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MARKED is set to 1 for each chdesc in a
 * connected subgraph. One of the nodes in the subgraph will be the
 * node you pass. Thus, you can run chdesc_unmark_graph() on 'ch' to
 * reset all the marks.
 *
 * return value: a chdesc on a remote BD, or NULL if there is none.
 */
static chdesc_t * get_external_dep(chdesc_t * ch, BD_t * home_bd)
{
	chmetadesc_t * p;
	
	if(ch->flags & CHDESC_MARKED)
		return NULL;
	ch->flags |= CHDESC_MARKED;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, ch, CHDESC_MARKED);
	
	if(ch->owner != home_bd)
		return ch;
	
	for(p = ch->dependencies; p; p = p->next)
	{
		chdesc_t * ret = get_external_dep(p->desc, home_bd);
		if(ret)
			return ret;
	}
	
	return NULL;
}

int revision_satisfy_external_deps(bdesc_t * block, BD_t * bd)
{
	for(;;)
	{
		int r;
		chdesc_t * ext_dep = get_external_dep(block->ddesc->changes, bd);
		chdesc_unmark_graph(block->ddesc->changes);
		if(!ext_dep)
			return 0;
		r = CALL(ext_dep->owner, sync, ext_dep->block->number, ext_dep);
		assert(r >= 0);
	}
}
