// in this file the words 'node' and 'chdesc' are interchangable.

#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/fixed_max_heap.h>
#include <inc/vector.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/chdesc.h>
#include <kfs/depman.h>

struct cache_info {
	BD_t * bd;
	uint32_t size;
	bdesc_t ** blocks;
	uint16_t blocksize;
};

static uint32_t
wb_cache_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct cache_info *) object->instance)->bd, get_numblocks);
}

static uint16_t
wb_cache_bd_get_blocksize(BD_t * object)
{
	return ((struct cache_info *) object->instance)->blocksize;
}

static uint16_t
wb_cache_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct cache_info *) object->instance)->bd, get_atomicsize);
}

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
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MAKRED is set to 1 for each chdesc in graph,
 * distance of each chdesc is set to (maximum distance from ch) + num.
 *
 * to get the distance from each node to the root, do
 * number_chdescs(root, 0);
 */
static void
number_chdescs(chdesc_t *ch, int num)
{
	chmetadesc_t *p;
	if (ch->flags & CHDESC_MARKED) return;
	ch->flags |= CHDESC_MARKED;
	if (ch->distance < num)
		ch->distance = num;
	p = ch->dependencies;
	while (p) {
		number_chdescs(p->desc, num+1);
		p = p->next;
	}
}

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MAKRED is set to 1 for each chdesc in graph,
 * heap contains all chdescs in graph.
 * 
 * the weight assigned to each chdesc in the graph is its distance.
 */
static void
heapify_nodes(chdesc_t *ch, fixed_max_heap_t *heap)
{
	chmetadesc_t *p;
	if (ch->flags & CHDESC_MARKED) return;
	ch->flags |= CHDESC_MARKED;
	fixed_max_heap_insert(heap, ch, ch->distance);
	p = ch->dependencies;
	while (p) {
		heapify_nodes(p->desc, heap);
		p = p->next;
	}
}

/*
 * ch is assumed to be in block, one hop from the NOOP root.
 * if a chdesc is in block, return 1 if any of these hold:
 * 
 * - chdesc is not in heap
 * - chdesc has out of block dependencies
 * - chdesc depends on a chdesc that will be rolled back (assumes that
 *   CHDESC_MARKED is set for chdescs that will be rolled back.)
 *
 * NOTE: you must call this fxn on older chdescs first. i.e., if A and
 * B are in the block and A -> B, you must call this fxn on B, then on
 * A.
 *
 * NOTE2: this function will not set the CHDESC_MARKED flag (notice
 * the const).
 */
static bool
should_rollback(const bdesc_t *block, const fixed_max_heap_t *heap,
				const chdesc_t *ch)
{
	chmetadesc_t *q;
	chdesc_t *dep;
	if (!fixed_max_heap_contains(heap, ch)) return 0;

	// if no dependencies, no need to roll back
	if (!ch->dependencies) return 0;
	// for each of ch's dependencies
	for (q = ch->dependencies; q != NULL; q = q->next) {
		if (q->desc->block != block)
			return 1; // we have an out of block dependency
		// we have an in-block dependency. will the dep be rolled back?
		dep = q->desc;
		if (dep->flags & CHDESC_MARKED)
			return 1;
	}
	return 0;
}

// for vector_sort()
int compare_chdescs(const void *a, const void *b)
{
	chdesc_t *ca = (chdesc_t*)a;
	chdesc_t *cb = (chdesc_t*)b;
	if (ca->distance < cb->distance) return -1;
	if (ca->distance > cb->distance) return 1;
	return 0;
}

/*
 * rollback all chdecs in block that meet any of these criteria:
 *
 * - the chdesc is not in the heap
 * - the chdesc has out of block dependencies
 * - the chdesc points to a chdesc that will be rolled back
 *
 * rolled back block will be inserted into 'rollback' so that they may
 * be later rolled forward. the chdescs that should be satisfied will
 * be inserted into 'satisfy'.
 */
