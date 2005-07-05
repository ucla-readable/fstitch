#include <inc/fixed_max_heap.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/revision.h>

#define STRIPPER_DEBUG 0

#if STRIPPER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MAKRED is set to 1 for each chdesc in graph,
 * distance is set to 0 for each chdesc in graph.
 *
 * side effect: returns number of chdescs in the graph. these two
 * functionalities were merged to avoid traversing the graph an extra
 * time.
 */
static int
reset_chdescs(chdesc_t *ch) {
	chmetadesc_t *p;
	int num = 1;
	if (ch->flags & CHDESC_MARKED) return 0;
	ch->flags |= CHDESC_MARKED;
	ch->distance = 0;
	p = ch->dependencies;
	while (p) {
		num += reset_chdescs(p->desc);
		p = p->next;
	}
	return num;
}

/*
 * precondition: CHDESC_MARKED is set to 1 for each chdesc in graph.
 * postcondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 */
static void
reset_marks(chdesc_t *ch)
{
	chmetadesc_t *p;
	if (!(ch->flags & CHDESC_MARKED)) return;
	ch->flags &= ~CHDESC_MARKED;
	p = ch->dependencies;
	while (p) {
		reset_marks(p->desc);
		p = p->next;
	}
}

/*
 * precondition: all nodes have distance set to zero.
 *
 * postconditions: distance of each chdesc is set to (maximum distance
 * from ch) + num.
 *
 * to get the distance from each node to the root, do
 * number_chdescs(root, 0);
 */
static void
number_chdescs(chdesc_t *ch, int num)
{
	chmetadesc_t *p;
	if ((ch->distance >= num) && num != 0) return;
	ch->distance = num;
	p = ch->dependencies;
	while (p) {
		number_chdescs(p->desc, num+1);
		p = p->next;
	}
}

int revision_tail_prepare(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (!root) {
		// XXX handle this?
		return 0;
	}

	reset_chdescs(root);
	reset_marks(root);
	// calculate the distance for all chdescs
	number_chdescs(root, 0);

	// find out how many chdescs are in the block
	count = 0;
	d = root->dependencies;
	while (d) {
		count++;
		d = d->next;
	}

	// heapify
	heap = fixed_max_heap_create(count);
	if (!heap)
		panic("out of memory\n");
	d = root->dependencies;
	while (d) {
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
		d = d->next;
	}
	// pop & rollback
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->owner == bd) continue;		
		r = chdesc_rollback(c);
		if (r != 0)
			panic("can't rollback!\n");
	}
	fixed_max_heap_free(heap);
	return 0;
}

int revision_tail_revert(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (!root) {
		// XXX handle this?
		return 0;
	}

	reset_chdescs(root);
	reset_marks(root);
	// calculate the distance for all chdescs
	number_chdescs(root, 0);

	// find out how many chdescs are in the block
	count = 0;
	d = root->dependencies;
	while (d) {
		count++;
		d = d->next;
	}

	// heapify
	heap = fixed_max_heap_create(count);
	if (!heap)
		panic("out of memory\n");
	d = root->dependencies;
	while (d) {
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
		d = d->next;
	}
	// pop & rollforward
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->owner == bd) continue;		
		r = chdesc_apply(c);
		if (r != 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	return 0;
}

int revision_tail_acknowledge(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (!root) {
		// XXX handle this?
		return 0;
	}

	reset_chdescs(root);
	reset_marks(root);
	// calculate the distance for all chdescs
	number_chdescs(root, 0);

	// find out how many chdescs are in the block
	count = 0;
	d = root->dependencies;
	while (d) {
		count++;
		d = d->next;
	}

	// heapify
	heap = fixed_max_heap_create(count);
	if (!heap)
		panic("out of memory\n");
	d = root->dependencies;
	while (d) {
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
		d = d->next;
	}
	// pop & rollforward
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->owner == bd) {
			chdesc_satisfy(c);
			chdesc_destroy(&c);
			continue;
		}
		r = chdesc_apply(c);
		if (r != 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	return 0;
}

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MAKRED is set to 1 for each chdesc in a
 * connected subgraph. One of the nodes in the subgraph will be the
 * node you pass. Thus, you can run reset_marks() on 'ch' to reset all
 * the marks.
 *
 * return value: a chdesc on a remote BD, or NULL if there is none.
 */
static chdesc_t *
get_external_dep(chdesc_t *ch, BD_t *home_bd)
{
	chmetadesc_t *p;
	chdesc_t *ret;
	if (ch->flags & CHDESC_MARKED) return NULL;
	ch->flags |= CHDESC_MARKED;

	if (ch->owner != home_bd) return ch;

	p = ch->dependencies;
	while (p) {
		ret = get_external_dep(p->desc, home_bd);
		if (ret) return ret;
		p = p->next;
	}
	return NULL;
}

int
commit_chdesc(chdesc_t *ch)
{
	bool is_noop = 0;
	chdesc_t * monitor = (chdesc_t *) 1;
	if (ch->type == NOOP) {
		assert(chdesc_weak_retain(ch, &monitor) >= 0);
		is_noop = 1;
	}

	for (;;) {
		chmetadesc_t * dep;
		if (!monitor)
			return 0;
		dep = ch->dependencies;
		if (dep)
			assert(commit_chdesc(dep->desc) >= 0);
		else
			break;
	}
		
	// write back this ch desc
	assert(is_noop == 0);
	assert(CALL(ch->owner, sync, ch->block->number, ch) >= 0);
	return 0;
}

int
revision_satisfy_external_deps(bdesc_t *block, BD_t *bd)
{
	chdesc_t *ext_dep;
	for (;;) {
		ext_dep = get_external_dep(block->ddesc->changes, bd);
		reset_marks(block->ddesc->changes);
		if (ext_dep == NULL) return 0;
		assert(CALL(ext_dep->owner, sync, ext_dep->block->number, ext_dep) >= 0);
	}
}
