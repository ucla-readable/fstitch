// in this file the words 'node' and 'chdesc' are interchangable.

#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/fixed_max_heap.h>
#include <inc/vector.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/modman.h>
#include <kfs/wb_cache_bd.h>

#define WB_CACHE_DEBUG 0
#define WB_CACHE_DEBUG_TREE 0

#if WB_CACHE_DEBUG
#define printd(x...) printf(x)
#else
#define printd(x...)
#endif

#define DEBUG_BACKUP_CACHE_SIZE 20

struct cache_info {
	BD_t * bd;
	uint32_t size;
	bdesc_t ** blocks;
	uint32_t *dirty_bits;
	uint16_t blocksize;
	char *debug_cache[DEBUG_BACKUP_CACHE_SIZE];
	int debug_dirty[DEBUG_BACKUP_CACHE_SIZE];
	int debug_blkno[DEBUG_BACKUP_CACHE_SIZE];
};

unsigned short inet_chksum(void *, unsigned short);

static void
mark_dirty(BD_t * object, uint16_t bno)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int course = bno/(8*sizeof(uint32_t));
	int fine = bno % (8*sizeof(uint32_t));
	info->dirty_bits[course] |= 1<<fine;
}

static bool
is_dirty(BD_t * object, uint16_t bno)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int course = bno/(8*sizeof(uint32_t));
	int fine = bno % (8*sizeof(uint32_t));
	return (info->dirty_bits[course] & (1<<fine)) != 0;
}

static void
mark_clean(BD_t * object, uint16_t bno)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int course = bno/(8*sizeof(uint32_t));
	int fine = bno % (8*sizeof(uint32_t));
	info->dirty_bits[course] &= ~(1<<fine);
}

static unsigned short
blk_checksum(bdesc_t *blk)
{
	void *dataptr = blk->ddesc->data;
	unsigned short len = blk->length;
	return inet_chksum(dataptr, len);
}

static int
wb_cache_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct cache_info * info = (struct cache_info *) bd->instance;
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "blocksize: %d, size: %d, contention: x%d", info->blocksize, info->size, (CALL(info->bd, get_numblocks) + info->size - 1) / info->size);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d x %d", info->blocksize, info->size);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "blocksize: %d, size: %d", info->blocksize, info->size);
	}
	return 0;
}

static int
wb_cache_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

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
void
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
 * precondition: CHDESC_PRMARKED is set to 1 for each chdesc in graph.
 * postcondition: CHDESC_PRMARKED is set to 0 for each chdesc in graph.
 */