static void
rollback_block(bdesc_t *block, fixed_max_heap_t *heap,
			   vector_t *rollback, vector_t *satisfy)
{
	chdesc_t *root;
	chmetadesc_t *p;
	vector_t *vect;
	int r, i;
	chdesc_t *ch;

	vect = vector_create();
	if (vect == NULL) {
		panic("vector_create failed!\n");
	}
	root = (chdesc_t*)depman_get_deps(block);
	// foreach chedsc ch in block:
	for (p = root->dependencies; p != NULL; p = p->next) {
		ch = p->desc;
		vector_push_back(vect, ch);
	}

	// remember, if two chdescs A and B are in the block and there is
	// a (possibly indirect) dependency between them (A -> B), we
	// either roll back neither, just A, or both (i.e. if we rollback
	// B, we must rollback A). notice that this convers the case where
	// A overlaps B, but it also covers the more general case where
	// they do not overlap.
	// 
	// the way we evaluate all of this is as follows: sort all chdescs
	// in the block by their max distance from the root. we will
	// iterate through this list from 'furthest from root' to 'closest
	// to root', each time deciding if we will need to rollback. note
	// that we will evaluate chdescs in the order from oldest to
	// newest here (i.e. we would evaluate B before A). then, we
	// iterate through the list from 'closest to root' to 'furthest
	// from root' and actually do the rollbacks. note that we will
	// evaluate chdescs from newer to older (i.e. we would rollback A,
	// then B as necessary). we will use the CHDESC_MARKED bit to do
	// the marking. when it is set to 1, we rollback. set to 0, we
	// don't.
	//
	// -adlr

	vector_sort(vect, compare_chdescs); // sort in ascending order
										// (i.e. from 'closest to
										// root' to 'furthest from
										// root')

	for (i = (vector_size(vect)-1); i >= 0; i--) {
		ch = vector_elt(vect, i);
		if (should_rollback(block, heap, ch)) {
			ch->flags |= CHDESC_MARKED;
		} else {
			ch->flags &= ~CHDESC_MARKED;
		}
	}

	for (i = 0; i < vector_size(vect); i++) {
		ch = vector_elt(vect, i);
		if (ch->flags & CHDESC_MARKED) {
			chdesc_rollback(ch);
			r = vector_push_back(rollback, ch);
			if (r < 0)
				panic("out of memory on vector_push_back()\n"); // isn't it ironic?
		} else {
			r = vector_push_back(satisfy, ch);
			if (r < 0)
				panic("out of memory on vector_push_back()\n");
		}
	}
	vector_destroy(vect);
}

/* this fxn is the opposite of rollback_block()
 *
 * as the chdescs were rolled back, they were inserted into the end of
 * vect. therefore, to roll then forward in the proper order, we must
 * go from the end of vect to the beginning.
 */
static void
rollforward_chdescs(vector_t *vect)
{
	while (vector_size(vect) > 0) {
		chdesc_t *ch;
		ch = vector_elt_end(vect);
		chdesc_apply(ch);
		vector_pop_back(vect);
	}
}

// tell depman that all chdescs in vect are satisfied. remve all
// chdescs from heap. XXX do i need to pick a special order among
// these chdescs? i assume 'no'
static void
satisfy_chdescs(vector_t *vect, fixed_max_heap_t *heap)
{
	while (vector_size(vect) > 0) {
		chdesc_t *ch;
		ch = vector_elt_end(vect);
		if (ch) {
			depman_remove_chdesc(ch);
			fixed_max_heap_delete(heap, ch);
		}
		vector_pop_back(vect);
	}
}

