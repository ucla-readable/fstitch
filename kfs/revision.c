#include <inc/fixed_max_heap.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/modman.h>

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

int bdesc_revision_tail_prepare(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (root == 0) {
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
	if (heap == 0)
		panic("out of memory\n");
	d = root->dependencies;
	while (d)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollback
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->block == block) continue;		
		r = chdesc_rollback(c);
		if (r != 0)
			panic("can't rollback!\n");
	}
	fixed_max_heap_free(heap);
	return 0;
}

int bdesc_revision_tail_revert(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (root == 0) {
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
	if (heap == 0)
		panic("out of memory\n");
	d = root->dependencies;
	while (d)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->block == block) continue;		
		r = chdesc_apply(c);
		if (r != 0)
			panic("can't rollforward!\n");
	}
	fixed_max_heap_free(heap);
	return 0;
}

int bdesc_revision_tail_acknowledge(bdesc_t *block, BD_t *bd)
{
	chdesc_t *root;
	chmetadesc_t *d;
	fixed_max_heap_t *heap;
	int r, i;
	int count;

	root = block->ddesc->changes;
	if (root == 0) {
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
	if (heap == 0)
		panic("out of memory\n");
	d = root->dependencies;
	while (d)
		fixed_max_heap_insert(heap, d->desc, d->desc->distance);
	// pop & rollforward
	for (i = 0; i < count; i++) {
		chdesc_t *c = (chdesc_t*)fixed_max_heap_pop(heap);
		if (c->block == block) {
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