void
reset_prmarks(chdesc_t *ch)
{
	chmetadesc_t *p;
	if (!(ch->flags & CHDESC_PRMARKED)) return;
	ch->flags &= ~CHDESC_PRMARKED;
	p = ch->dependencies;
	while (p) {
		reset_prmarks(p->desc);
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

/*
 * precondition: CHDESC_PRMARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_PRMAKRED is set to 1 for each chdesc in graph.
 *
 * printf() out all the dependencies.
 */
void
print_chdescs(chdesc_t *ch, int num)
{
	chmetadesc_t *p;
	int i;

	for (i = 0; i < num; i++)
		printf("  ");
	switch (ch->type) {
	case BIT:
		//printf("bit.\n");
		//printf("0x%08x\n", ch);
		//printf("0x%08x\n", ch->distance);
		//printf("0x%08x\n", ch->block->number);
		//printf("0x%08x\n", ch->byte.length);
		printf("ch: 0x%08x BIT dist %d block %d off 0x%x",
			   ch, ch->distance,
			   ch->block->number, ch->bit.offset);
		break;
	case BYTE:
		printf("ch: 0x%08x BYTE dist %d block %d off 0x%x len %d",
			   ch, ch->distance, ch->block->number,
			   ch->byte.offset, ch->byte.length);
		break;
	case NOOP:
		printf("ch: 0x%08x NOOP dist %d", ch, ch->distance);
		break;
	default:
		printf("ch: 0x%08x UNKNOWN TYPE!! type: 0x%x", ch, ch->type);
	}

	if (ch->flags & CHDESC_PRMARKED) {
		printf(" (repeat)\n");
		return;
	} else
		printf("\n");
	ch->flags |= CHDESC_PRMARKED;

	p = ch->dependencies;
	while (p) {
		print_chdescs(p->desc, num+1);
		p = p->next;
	}
}

void
print_chdescs_gv(chdesc_t *ch, int num)
{
	chmetadesc_t *p;
	int i;

	if (num == 0) {
		printf("digraph chdescs\n{\nnodesep=0.15;\nranksep=0.15;\n"
			"node [shape=circle,color=black];\n");
	}

	if (ch->flags & CHDESC_PRMARKED) {
		return;
	}
	ch->flags |= CHDESC_PRMARKED;

	switch (ch->type) {
	case BIT:
		printf("subgraph cluster%x {\n  n%x [label=\"%d;BIT\",fillcolor=slateblue1,style=filled]\n}\n",
			   ch->block, ch, ch->distance);
		break;
	case BYTE:
		printf("subgraph cluster%x {\n  n%x [label=\"%d;BYTE\",fillcolor=slateblue1,style=filled]\n}\n",
			   ch->block, ch, ch->distance);
		break;
	case NOOP:
		printf("n%x [label=\"%d;NOOP\",fillcolor=slateblue1,style=filled]\n",
			   ch, ch->distance);
		break;
	}

	p = ch->dependencies;
	while (p) {
		print_chdescs_gv(p->desc, num+1);
		printf("\tn%x -> n%x [label=\"\"];\n", ch, p->desc);
		p = p->next;
	}
	if (num == 0) {
		printf("}\n");
	}
}

/*
 * precondition: CHDESC_MARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_MAKRED is set to 1 for all nodes from root
 * to leaves, stopping at rolled-back chdecs. also, heap contains all
 * such chdescs in graph.
 * 
 * the weight assigned to each chdesc in the graph is its distance.
 */
static void
heapify_nodes(chdesc_t *ch, fixed_max_heap_t *heap)
{
	chmetadesc_t *p;
	if (ch->flags & CHDESC_MARKED) return;
	ch->flags |= CHDESC_MARKED;
	if (ch->flags & CHDESC_ROLLBACK) return;
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
	if (!fixed_max_heap_contains(heap, ch)) return 1;

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
int wb_cache_compare_chdescs(const void *a, const void *b)
{
	chdesc_t *ca = (chdesc_t*)a;
	chdesc_t *cb = (chdesc_t*)b;
	if (ca->distance < cb->distance) return -1;
	if (ca->distance > cb->distance) return 1;
	return 0;
}

/*
 * rollback all non-rolled-back chdecs in block that meet any of these
 * criteria:
 *
 * - the chdesc is not in the heap
 * - the chdesc has out of block dependencies
 * - the chdesc points to a chdesc that will be rolled back
 *
 * rolled back block will be inserted into 'rollback' so that they may
 * be later rolled forward. the chdescs that should be satisfied will
 * be inserted into 'satisfy'. Blocks that were already rolled back
 * are inserted into neither 'rollback' nor 'satisfy'.
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
	if (root == NULL) return; // nothing to do
	// foreach chedsc ch in block:
	for (p = root->dependencies; p != NULL; p = p->next) {
		ch = p->desc;
		if (ch->flags & CHDESC_ROLLBACK) continue; // skip already
												   // rolled back
												   // chdescs
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

	vector_sort(vect, wb_cache_compare_chdescs); // sort in ascending
												 // order (i.e. from
												 // 'closest to root'
												 // to 'furthest from
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
			ch->flags &= ~CHDESC_MARKED;
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
			//printf("satisfying chdesc 0x%08x\n", ch);
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

	if (!is_dirty(object, block->number)) {
		goto end;
	}

	root = (chdesc_t*)depman_get_deps(block); // shedding const
											  // intentionally b/c of
											  // CHDESC_MARKED flag.
	if (root == NULL) {
		// do the write
		block->translated++;
		block->bd = info->bd;
		value = CALL(block->bd, write_block, block);
		block->bd = object;
		block->translated--;
		goto end;
	}

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

	count = reset_chdescs(root);
	reset_marks(root);
	heap = fixed_max_heap_create(count);
	if (heap == NULL) {
		panic("%s: out of memory!!!\n", __FUNCTION__);
		return -E_NO_MEM;
	}

	number_chdescs(root, 0); // calc max distance from nodes to root
#if WB_CACHE_DEBUG_TREE
	printf("tree for block %d:\n", block->number);
	print_chdescs_gv(root, 0);
	reset_prmarks(root);
#endif
	heapify_nodes(root, heap);
	reset_marks(root);
	//printf("heap contains %d chdescs\n", fixed_max_heap_length(heap));

	// go through all nodes and commit their blocks
	while (fixed_max_heap_length(heap)) {
		chdesc_t *leaf;
		bdesc_t *leafblock;
		leaf = fixed_max_heap_head(heap);
		if (leaf == NULL) {
			printf("\n\n\nLEAF IS NULL\n\n\n");
		}
		if (leaf == root) {
			break;
		}
		assert(leaf->dependencies == NULL);
		if (leaf->type == NOOP) {
			depman_remove_chdesc(leaf);
			continue;
		}
		//printf("popped leaf chdesc: 0x%08x\n", leaf);
		leafblock = leaf->block;
		if (leafblock == NULL)
			printf("leafblock is NULL!\n");
		//printf("rolling back\n");
		rollback_block(leafblock, heap, rollback, satisfy);
		//printf("done rolling back\n");
#if WB_CACHE_DEBUG
		if (vector_size(rollback)) {
			printf("writing block w/ %d rolledback chdecs\n",
				   vector_size(rollback));
			printf("block refs: %d\n", leafblock->refs);
		}
		printf("writing block 0x%08x\n", leafblock);
#endif

		// do the write
		//printf("satisfying chdescs\n");
		satisfy_chdescs(satisfy, heap);
		//printf("done satisfying chdescs\n");
		assert(vector_size(satisfy) == 0);

		if (leafblock->bd == object) {
			// a block in this cache. we write to the next lower bd
			leafblock->translated++;
			leafblock->bd = info->bd;
			value = CALL(leafblock->bd, write_block, leafblock);
			leafblock->bd = object;
			leafblock->translated--;
			if (vector_size(rollback) == 0) // we're done w/ the leafblock!
				mark_clean(object, leafblock->number);
		} else {
			// block in another cache. just tell that cache to write
			// it.
			value = CALL(leafblock->bd, write_block, leafblock);
		}
		if (vector_size(rollback)) {
			printf("post-write block refs: %d\n", leafblock->refs);
		}
		
		if (vector_size(rollback) > 0)
			rollforward_chdescs(rollback);
		assert(vector_size(rollback) == 0);
	}
	// all done comitting!
	fixed_max_heap_free(heap);
	vector_destroy(rollback);
	vector_destroy(satisfy);
 end:
	mark_clean(object, block->number);
	root = (chdesc_t*)depman_get_deps(block); // shedding const
											  // intentionally b/c of
											  // CHDESC_MARKED flag.
	if (root != NULL) {
		printf("problem tree?\n");
		print_chdescs_gv(root, 0);
		reset_prmarks(root);
	}
	bdesc_release(&block);
	return 0;
}

static void
dump_data(char *data, int len)
{
	int i, j;
	return;
	printf("data dump:\n");
	for (i = 0; i < len; i+=32) {
		printf("\t");
		for (j = 0; j < 32; j+=4)
			printf(" %08x", data[i + j]);
		printf("\n");
	}
	printf("data dump done\n");
}

static void
backup_insert(BD_t *object, bdesc_t *blk)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int i;
	for (i = 0; i < DEBUG_BACKUP_CACHE_SIZE; i++) {
		if (info->debug_blkno[i] == blk->number ||
			info->debug_blkno[i] == -1) {
			info->debug_blkno[i] = blk->number;
			memcpy(info->debug_cache[i], blk->ddesc->data, blk->length);
			return;
		}
	}
	//printf("BACKUP CACHE IS FULL!\n\n\n\n");
}

// return non zero if problem. 0 if no problem
static bool
backup_check(BD_t *object, bdesc_t *blk)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	int i;
	int ret;
	for (i = 0; i < DEBUG_BACKUP_CACHE_SIZE; i++) {
		if (info->debug_blkno[i] == blk->number) {
			ret = memcmp(info->debug_cache[i], blk->ddesc->data, blk->length);
			if (ret != 0) {
				printf("(actual checksum 0x%04x, should be 0x%04x)\n",
					   blk_checksum(blk),
					   inet_chksum(info->debug_cache[i],
								   blk->length));
				printf("you're asking:\n");
				dump_data(blk->ddesc->data, blk->length);
				printf("you should have:\n");
				dump_data(info->debug_cache[i], blk->length);
			}
			return ret;
		}
	}
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
		if(info->blocks[index]->number == number) {
			return info->blocks[index];
		}
		
		// evict this cache entry
		wb_cache_bd_evict_block(object, info->blocks[index]);
	} else {

	}
	/* not in the cache, need to read it */
	info->blocks[index] = CALL(info->bd, read_block, number);
	
	if(!info->blocks[index]) {
		printf("failed to read block number %d from lower level!\n",
			   number);
		return NULL;
	}
	
	/* FIXME bdesc_alter() and bdesc_retain() can fail */
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&info->blocks[index]);
	
	/* adjust the block descriptor to match the cache */
	info->blocks[index]->bd = object;

	/*
	if (backup_check(object, info->blocks[index]) != 0) {
		panic("read block %d failed!!\n", info->blocks[index]->number);
	}
	*/
	
	/* increase reference count */
	bdesc_retain(&info->blocks[index]);
	
	return info->blocks[index];
}

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
	
	//backup_insert(object, block);
	//printf("got a write for block %d\n", block->number);
	
	index = block->number % info->size;
	if (info->blocks[index]->number == block->number) {
		// overwrite existing block
		value = bdesc_overwrite(info->blocks[index], block);
		if (value < 0)
			panic("bdesc_overwrite: %e\n", value);
		bdesc_drop(&block);
	} else {
		// evict old block and write a new one
		printd("cache conflict or miss. maybe evicting.\n");
		if (info->blocks[index])
			value = wb_cache_bd_evict_block(object, info->blocks[index]);
		bdesc_retain(&block);
		info->blocks[index] = block;
	}
	mark_dirty(object, info->blocks[index]->number);

	return value;
}