// take care of deps. this fxn will only release 'block', and no other
// blocks.
static uint32_t
wb_cache_bd_evict_block(BD_t *object, bdesc_t *block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	chdesc_t *root;
	fixed_max_heap_t *heap;
	vector_t *rollback;
	vector_t *satisfy;
	int count;
	int value;

	printf("Evicting block %d\n", block->number);
	rollback = vector_create();
	if (rollback == NULL) {
		printf("%s: out of memory on vector_create()\n",
			   __FUNCTION__);
		return -E_NO_MEM;
	}
	satisfy = vector_create();
	if (satisfy == NULL) {
		printf("%s: out of memory on vector_create()\n",
			   __FUNCTION__);
		return -E_NO_MEM;
	}

	root = (chdesc_t*)depman_get_deps(block); // shedding const
											  // intentionally b/c of
											  // CHDESC_MARKED flag.
	count = reset_chdescs(root);
	reset_marks(root);
	heap = fixed_max_heap_create(count);
	if (heap == NULL) {
		panic("%s: out of memory!!!\n", __FUNCTION__);
		return -E_NO_MEM;
	}

	number_chdescs(root, 0); // calc max distance from nodes to root
	reset_marks(root);
	heapify_nodes(root, heap);

	// go through all nodes and commit their blocks
	while (fixed_max_heap_length(heap)) {
		chdesc_t *leaf;
		bdesc_t *leafblock;
		leaf = fixed_max_heap_pop(heap);
		assert(leaf->dependencies == NULL);
		if (leaf->type == NOOP) {
			depman_remove_chdesc(leaf);
			continue;
		}
		leafblock = leaf->block;
		rollback_block(leafblock, heap, rollback, satisfy);

		// do the write
		leafblock->translated++;
		leafblock->bd = info->bd;
		value = CALL(leafblock->bd, write_block, leafblock);
		leafblock->bd = object;
		leafblock->translated--;
		
		satisfy_chdescs(satisfy, heap);
		if (vector_size(rollback) == 0) {
			// we're done w/ the leafblock!
		} else
			rollforward_chdescs(rollback);
		assert(vector_size(satisfy) == 0);
		assert(vector_size(rollback) == 0);
	}
	// all done comitting!
	fixed_max_heap_free(heap);
	vector_destroy(rollback);
	vector_destroy(satisfy);
	bdesc_release(&block);
	return 0;
}

static bdesc_t *
wb_cache_bd_read_block(BD_t * object, uint32_t number)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t index;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	index = number % info->size;
	if(info->blocks[index])
	{
		/* in the cache, use it */
		if(info->blocks[index]->number == number)
			return info->blocks[index];
		
		// evict this cache entry
		wb_cache_bd_evict_block(object, info->blocks[index]);
	}
	
	/* not in the cache, need to read it */
	info->blocks[index] = CALL(info->bd, read_block, number);
	
	if(!info->blocks[index])
		return NULL;
	
	/* FIXME bdesc_alter() and bdesc_retain() can fail */
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&info->blocks[index]);
	
	/* adjust the block descriptor to match the cache */
	info->blocks[index]->bd = object;
	
	/* increase reference count */
	bdesc_retain(&info->blocks[index]);
	
	return info->blocks[index];
}

// XXX unfinished (i think) -adlr
static int wb_cache_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t index;
	int value = 0;

	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	index = block->number % info->size;
	if (info->blocks[index]->number == block->number) {
		// overwrite existing block
		value = bdesc_overwrite(block, info->blocks[index]);
		if (value < 0)
			panic("bdesc_overwrite: %e\n", value);
	} else {
		// evict old block and write a new one
		if (info->blocks[index])
			value = wb_cache_bd_evict_block(object, info->blocks[index]);
		bdesc_retain(&block);
		info->blocks[index] = block;
	}

	return value;
}

static int wb_cache_bd_sync(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t refs;
	int value;
	
	/* since this is a write-through cache, syncing is a no-op */
	/* ...but we still have to pass the sync on correctly */
	
	if(!block)
		return CALL(info->bd, sync, NULL);
	
	/* save reference count */
	refs = block->refs;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	block->translated++;
	block->bd = info->bd;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	if(refs)
	{
		block->bd = object;
		block->translated--;
	}
	
	return value;
}

static int wb_cache_bd_destroy(BD_t * bd)
{
	struct cache_info * info = (struct cache_info *) bd->instance;
	uint32_t block;
	
	for(block = 0; block != info->size; block++)
		if(info->blocks[block])
			bdesc_release(&info->blocks[block]);
	free(info->blocks);
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks)
{
	struct cache_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	bd->instance = info;
	
	info->blocks = malloc(blocks * sizeof(*info->blocks));
	if(!info->blocks)
	{
		free(info);
		free(bd);
		return NULL;
	}
	memset(info->blocks, 0, blocks * sizeof(*info->blocks));
	
	ASSIGN(bd, wb_cache_bd, get_numblocks);
	ASSIGN(bd, wb_cache_bd, get_blocksize);
	ASSIGN(bd, wb_cache_bd, get_atomicsize);
	ASSIGN(bd, wb_cache_bd, read_block);
	ASSIGN(bd, wb_cache_bd, write_block);
	ASSIGN(bd, wb_cache_bd, sync);
	ASSIGN_DESTROY(bd, wb_cache_bd, destroy);
	
	info->bd = disk;
	info->size = blocks;
	info->blocksize = CALL(disk, get_blocksize);
	
	return bd;
}