static int wb_cache_bd_sync(BD_t * object, bdesc_t * block)
{
	struct cache_info * info = (struct cache_info *) object->instance;
	uint32_t refs;
	int value;
	
	/* since this is a write-through cache, syncing is a no-op */
	/* ...but we still have to pass the sync on correctly */
	
	printf("sync not supported yet. sorry.\n");
	return CALL(info->bd, sync, NULL);
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
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	/* FIXME a write-back cache should probably sync these blocks in case they are dirty */
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
	int i;
	
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
	info->dirty_bits = malloc(blocks / (8*sizeof(uint32_t)) + 1);
	if (info->dirty_bits == NULL) {
		free(info->blocks);
		free(info);
		free(bd);
		return NULL;
	}
	for (i = 0; i < blocks / (8*sizeof(uint32_t)) + 1; i++)
		info->dirty_bits[i] = 0;

	info->blocksize = CALL(disk, get_blocksize);
	if (info->debug_cache == 0) {
		return NULL;
	}
	for (i = 0; i < DEBUG_BACKUP_CACHE_SIZE; i++) {
		info->debug_cache[i] = malloc(info->blocksize);
		if (info->debug_cache[i] == 0)
			return NULL;
		info->debug_dirty[i] = 0;
		info->debug_blkno[i] = -1;
	}

	memset(info->blocks, 0, blocks * sizeof(*info->blocks));
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, wb_cache_bd, get_config);
	OBJASSIGN(bd, wb_cache_bd, get_status);
	ASSIGN(bd, wb_cache_bd, get_numblocks);
	ASSIGN(bd, wb_cache_bd, get_blocksize);
	ASSIGN(bd, wb_cache_bd, get_atomicsize);
	ASSIGN(bd, wb_cache_bd, read_block);
	ASSIGN(bd, wb_cache_bd, write_block);
	ASSIGN(bd, wb_cache_bd, sync);
	DESTRUCTOR(bd, wb_cache_bd, destroy);
	
	info->bd = disk;
	info->size = blocks;
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	if(modman_inc_bd(disk, bd, NULL) < 0)
	{
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
