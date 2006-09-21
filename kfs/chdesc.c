#include <inc/error.h>
#include <lib/assert.h>
#include <lib/kdprintf.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>
#include <lib/memdup.h>
#include <lib/panic.h>

#include <kfs/debug.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

/* Change descriptor multigraphs allow more than one dependency between the same
 * two change descriptors. This currently saves us the trouble of making sure we
 * don't create a duplicate dependency between chdescs, though it also causes us
 * to allocate somewhat more memory in many cases where we would otherwise
 * detect the duplicate dependency. Allowing multigraphs results in a reasonable
 * speedup, even though we use more memory, so it is enabled by default. */
#define CHDESC_ALLOW_MULTIGRAPH 1

/* Set to make nonrollbackable chdescs always cover the entire block */
/* TODO: should we or should we not do this? Pluses and minuses:
 * + More chdecs can be merged
 *   (though quick testing shows only a small increase)
 * + Should allow for faster new chdesc merging
 * + A higher percentage of rollbackables explicitly depend on nonrollbackables
 *   (but still not all, eg a nonrollbackable created after a rollbackable)
 * - Nonrollbackables claim to modify data that they may not
 *   - barrier traversal implications?
 *   - a synthetic block with a 1B nrb chdesc will appear to be inited
 * - More dependencies will exist */
#define CHDESC_NRB_WHOLEBLOCK 1

/* Set to allow new chdescs to be merged into existing chdescs */
#define CHDESC_MERGE_NEW 1
/* Set to track new chdesc merge stats and print them after shutdown */
#define CHDESC_MERGE_NEW_STATS 1

#if CHDESC_MERGE_NEW_STATS && !CHDESC_MERGE_NEW
# error CHDESC_MERGE_NEW_STATS requires CHDESC_MERGE_NEW
#endif

static chdesc_t * free_head = NULL;

static void chdesc_free_push(chdesc_t * chdesc)
{
	assert(free_head != chdesc && !chdesc->free_prev);
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc, free_head);
	chdesc->free_next = free_head;
	if(free_head)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, free_head, chdesc);
		free_head->free_prev = chdesc;
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_HEAD, chdesc);
	free_head = chdesc;
}

static void chdesc_free_remove(chdesc_t * chdesc)
{
	assert(chdesc->free_prev || free_head == chdesc);
	if(chdesc->free_prev)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc->free_prev, chdesc->free_next);
		chdesc->free_prev->free_next = chdesc->free_next;
	}
	else
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_HEAD, chdesc->free_next);
		free_head = chdesc->free_next;
	}
	if(chdesc->free_next)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, chdesc->free_next, chdesc->free_prev);
		chdesc->free_next->free_prev = chdesc->free_prev;
	}
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_PREV, chdesc, NULL);
	chdesc->free_prev = NULL;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FREE_NEXT, chdesc, NULL);
	chdesc->free_next = NULL;
}

/* ensure bdesc->ddesc->overlaps has a noop chdesc */
static int ensure_bdesc_has_overlaps(bdesc_t * block)
{
	chdesc_t * chdesc;
	assert(block);
	
	if(block->ddesc->overlaps)
	{
		assert(block->ddesc->overlaps->type == NOOP);
		return 0;
	}
	
	chdesc = chdesc_create_noop(NULL, NULL);
	if(!chdesc)
		return -E_NO_MEM;
	
	if(chdesc_weak_retain(chdesc, &block->ddesc->overlaps) < 0)
	{
		chdesc_destroy(&chdesc);
		return -E_NO_MEM;
	}
	
	return 0;
}

/* ensure bdesc->ddesc->bit_changes[offset] has a noop chdesc */
static chdesc_t * ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset)
{
	chdesc_t * chdesc;
	hash_map_elt_t * elt;
	void * key = (void *) (uint32_t) offset;
	assert(block);
	
	chdesc = (chdesc_t *) hash_map_find_val(block->ddesc->bit_changes, key);
	if(chdesc)
	{
		assert(chdesc->type == NOOP);
		return chdesc;
	}
	
	chdesc = chdesc_create_noop(NULL, NULL);
	if(!chdesc)
		return NULL;
	
	if(hash_map_insert(block->ddesc->bit_changes, key, chdesc) < 0)
	{
		chdesc_destroy(&chdesc);
		return NULL;
	}
	elt = hash_map_find_eltp(block->ddesc->bit_changes, key);
	assert(elt);
	
	/* we don't really need a flag for this, since we could just use the
	 * noop.bit_changes field to figure it out... but that would be error-prone */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, chdesc, CHDESC_BIT_NOOP);
	chdesc->flags |= CHDESC_BIT_NOOP;
	chdesc->noop.bit_changes = block->ddesc->bit_changes;
	chdesc->noop.hash_key = key;
	
	if(chdesc_weak_retain(chdesc, (chdesc_t **) &elt->val) < 0)
	{
		hash_map_erase(block->ddesc->bit_changes, key);
		chdesc_destroy(&chdesc);
		return NULL;
	}
	
	return chdesc;
}

/* get bdesc->ddesc->bit_changes[offset] */
static chdesc_t * chdesc_bit_changes(bdesc_t * block, uint16_t offset)
{
	return hash_map_find_val(block->ddesc->bit_changes, (void *) (uint32_t) offset);
}


#include <lib/string.h>
/* TODO: should we (and how do we want to) optimize this in the kernel? */
static void * srealloc(void * p, size_t p_size, size_t new_size)
{
	void * q = smalloc(new_size);
	if(!q)
		return NULL;
	if(p)
		memcpy(q, p, p_size);
	sfree(p, p_size);
	return q;
}

#define STATIC_STATES_CAPACITY 1024 /* 1024 is fairly arbitrary */

static void propagate_noop_level_change(chdesc_t * noop_dependent, uint16_t prev_level, uint16_t new_level)
{
	/* recursion-on-the-heap support */
	struct state {
		chmetadesc_t * noops_dependents;
		uint16_t prev_level;
		uint16_t new_level;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;

  recurse_start:
	assert(!noop_dependent->owner);
	assert(prev_level != new_level);
	assert(prev_level != BDLEVEL_NONE || new_level != BDLEVEL_NONE);

	chmetadesc_t * noops_dependents = noop_dependent->dependents;
	for(; noops_dependents; noops_dependents = noops_dependents->dependent.next)
	{
		chdesc_t * c = noops_dependents->dependent.desc;
		uint16_t c_prev_level = chdesc_level(c);

		if(prev_level != BDLEVEL_NONE)
		{
			assert(c->befores[prev_level]);
			c->befores[prev_level]--;
		}
		if(new_level != BDLEVEL_NONE)
		{
			c->befores[new_level]++;
			assert(c->befores[new_level]);
		}
		chdesc_update_ready_changes(c);

		if(!c->owner)
		{
			uint16_t c_new_level = chdesc_level(c);
			if(c_prev_level != c_new_level)
			{
				/* Recursively propagate the level change; equivalent to
				 * propagate_noop_level_change(c, c_prev_level, c_new_level).
				 * We don't recursively call this function because we can
				 * overflow the stack. We instead use the 'states' array
				 * to hold this function's recursive state. */
				size_t next_index = 1 + state - &states[0];

				state->noops_dependents = noops_dependents;
				state->prev_level = prev_level;
				state->new_level = new_level;

				noop_dependent = c;
				prev_level = c_prev_level;
				new_level = c_new_level;
				
				if(next_index < states_capacity)
					state++;
				else
				{
					size_t cur_size = states_capacity * sizeof(*state);
					states_capacity *= 2;
					if(states == static_states)
					{
						states = smalloc(states_capacity * sizeof(*state));
						if(states)
							memcpy(states, static_states, cur_size);
					}
					else
						states = srealloc(states, cur_size, states_capacity * sizeof(*state));
					if(!states)
						panic("smalloc/srealloc(%u bytes) failed", states_capacity * sizeof(*state));
					state = &states[next_index];
				}
				goto recurse_start;

			  recurse_resume:
				(void) 0; /* placate compiler re deprecated end labels */
			}
		}
	}

	if(state != &states[0])
	{
		state--;
		noops_dependents = state->noops_dependents;
		prev_level = state->prev_level;
		new_level = state->new_level;
		goto recurse_resume;
	}

	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
}

/* propagate the new dependency's level info for 'dependent' on 'dependency' */
static void propagate_dependency(chdesc_t * dependent, const chdesc_t * dependency)
{
	uint16_t dependency_level = chdesc_level(dependency);
	uint16_t dependent_prev_level;

	if(dependency_level == BDLEVEL_NONE)
		return;
	dependent_prev_level = chdesc_level(dependent);

	dependent->befores[dependency_level]++;
	assert(dependent->befores[dependency_level]);
	chdesc_update_ready_changes(dependent);
	if(!dependent->owner)
	{
		if(dependency_level > dependent_prev_level || dependent_prev_level == BDLEVEL_NONE)
			propagate_noop_level_change(dependent, dependent_prev_level, dependency_level);
	}
}

/* unpropagate the dependency's level info for 'dependent' on 'dependency' */
static void unpropagate_dependency(chdesc_t * dependent, const chdesc_t * dependency)
{
	uint16_t dependency_level = chdesc_level(dependency);
	uint16_t dependent_prev_level;

	if(dependency_level == BDLEVEL_NONE)
		return;
	dependent_prev_level = chdesc_level(dependent);

	assert(dependent->befores[dependency_level]);
	dependent->befores[dependency_level]--;
	chdesc_update_ready_changes(dependent);
	if(!dependent->owner)
	{
		if(dependency_level == dependent_prev_level && !dependent->befores[dependency_level])
			propagate_noop_level_change(dependent, dependent_prev_level, chdesc_level(dependent));
	}
}

void chdesc_propagate_level_change(chmetadesc_t * dependents, uint16_t prev_level, uint16_t new_level)
{
	assert(prev_level < NBDLEVEL || prev_level == BDLEVEL_NONE);
	assert(new_level < NBDLEVEL || new_level == BDLEVEL_NONE);
	assert(prev_level != new_level);
	for(; dependents; dependents = dependents->dependent.next)
	{
		chdesc_t * c = dependents->dependent.desc;
		uint16_t c_prev_level = chdesc_level(c);

		if(prev_level != BDLEVEL_NONE)
		{
			assert(c->befores[prev_level]);
			c->befores[prev_level]--;
		}
		if(new_level != BDLEVEL_NONE)
		{
			c->befores[new_level]++;
			assert(c->befores[new_level]);
		}
		chdesc_update_ready_changes(c);

		if(!c->owner)
		{
			uint16_t c_new_level = chdesc_level(c);
			if(c_prev_level != c_new_level)
				propagate_noop_level_change(c, c_prev_level, c_new_level);
		}
	}
}

/* add a dependency to a change descriptor without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
#if !CHDESC_ALLOW_MULTIGRAPH
	/* make sure it's not already there */
	for(meta = dependent->dependencies; meta; meta = meta->dependency.next)
		if(meta->desc == dependency)
			return 0;
	/* shouldn't be there */
	for(meta = dependency->dependents; meta; meta = meta->dependent.next)
		assert(meta->desc != dependent);
#endif
	meta = malloc(sizeof(*meta));
	if(!meta)
		return -E_NO_MEM;
	
	propagate_dependency(dependent, dependency);
	
	/* add the dependency to the dependent */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENCY, dependent, dependency);
	meta->dependency.desc = dependency;
	meta->dependency.next = NULL;
	meta->dependency.ptr = dependent->dependencies_tail;
	*dependent->dependencies_tail = meta;
	dependent->dependencies_tail = &meta->dependency.next;
	
	/* add the dependent to the dependency */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_DEPENDENT, dependency, dependent);
	meta->dependent.desc = dependent;
	meta->dependent.next = NULL;
	meta->dependent.ptr = dependency->dependents_tail;
	*dependency->dependents_tail = meta;
	dependency->dependents_tail = &meta->dependent.next;
	
	/* virgin NOOP chdesc getting its first dependency */
	if(free_head == dependent || dependent->free_prev)
	{
		assert(dependent->type == NOOP);
		assert(!(dependent->flags & CHDESC_WRITTEN));
		chdesc_free_remove(dependent);
	}
	
	return 0;
}

/* note that we don't check to see if these chdescs are for the same ddesc or not */
/* returns 0 for no overlap, 1 for overlap, and 2 for a overlaps b completely */
int chdesc_overlap_check(chdesc_t * a, chdesc_t * b)
{
	uint16_t a_start, a_len;
	uint16_t b_start, b_len;
	uint32_t start, end, tag;
	
	/* if either is a NOOP chdesc, they don't overlap */
	if(a->type == NOOP || b->type == NOOP)
		return 0;
	
	/* two bit chdescs overlap if they modify the same bits */
	if(a->type == BIT && b->type == BIT)
	{
		uint32_t shared;
		if(a->bit.offset != b->bit.offset)
			return 0;
		shared = a->bit.xor & b->bit.xor;
		if(!shared)
			return 0;
		/* check for complete overlap */
		return (shared == b->bit.offset) ? 2 : 1;
	}
	
	if(a->type == BIT)
	{
		a_len = sizeof(a->bit.xor);
		a_start = a->bit.offset * a_len;
	}
	else
	{
		a_len = a->byte.length;
		a_start = a->byte.offset;
	}
	if(b->type == BIT)
	{
		b_len = sizeof(b->bit.xor);
		b_start = b->bit.offset * b_len;
	}
	else
	{
		b_len = b->byte.length;
		b_start = b->byte.offset;
	}
	
	start = b_start;
	end = start + b_len + a_len;
	tag = a_start + a_len;
	if(tag <= start || end <= tag)
		return 0;
	return (a_start <= b_start && start + b_len <= tag) ? 2 : 1;
}

#if !CHDESC_NRB_WHOLEBLOCK
/* note that we don't check to see if these chdescs are for the same ddesc or not */
/* returns 1 if a and b change contiguous bytes, 0 if they do not */
static bool chdesc_byte_contiguous_check(chdesc_t * a, chdesc_t * b)
{
	uint16_t a_start, a_len;
	uint16_t b_start, b_len;
	
	/* if either is a NOOP chdesc, they don't overlap */
	if(a->type == NOOP || b->type == NOOP)
		return 0;

	/* let's say two bit chdescs are byte contiguous if they modify
	 * contiguous words, because it is easy to check */
	if(a->type == BIT && b->type == BIT)
	{
		int32_t offset_diff = a->bit.offset - b->bit.offset;
		if(offset_diff == -1 || offset_diff == 0 || offset_diff == 1)
			return 1;
		return 0;
	}

	if(a->type == BIT)
	{
		a_len = sizeof(a->bit.xor);
		a_start = a->bit.offset * a_len;
	}
	else
	{
		a_len = a->byte.length;
		a_start = a->byte.offset;
	}
	if(b->type == BIT)
	{
		b_len = sizeof(b->bit.xor);
		b_start = b->bit.offset * b_len;
	}
	else
	{
		b_len = b->byte.length;
		b_start = b->byte.offset;
	}

	if(a_start + a_len < b_start || b_start + b_len < a_start)
		return 0;
	return 1;
}
#endif

/* make the recent chdesc depend on the given earlier chdesc in the same block if it overlaps */
static int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original)
{
	int r, overlap;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_ATTACH, recent, original);
	
	/* if either is a NOOP chdesc, warn about it */
	if(recent->type == NOOP || original->type == NOOP)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Unexpected NOOP chdesc\n", __FUNCTION__, __FILE__, __LINE__);
	
	/* if they don't overlap, we are done */
	overlap = chdesc_overlap_check(recent, original);
	if(!overlap)
		return 0;
	
	if(original->flags & CHDESC_ROLLBACK)
	{
		/* it's not clear what to do in this case... just fail with a warning for now */
		kdprintf(STDERR_FILENO, "Attempt to overlap a new chdesc with a rolled-back chdesc! (debug = %d)\n", KFS_DEBUG_COUNT());
		return -E_BUSY;
	}
	
	r = chdesc_add_depend(recent, original);
	if(r < 0)
		return r;
	
	/* if it overlaps completely, remove original from ddesc->overlaps or ddesc->bit_changes */
	if(overlap == 2)
	{
		if(original->type == BYTE)
			chdesc_remove_depend(original->block->ddesc->overlaps, original);
		else if(original->type == BIT)
		{
			chdesc_t * bit_changes = chdesc_bit_changes(original->block, original->bit.offset);
			assert(bit_changes);
			chdesc_remove_depend(bit_changes, original);
		}
		else
			kdprintf(STDERR_FILENO, "Complete overlap of unhandled chdesc type!\n");
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, recent, CHDESC_OVERLAP);
		recent->flags |= CHDESC_OVERLAP;
	}
	
	return 0;
}

static int _chdesc_overlap_multiattach(chdesc_t * chdesc, chdesc_t * list_chdesc)
{
	chmetadesc_t * list = list_chdesc->dependencies;
	chmetadesc_t * next = list;
	while((list = next))
	{
		int r;
		
		/* this loop is tricky, because we might remove the item we're
		 * looking at currently if we overlap it entirely - so we
		 * prefetch the next pointer at the top of the loop */
		next = list->dependency.next;
		
		list_chdesc = list->dependency.desc;
		
		/* skip moved chdescs - they have just been added to this block
		 * by chdesc_move() and already have proper overlap dependency
		 * information with respect to the chdesc now arriving */
		if(list_chdesc->flags & CHDESC_MOVED || list_chdesc == chdesc)
			continue;
		
		r = chdesc_overlap_attach(chdesc, list_chdesc);
		if(r < 0)
			return r;
	}
	return 0;
}

static int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_OVERLAP_MULTIATTACH, chdesc, block);
	
	if(chdesc->type == BIT)
	{
		chdesc_t * bit_changes = chdesc_bit_changes(block, chdesc->bit.offset);
		if(bit_changes)
		{
			int r = _chdesc_overlap_multiattach(chdesc, bit_changes);
			if(r < 0)
				return r;
		}
	}
	
	if(!block->ddesc->overlaps)
		return 0;
	
	return _chdesc_overlap_multiattach(chdesc, block->ddesc->overlaps);
}

void __propagate_dependency(chdesc_t * dependent, const chdesc_t * dependency)
#if defined(__MACH__)
{
	return propagate_dependency(dependent, dependency);
}
#else
	__attribute__ ((alias("propagate_dependency")));
#endif

void __unpropagate_dependency(chdesc_t * dependent, const chdesc_t * dependency)
#if defined(__MACH__)
{
	return unpropagate_dependency(dependent, dependency);
}
#else
	__attribute__ ((alias("unpropagate_dependency")));
#endif

int __ensure_bdesc_has_overlaps(bdesc_t * block)
#if defined(__MACH__)
{
	return ensure_bdesc_has_overlaps(block);
}
#else
	__attribute__ ((alias("ensure_bdesc_has_overlaps")));
#endif

chdesc_t * __ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset)
#if defined(__MACH__)
{
	return ensure_bdesc_has_bit_changes(block, offset);
}
#else
	__attribute__ ((alias("ensure_bdesc_has_bit_changes")));
#endif

chdesc_t * __chdesc_bit_changes(bdesc_t * block, uint16_t offset)
#if defined(__MACH__)
{
	return chdesc_bit_changes(block, offset);
}
#else
	__attribute__ ((alias("chdesc_bit_changes")));
#endif

int __chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
#if defined(__MACH__)
{
	return chdesc_add_depend_fast(dependent, dependency);
}
#else
	__attribute__((alias("chdesc_add_depend_fast")));
#endif

int __chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block)
#if defined(__MACH__)
{
	return chdesc_overlap_multiattach(chdesc, block);
}
#else
	__attribute__((alias("chdesc_overlap_multiattach")));
#endif

void chdesc_link_all_changes(chdesc_t * chdesc)
{
	assert(!chdesc->ddesc_next && !chdesc->ddesc_pprev);
	if(chdesc->block)
	{
		datadesc_t * ddesc = chdesc->block->ddesc;
		chdesc->ddesc_pprev = &ddesc->all_changes;
		chdesc->ddesc_next = ddesc->all_changes;
		ddesc->all_changes = chdesc;
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = &chdesc->ddesc_next;
		else
			ddesc->all_changes_tail = &chdesc->ddesc_next;
	}
}

void chdesc_unlink_all_changes(chdesc_t * chdesc)
{
	if(chdesc->ddesc_pprev)
	{
		datadesc_t * ddesc = chdesc->block->ddesc;
		// remove from old ddesc changes list
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = chdesc->ddesc_pprev;
		else
			ddesc->all_changes_tail = chdesc->ddesc_pprev;
		*chdesc->ddesc_pprev = chdesc->ddesc_next;
		chdesc->ddesc_next = NULL;
		chdesc->ddesc_pprev = NULL;
	}
	else
		assert(!chdesc->ddesc_next && !chdesc->ddesc_pprev);
}

void chdesc_link_ready_changes(chdesc_t * chdesc)
{
	assert(!chdesc->ddesc_ready_next && !chdesc->ddesc_ready_pprev);
	if(chdesc->block)
	{
		datadesc_t * ddesc = chdesc->block->ddesc;
		chdesc_dlist_t * rcl = &ddesc->ready_changes[chdesc->owner->level];
		chdesc->ddesc_ready_pprev = &rcl->head;
		chdesc->ddesc_ready_next = rcl->head;
		rcl->head = chdesc;
		if(chdesc->ddesc_ready_next)
			chdesc->ddesc_ready_next->ddesc_ready_pprev = &chdesc->ddesc_ready_next;
		else
			rcl->tail = &chdesc->ddesc_ready_next;
	}
}

void chdesc_unlink_ready_changes(chdesc_t * chdesc)
{
	if(chdesc->ddesc_ready_pprev)
	{
		datadesc_t * ddesc = chdesc->block->ddesc;
		chdesc_dlist_t * rcl = &ddesc->ready_changes[chdesc->owner->level];
		// remove from old ddesc changes list
		if(chdesc->ddesc_ready_next)
			chdesc->ddesc_ready_next->ddesc_ready_pprev = chdesc->ddesc_ready_pprev;
		else
			rcl->tail = chdesc->ddesc_ready_pprev;
		*chdesc->ddesc_ready_pprev = chdesc->ddesc_ready_next;
		chdesc->ddesc_ready_next = NULL;
		chdesc->ddesc_ready_pprev = NULL;
	}
	else
		assert(!chdesc->ddesc_ready_next && !chdesc->ddesc_ready_pprev);
}

/* return whether chdesc is ready to go down one level */
static __inline bool chdesc_is_ready(const chdesc_t * chdesc) __attribute__((always_inline));
static __inline bool chdesc_is_ready(const chdesc_t * chdesc)
{
	/* empty noops are not on blocks and so cannot be on a ready list */
	if(!chdesc->owner)
		return 0;
	uint16_t before_level = chdesc_before_level(chdesc);
	return before_level < chdesc->owner->level || before_level == BDLEVEL_NONE;
}

void chdesc_update_ready_changes(chdesc_t * chdesc)
{
	bool is_ready = chdesc_is_ready(chdesc);
	bool is_in_ready_list = chdesc->ddesc_ready_pprev != NULL;
	if(is_in_ready_list)
	{
		if(!is_ready)
			chdesc_unlink_ready_changes(chdesc);
	}
	else
	{
		if(is_ready)
			chdesc_link_ready_changes(chdesc);
	}
}

void chdesc_tmpize_all_changes(chdesc_t * chdesc)
{
	assert(!chdesc->tmp_next && !chdesc->tmp_pprev);

	if(chdesc->ddesc_pprev)
	{
		chdesc->tmp_next = chdesc->ddesc_next;
		chdesc->tmp_pprev = chdesc->ddesc_pprev;
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = chdesc->ddesc_pprev;
		else
			chdesc->block->ddesc->all_changes_tail = chdesc->ddesc_pprev;
		*chdesc->ddesc_pprev = chdesc->ddesc_next;

		chdesc->ddesc_next = NULL;
		chdesc->ddesc_pprev = NULL;
	}
	else
		assert(!chdesc->ddesc_next);
}

void chdesc_untmpize_all_changes(chdesc_t * chdesc)
{
	assert(!chdesc->ddesc_next && !chdesc->ddesc_pprev);

	if(chdesc->tmp_pprev)
	{
		chdesc->ddesc_next = chdesc->tmp_next;
		chdesc->ddesc_pprev = chdesc->tmp_pprev;
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = &chdesc->ddesc_next;
		else
			chdesc->block->ddesc->all_changes_tail = &chdesc->ddesc_next;
		*chdesc->ddesc_pprev = chdesc;

		chdesc->tmp_next = NULL;
		chdesc->tmp_pprev = NULL;
	}
	else
		assert(!chdesc->tmp_next);
}

chdesc_t * chdesc_create_noop(bdesc_t * block, BD_t * owner)
{
	chdesc_t * chdesc;
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_NOOP, chdesc, block, owner);
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependencies_tail = &chdesc->dependencies;
	chdesc->dependents = NULL;
	chdesc->dependents_tail = &chdesc->dependents;
	chdesc->weak_refs = NULL;
	memset(chdesc->befores, 0, sizeof(chdesc->befores));
	chdesc->free_prev = NULL;
	chdesc->free_next = NULL;
	chdesc->ddesc_next = NULL;
	chdesc->ddesc_pprev = NULL;
	chdesc->ddesc_ready_next = NULL;
	chdesc->ddesc_ready_pprev = NULL;
	chdesc->tmp_next = NULL;
	chdesc->tmp_pprev = NULL;
	chdesc->stamps = 0;
	
	/* NOOP chdescs start applied */
	chdesc->flags = 0;
	
	if(block)
	{
		/* add chdesc to block's dependencies */
		chdesc_link_all_changes(chdesc);
		chdesc_link_ready_changes(chdesc);
		
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	chdesc_free_push(chdesc);
	
	return chdesc;
}

#if CHDESC_DATA_OMITTANCE
static bool chdesc_has_external_dependents(const chdesc_t * chdesc, const bdesc_t * block)
{
	const chmetadesc_t * dependents;
	for(dependents = chdesc->dependents; dependents; dependents = dependents->dependent.next)
	{
		const chdesc_t * dependent = dependents->dependent.desc;
		if(dependent->type == NOOP)
		{
			if(dependent->block && dependent->block->number != block->number)
				return 1;
			/* XXX: stack usage */
			if(chdesc_has_external_dependents(dependent, block))
				return 1;
		}
		else if(dependent->block->number != block->number)
			return 1;
	}
	return 0;
}

static bool bdesc_has_external_dependents(const bdesc_t * block)
{
	const chdesc_t * c;
	for(c = block->ddesc->all_changes; c; c = c->ddesc_next)
		if(chdesc_has_external_dependents(c, block))
			return 1;
	return 0;
}
#endif

static bool new_chdescs_require_data(const bdesc_t * block)
{
#if CHDESC_DATA_OMITTANCE
	/* Rule: When adding chdesc C to block B,
	 * and forall C' on B, with C' != C: C' has no dependents on blocks != B,
	 * then C will never need to be rolled back. */
	return bdesc_has_external_dependents(block);
#else
	return 1;
#endif
}

#include <time.h>

/* Check whether a chdesc merge that adds a dependency on 'chdesc' to an
 * existing chdesc on 'block' could lead to an indirect dependency cycle.
 * Returns 0 if a cycle is not possible, <0 if a cycle is possible.
 * Precondition: 0 == bdesc_has_external_dependents(block). */
static int merge_indirect_cycle_is_possible(const chdesc_t * chdesc, const bdesc_t * block)
{
#if 0
	const chmetadesc_t * meta;
	for(meta = chdesc->dependencies; meta; meta = meta->dependency.next)
	{
		chdesc_t * dependency = meta->dependency.desc;
		int r;
		
		/* It is a precondition that dependencies on other blocks cannot
		 * induce cycles. */
		if(dependency->block && dependency->block->ddesc != block->ddesc)
			continue;
		
#if 1
		/* A rollbackable on 'block' that is a dependency could already
		 * depend on the existing chdesc that is merged into. (Cycle!)
		 * Dependencies on rollbakcables on 'block' rarely occur in practice,
		 * so conservatively give up on them to make detection simple.
		 * NOTE: this check could instead scan block->ddesc->all_changes */
		if(dependency->block && dependency->block->ddesc == block->ddesc && chdesc_is_rollbackable(dependency))
			return -1;
#endif
		
		/* A NOOP could now, or later be made to, depend on a chdesc on block.
		 * Conservatively say possible cycle for all NOOP dependencies
		 * unless the NOOP is reachable only through chdescs on other blocks.
		 * This check could be more lenient, but NOOPs can have complicated
		 * relations and this check gives a low enough false negative rate. */
		if(dependency->type == NOOP)
			return -2;
		
		/* Check indirect dependencies for induced cycles */
		/* XXX: stack usage */
		if((r = merge_indirect_cycle_is_possible(dependency, block)) < 0)
			return r;
	}
	return 0;
#else
	struct state {
		const chmetadesc_t * meta;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;

	const chmetadesc_t * meta = chdesc->dependencies;
	int r = 0;
  recurse_start:
	for(; meta; meta = meta->dependency.next)
	{
		const chdesc_t * dependency = meta->dependency.desc;
		
		if(dependency->block && dependency->block->ddesc != block->ddesc)
			continue;
		
		if(dependency->block && dependency->block->ddesc == block->ddesc && chdesc_is_rollbackable(dependency))
		{
			r = -1;
			goto exit;
		}
		if(dependency->type == NOOP)
		{
			r = -2;
			goto exit;
		}
		
		/* Check indirect dependencies for induced cycles
		 * Equivalent to:
		 * if((r = merge_indirect_cycle_is_possible(dependency, block)) < 0)
		 *	return r;
		 */
		size_t next_index = 1 + state - &states[0];
		
		state->meta = meta;
		
		meta = dependency->dependencies;
		
		if(next_index < states_capacity)
			state++;
		else
		{
			size_t cur_size = states_capacity * sizeof(*state);
			states_capacity *= 2;
			if(states == static_states)
			{
				states = smalloc(states_capacity * sizeof(*state));
				if(states)
					memcpy(states, static_states, cur_size);
			}
			else
				states = srealloc(states, cur_size, states_capacity * sizeof(*state));
			if(!states)
				panic("smalloc/srealloc(%u bytes) failed", states_capacity * sizeof(*state));
			state = &states[next_index];
			printf("%s: increase to %u at %lu s\n", __FUNCTION__, states_capacity, time(NULL));
		}
		goto recurse_start;
		
	  recurse_resume:
		(void) 0; /* placate compiler re deprecated end labels */
	}
	
	if(state != &states[0])
	{
		state--;
		meta = state->meta;
		goto recurse_resume;
	}
	
  exit:
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
	
	return r;
#endif
}

/* Check whether a chdesc merge that adds a dependency on 'dependency' to an
 * existing chdesc on 'block' could lead to a dependency cycle.
 * Returns 0 if a cycle is not possible, <0 if a cycle is possible.
 * Precondition: 0 == bdesc_has_external_dependents(block). */
/* TODO: unify this and merge_indirect_cycle_is_possible(). They are the same
 * except that merge_indirect_cycle_is_possible() does not check
 * for direct cycles. */
static int merge_cycle_is_possible(const chdesc_t * dependency, const bdesc_t * block)
{
	/* It is a precondition that dependencies on other blocks cannot
	 * induce cycles. */
	if(dependency->block && dependency->block->ddesc != block->ddesc)
		return 0;

#if 1
	/* A rollbackable on 'block' that is a dependency could already
	 * depend on the existing chdesc that is merged into. (Cycle!)
	 * Dependencies on rollbakcables on 'block' rarely occur in practice,
	 * so conservatively give up on them to make detection simple.
	 * NOTE: this check could instead scan block->ddesc->all_changes */
	if(dependency->block && dependency->block->ddesc == block->ddesc && chdesc_is_rollbackable(dependency))
		return -1;
#endif
	
	/* A NOOP could now, or later be made to, depend on a chdesc on block.
	 * Conservatively say possible cycle for all NOOP dependencies
	 * unless the NOOP is reachable only through chdescs on other blocks.
	 * This check could be more lenient, but NOOPs can have complicated
	 * relations and this check gives a low enough false negative rate. */
	if(dependency->type == NOOP)
		return -2;
	
	/* Check indirect dependencies for induced cycles */
	return merge_indirect_cycle_is_possible(dependency, block);
}

/* chdesc merge stat tracking definitions */
#if CHDESC_MERGE_NEW_STATS
# define N_CHDESC_MERGE_NEW_STATS 6
static uint32_t chdesc_merge_new_stats[N_CHDESC_MERGE_NEW_STATS] = {0,0,0,0,0};
static unsigned chdesc_merge_new_stats_idx = -1;
static bool chdesc_merge_new_stats_callback_registered = 0;

static void print_chdesc_merge_new_stats(void * ignore)
{
	unsigned i;
	uint32_t nchdescs = 0;
	uint32_t nchdescs_notmerged = 0;
	(void) ignore;

	for(i = 0; i < N_CHDESC_MERGE_NEW_STATS; i++)
	{
		nchdescs += chdesc_merge_new_stats[i];
		if(i > 0)
			nchdescs_notmerged += chdesc_merge_new_stats[i];
	}
	
	printf("chdescs merge stats:\n");
	
	if(!nchdescs)
	{
		/* protect against divide by zero */
		printf("\tno chdescs created\n");
		return;
	}
	printf("\tmerged: %u (%3.1f%% all)\n", chdesc_merge_new_stats[0], 100 * ((float) chdesc_merge_new_stats[0]) / ((float) nchdescs));

	if(!nchdescs_notmerged)
	{
		/* protect against divide by zero */
		printf("\tall chdescs merged?!\n");
		return;
	}
	for(i = 1; i < N_CHDESC_MERGE_NEW_STATS; i++)
		printf("\tnot merged case %u: %u (%3.1f%% non-merged)\n", i, chdesc_merge_new_stats[i], 100 * ((float) chdesc_merge_new_stats[i]) / ((float) nchdescs_notmerged));
}

# include <kfs/kfsd.h>
static void chdesc_merge_new_stats_log(unsigned idx)
{
	if(!chdesc_merge_new_stats_callback_registered)
	{
		int r = kfsd_register_shutdown_module(print_chdesc_merge_new_stats, NULL, SHUTDOWN_POSTMODULES);
		if(r < 0)
			panic("kfsd_register_shutdown_module() = %i", r);
		chdesc_merge_new_stats_callback_registered = 1;
	}
	chdesc_merge_new_stats_idx = idx;
	chdesc_merge_new_stats[idx]++;
}
# define CHDESC_MERGE_NEW_STATS_LOG(_idx) chdesc_merge_new_stats_log(_idx)
#else
# define CHDESC_MERGE_NEW_STATS_LOG(_idx) do { } while(0)
#endif

/* Determine whether a new chdesc on 'block', with 'data_required',
 * at 'offset' and 'length', and with the dependency 'dependency' can be merged
 * into an existing chdesc. Return such a chdesc if so, else return NULL. */
static chdesc_t * select_new_chdesc_merger(bdesc_t * block, bool data_required, uint16_t offset, uint16_t length, chdesc_t * dependency)
{
#if !CHDESC_NRB_WHOLEBLOCK
	chdesc_t new;
#endif
	chdesc_t * chdesc;
	chdesc_t * existing = NULL;
	int r;
	
#if !CHDESC_MERGE_NEW
	return NULL;
#endif
	
	if(data_required)
	{
		/* rollbackable chdesc meta relations can be complicated, give up */
		CHDESC_MERGE_NEW_STATS_LOG(1);
		return NULL;
	}
	
	if(dependency && ((r = merge_cycle_is_possible(dependency, block)) < 0))
	{
		CHDESC_MERGE_NEW_STATS_LOG(r == -1 ? 2 : 3);
		return NULL;
	}
	
#if !CHDESC_NRB_WHOLEBLOCK
	new.type = BYTE;
	new.byte.offset = offset;
	new.byte.length = length;
#endif
	/* TODO: eliminate scan of all_changes? */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc->ddesc_next)
	{
		/* rollbackable chdesc meta relations can be complicated */
		if(chdesc_is_rollbackable(chdesc))
		{
			CHDESC_MERGE_NEW_STATS_LOG(4);
			return NULL;
		}

#if CHDESC_NRB_WHOLEBLOCK
		if(!chdesc_is_rollbackable(chdesc))
#else
		if(!chdesc_is_rollbackable(chdesc) && chdesc_byte_contiguous_check(chdesc, &new))
#endif
			/* merge with last nonrollbackable, they are all equally good */
			existing = chdesc;
	}
	
	if(existing)
	{
		CHDESC_MERGE_NEW_STATS_LOG(0);
		return chdesc;
	}
	CHDESC_MERGE_NEW_STATS_LOG(5);
	return NULL;
}

/* Merge what would be a new chdesc into an existing chdesc.
 * Precondition: select_new_chdesc_merger() returned 'existing'. */
static int merge_new_chdesc(chdesc_t * existing, uint16_t new_offset, uint16_t new_length, chdesc_t * new_dependency)
{
#if !CHDESC_NRB_WHOLEBLOCK
	uint16_t updated_offset = MIN(existing->byte.offset, new_offset);
	uint16_t updated_length;
#endif
	int r;
	
	assert(existing && existing->type == BYTE);
	assert(!chdesc_is_rollbackable(existing));
#if CHDESC_NRB_WHOLEBLOCK
	assert(existing->byte.offset == 0);
	assert(existing->byte.length == existing->block->ddesc->length);
#endif
	
	/* Ensure 'existing' depends on 'new_dependency', taking care to not
	 * create a cycle. Cases for 'new_dependency':
	 * - on this block: it is nonrollbackable, so it can be ignored
	 * - on another block: it does not depend on chdescs on this block,
	 *   so it can be added as a dependency
	 * - is a noop: not possible */
	assert(!new_dependency || new_dependency->type != NOOP);
	if(new_dependency && (new_dependency->block->ddesc != existing->block->ddesc))
		if ((r = chdesc_add_depend(existing, new_dependency)) < 0)
			return r;
	
#if !CHDESC_NRB_WHOLEBLOCK
	/* calculate existing's updated location */
	if(new_offset < existing->byte.offset)
	{
		updated_length = existing->byte.length + new_length - (new_offset + new_length - existing->byte.offset);
	}
	else if(new_offset == existing->byte.offset)
	{
		updated_length = MAX(new_length, existing->byte.length);
	}
	else
	{
		assert(new_offset > existing->byte.offset);
		if(new_offset + new_length <= existing->byte.offset + existing->byte.length)
			updated_length = existing->byte.length;
		else
			updated_length = existing->byte.length + new_length - (existing->byte.offset + existing->byte.length - new_offset);
	}

	/* update existing's location */
# if CHDESC_MERGE_NEW
#  warning TODO: merge_new_chdesc() needs to add overlap dependencies
# endif
	if(existing->byte.offset != updated_offset)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OFFSET, existing, updated_offset);
		existing->byte.offset = updated_offset;
	}
	if(existing->byte.length != updated_length)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_LENGTH, existing, updated_length);
		existing->byte.length = updated_length;
	}
#endif
	return 0;
}

#if CHDESC_BYTE_SUM
/* stupid little checksum, just to try and make sure we get the same data */
static uint16_t chdesc_byte_sum(uint8_t * data, size_t length)
{
	uint16_t sum = 0x5AFE;
	while(length-- > 0)
	{
		/* ROL 3 */
		sum = (sum << 3) | (sum >> 13);
		sum ^= *(data++);
	}
	return sum;
}
#endif

int chdesc_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	uint16_t atomic_size = CALL(owner, get_atomicsize);
	uint16_t init_offset = offset % atomic_size;
	uint16_t count = (length + init_offset + atomic_size - 1) / atomic_size;
	
	if(count == 1)
		return chdesc_create_byte(block, owner, offset, length, data, head);
	return -E_INVAL;
}

/* common code to create a byte chdesc */
static int _chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, uint8_t * data, chdesc_t ** head)
{
	bool data_required = new_chdescs_require_data(block);
	chdesc_t * chdesc;
	int r;
	
	assert(block && block->ddesc && owner && head);
	
	if(offset + length > block->ddesc->length)
		return -E_INVAL;
	
	if((r = ensure_bdesc_has_overlaps(block)) < 0)
		return r;
	
	if((chdesc = select_new_chdesc_merger(block, data_required, offset, length, *head)))
	{
		if((r = merge_new_chdesc(chdesc, offset, length, *head)) < 0)
			return r;
		if(data)
			memcpy(&block->ddesc->data[offset], data, length);
		else
			memset(&block->ddesc->data[offset], 0, length);
		*head = chdesc;
		return 0;
	}
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return -E_NO_MEM;
	
	chdesc->owner = owner;		
	chdesc->block = block;
	chdesc->type = BYTE;
#if CHDESC_NRB_WHOLEBLOCK
	if(data_required)
	{
		chdesc->byte.offset = offset;
		chdesc->byte.length = length;
	}
	else
	{
		/* Expand to cover entire block. This is safe since all chdescs on
		 * this block at least implicitly depend on all nonrollbackables.
		 * Leave 'offset' and 'length' as is to copy source data. */
		chdesc->byte.offset = 0;
		chdesc->byte.length = block->ddesc->length;
	}
#else
	chdesc->byte.offset = offset;
	chdesc->byte.length = length;
#endif
	
	if(data_required)
	{
		chdesc->byte.data = data ? memdup(data, length) : calloc(1, length);
		if(!chdesc->byte.data)
		{
			free(chdesc);
			return -E_NO_MEM;
		}
#if CHDESC_BYTE_SUM
		chdesc->byte.old_sum = chdesc_byte_sum(&block->ddesc->data[offset], length);
		chdesc->byte.new_sum = chdesc_byte_sum(chdesc->byte.data, length);
#endif
	}
	else
	{
		chdesc->byte.data = NULL;
#if CHDESC_BYTE_SUM
		chdesc->byte.old_sum = 0;
		chdesc->byte.new_sum = 0;
#endif
	}
	
	chdesc->dependencies = NULL;
	chdesc->dependencies_tail = &chdesc->dependencies;
	chdesc->dependents = NULL;
	chdesc->dependents_tail = &chdesc->dependents;
	chdesc->weak_refs = NULL;
	memset(chdesc->befores, 0, sizeof(chdesc->befores));
	chdesc->free_prev = NULL;
	chdesc->free_next = NULL;
	chdesc->ddesc_next = NULL;
	chdesc->ddesc_pprev = NULL;
	chdesc->ddesc_ready_next = NULL;
	chdesc->ddesc_ready_pprev = NULL;
	chdesc->tmp_next = NULL;
	chdesc->tmp_pprev = NULL;
	chdesc->stamps = 0;
	
	/* start rolled back so we can apply it */
	chdesc->flags = CHDESC_ROLLBACK;
		
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdesc, block, owner, chdesc->byte.offset, chdesc->byte.length);
	
	chdesc_link_all_changes(chdesc);
	chdesc_link_ready_changes(chdesc);
	if(chdesc_add_depend_fast(block->ddesc->overlaps, chdesc) < 0)
	{
		chdesc_destroy(&chdesc);
		return -E_NO_MEM;
	}
	
	/* make sure it is dependent upon any pre-existing chdescs */
	if(chdesc_overlap_multiattach(chdesc, block))
	{
		chdesc_destroy(&chdesc);
		return -E_NO_MEM;
	}
	
	/* this is a new chdesc, so we don't need to check for loops.
	 * but we should check to make sure head has not already been written. */
	if(*head && !((*head)->flags & CHDESC_WRITTEN))
		if((r = chdesc_add_depend_fast(chdesc, *head)) < 0)
		{
			chdesc_destroy(&chdesc);
			return r;
		}
	
	if(data_required)
	{	
		if((r = chdesc_apply(chdesc)) < 0)
		{
			chdesc_destroy(&chdesc);
			return r;
		}
	}
	else
	{
		if(data)
			memcpy(&chdesc->block->ddesc->data[offset], data, length);
		else
			memset(&chdesc->block->ddesc->data[offset], 0, length);
		chdesc->flags &= ~CHDESC_ROLLBACK;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_APPLY, chdesc);
	}
	
	*head = chdesc;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	return 0;
}

int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	if(&block->ddesc->data[offset] == data)
		panic("Cannot create a change descriptor in place!");
	return _chdesc_create_byte(block, owner, offset, length, (uint8_t *) data, head);
}

int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head)
{
	return _chdesc_create_byte(block, owner, 0, block->ddesc->length, NULL, head);
}

int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	return _chdesc_create_byte(block, owner, 0, block->ddesc->length, data, head);
}

int chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor, chdesc_t ** head)
{
	bool data_required = new_chdescs_require_data(block);
	chdesc_t * chdesc;
	chdesc_t * bit_changes;
	int r;
	
	if((chdesc = select_new_chdesc_merger(block, data_required, offset * 4, 4, *head)))
	{
		if((r = merge_new_chdesc(chdesc, offset * 4, 4, *head)) < 0)
			return r;
		((uint32_t *) block->ddesc->data)[offset] ^= xor;
		*head = chdesc;
		return 0;
	}
	
	if(!data_required)
	{
		uint32_t data = ((uint32_t *) block->ddesc->data)[offset] ^ xor;
#if CHDESC_MERGE_NEW_STATS
		chdesc_merge_new_stats[chdesc_merge_new_stats_idx]--; /* don't double count */
#endif
		return _chdesc_create_byte(block, owner, offset * 4, 4, (uint8_t *) &data, head);
	}
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BIT, chdesc, block, owner, offset, xor);
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	chdesc->dependencies = NULL;
	chdesc->dependencies_tail = &chdesc->dependencies;
	chdesc->dependents = NULL;
	chdesc->dependents_tail = &chdesc->dependents;
	chdesc->weak_refs = NULL;
	memset(chdesc->befores, 0, sizeof(chdesc->befores));
	chdesc->free_prev = NULL;
	chdesc->free_next = NULL;
	chdesc->ddesc_next = NULL;
	chdesc->ddesc_pprev = NULL;
	chdesc->ddesc_ready_next = NULL;
	chdesc->ddesc_ready_pprev = NULL;
	chdesc->tmp_next = NULL;
	chdesc->tmp_pprev = NULL;
	chdesc->stamps = 0;

	/* start rolled back so we can apply it */
	chdesc->flags = CHDESC_ROLLBACK;

	chdesc_link_ready_changes(chdesc);
	
	/* make sure it is dependent upon any pre-existing chdescs */
	if((r = chdesc_overlap_multiattach(chdesc, block)) < 0)
		goto error;
	
	/* this is a new chdesc, so we don't need to check for loops.
	 * but we should check to make sure head has not already been written. */
	if(*head && !((*head)->flags & CHDESC_WRITTEN))
		if((r = chdesc_add_depend_fast(chdesc, *head)) < 0)
			goto error;
	
	/* make sure it applies cleanly */
	if((r = chdesc_apply(chdesc)) < 0)
		goto error;
	
	/* add chdesc to block's dependencies */
	chdesc_link_all_changes(chdesc);
	if(!(bit_changes = ensure_bdesc_has_bit_changes(block, offset)))
	{
		r = -E_NO_MEM;
		goto error;
	}
	if((r = chdesc_add_depend_fast(bit_changes, chdesc)) < 0)
		goto error;
	
	*head = chdesc;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	return 0;
	
  error:
	chdesc_destroy(&chdesc);
	return r;
}

/* Rewrite a byte change descriptor to have an updated "new data" field,
 * avoiding the need to create layers of byte change descriptors if the previous
 * changes are no longer relevant (e.g. if they are being overwritten and will
 * never need to be rolled back independently from the new data). The change
 * descriptor must not be overlapped by any other change descriptors. The offset
 * and length parameters are relative to the change descriptor itself. */
int chdesc_rewrite_byte(chdesc_t * chdesc, uint16_t offset, uint16_t length, void * data)
{
	/* sanity checks */
	if(chdesc->type != BYTE)
		return -E_INVAL;
	if(offset + length > chdesc->byte.offset + chdesc->byte.length)
		return -E_INVAL;
	
	/* scan for overlapping chdescs - they will all depend on us, or at
	 * least, if there are any, at least one will depend directly on us */
	if(chdesc->dependents)
	{
		chmetadesc_t * meta;
		for(meta = chdesc->dependents; meta; meta = meta->dependent.next)
		{
			/* no block? doesn't overlap */
			if(!meta->dependent.desc->block)
				continue;
			/* not the same block? doesn't overlap */
			if(meta->dependent.desc->block->ddesc != chdesc->block->ddesc)
				continue;
			/* chdesc_overlap_check doesn't check that the block is
			 * the same, which is why we just checked it by hand */
			if(!chdesc_overlap_check(meta->dependent.desc, chdesc))
				continue;
			/* overlap detected! */
			return -E_PERM;
		}
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REWRITE_BYTE, chdesc);
	
	/* no overlaps */
	if(chdesc->flags & CHDESC_ROLLBACK)
	{
		memcpy(&chdesc->byte.data[offset], data, length);
#if CHDESC_BYTE_SUM
		chdesc->byte.new_sum = chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length);
#endif
	}
	else
	{
		memcpy(&chdesc->block->ddesc->data[chdesc->byte.offset + offset], data, length);
#if CHDESC_BYTE_SUM
		chdesc->byte.new_sum = chdesc_byte_sum(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.length);
#endif
	}
	return 0;
}

#if CHDESC_CYCLE_CHECK
static int chdesc_has_dependency(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, dependent, CHDESC_MARKED);
	dependent->flags |= CHDESC_MARKED;
	for(meta = dependent->dependencies; meta; meta = meta->dependency.next)
	{
		if(meta->dependency.desc == dependency)
			return 1;
		if(!(meta->dependency.desc->flags & CHDESC_MARKED))
			if(chdesc_has_dependency(meta->dependency.desc, dependency))
				return 1;
	}
	/* the chdesc graph is a DAG, so unmarking here would defeat the purpose */
	return 0;
}
#endif

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	/* compensate for Heisenberg's uncertainty principle */
	if(!dependent || !dependency)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided use of NULL pointer!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	/* make sure we're not fiddling with chdescs that are already written */
	if(dependent->flags & CHDESC_WRITTEN)
	{
		if(dependency->flags & CHDESC_WRITTEN)
			return 0;
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Attempt to add dependency to already written data!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	if(dependency->flags & CHDESC_WRITTEN)
		return 0;
	
	/* avoid creating a dependency loop */
#if CHDESC_CYCLE_CHECK
	if(dependent == dependency || chdesc_has_dependency(dependency, dependent))
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	/* chdesc_has_dependency() marks the DAG rooted at "dependency" so we must unmark it */
	chdesc_unmark_graph(dependency);
#endif
	
	return chdesc_add_depend_fast(dependent, dependency);
}

static void chdesc_meta_remove(chmetadesc_t * meta)
{
	unpropagate_dependency(meta->dependent.desc, meta->dependency.desc);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENCY, meta->dependent.desc, meta->dependency.desc);
	*meta->dependency.ptr = meta->dependency.next;
	if(meta->dependency.next)
		meta->dependency.next->dependency.ptr = meta->dependency.ptr;
	else
		meta->dependent.desc->dependencies_tail = meta->dependency.ptr;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_DEPENDENT, meta->dependency.desc, meta->dependent.desc);
	*meta->dependent.ptr = meta->dependent.next;
	if(meta->dependent.next)
		meta->dependent.next->dependent.ptr = meta->dependent.ptr;
	else
		meta->dependency.desc->dependents_tail = meta->dependent.ptr;
	
	if(meta->dependent.desc->type == NOOP && !meta->dependent.desc->dependencies)
		/* we just removed the last dependency of a NOOP chdesc, so satisfy it */
		chdesc_satisfy(&meta->dependent.desc);
	
	memset(meta, 0, sizeof(*meta));
	free(meta);
}

void chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * scan_dependencies = dependent->dependencies;
	chmetadesc_t * scan_dependents = dependency->dependents;
	while(scan_dependencies && scan_dependents &&
	      scan_dependencies->dependency.desc != dependency &&
	      scan_dependents->dependent.desc != dependent)
	{
		scan_dependencies = scan_dependencies->dependency.next;
		scan_dependents = scan_dependents->dependent.next;
	}
	if(scan_dependencies && scan_dependencies->dependency.desc == dependency)
		chdesc_meta_remove(scan_dependencies);
	else if(scan_dependents && scan_dependents->dependent.desc == dependent)
		chdesc_meta_remove(scan_dependents);
}

static void memxchg(void * p, void * q, size_t n)
{
	/* align at least p on 32-bit boundary */
	while((uint32_t) p % 4 && n > 0)
	{
		uint8_t c = *(uint8_t *) p;
		*(uint8_t *) p++ = *(uint8_t *) q;
		*(uint8_t *) q++ = c;
		n--;
	}
	while(n > 3)
	{
		uint32_t d = *(uint32_t *) p;
		*(uint32_t *) p = *(uint32_t *) q;
		*(uint32_t *) q = d;
		p += 4;
		q += 4;
		n -= 4;
	}
	while(n > 0)
	{
		uint8_t c = *(uint8_t *) p;
		*(uint8_t *) p++ = *(uint8_t *) q;
		*(uint8_t *) q++ = c;
		n--;
	}
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
			if(!chdesc->byte.data)
				return -E_INVAL;
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.new_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			memxchg(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.data, chdesc->byte.length);
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.old_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			break;
		case NOOP:
			/* NOOP application is easy! */
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	chdesc->flags &= ~CHDESC_ROLLBACK;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_APPLY, chdesc);
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
			if(!chdesc->byte.data)
				return -E_INVAL;
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.old_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			memxchg(&chdesc->block->ddesc->data[chdesc->byte.offset], chdesc->byte.data, chdesc->byte.length);
#if CHDESC_BYTE_SUM
			if(chdesc_byte_sum(chdesc->byte.data, chdesc->byte.length) != chdesc->byte.new_sum)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): BYTE chdesc %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, chdesc, KFS_DEBUG_COUNT());
#endif
			break;
		case NOOP:
			/* NOOP rollback is easy! */
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, chdesc->type);
			return -E_INVAL;
	}
	chdesc->flags |= CHDESC_ROLLBACK;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ROLLBACK, chdesc);
	return 0;
}

static void chdesc_weak_collect(chdesc_t * chdesc)
{
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_WEAK_COLLECT, chdesc);
	while(chdesc->weak_refs)
	{
		/* in theory, this is all that is necessary... */
		if(*chdesc->weak_refs->desc == chdesc)
			chdesc_weak_release(chdesc->weak_refs->desc);
		else
		{
			/* ...but check for this anyway */
			chrefdesc_t * next = chdesc->weak_refs;
			kdprintf(STDERR_FILENO, "%s: (%s:%d): dangling chdesc weak reference!\n", __FUNCTION__, __FILE__, __LINE__);
			chdesc->weak_refs = next->next;
			free(next);
		}
	}
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it and add it to the list of written chdescs */
int chdesc_satisfy(chdesc_t ** chdesc)
{
	if((*chdesc)->flags & CHDESC_WRITTEN)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfaction of already satisfied chdesc!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_SATISFY, *chdesc);
	
	if((*chdesc)->dependencies)
	{
		chdesc_t * bit_changes;
		/* We are trying to satisfy a chdesc with dependencies, which
		 * can happen if we have modules generating out-of-order chdescs
		 * but no write-back caches. We need to convert it to a NOOP so
		 * that any of its dependents will still have the indirect
		 * dependencies on the dependencies of this chdesc. However, we
		 * still need to collect any weak references to it in case
		 * anybody was watching it to see when it got satisfied. */
		if((*chdesc)->type != NOOP)
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfying chdesc %p of type %d with dependencies!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc, (*chdesc)->type);
		switch((*chdesc)->type)
		{
			case BYTE:
				if((*chdesc)->byte.data)
				{
					free((*chdesc)->byte.data);
					(*chdesc)->byte.data = NULL;
					/* data == NULL does not mean "cannot be rolled back" since the chdesc is satisfied */
				}
				chdesc_remove_depend((*chdesc)->block->ddesc->overlaps, *chdesc);
				(*chdesc)->type = NOOP;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, *chdesc);
				break;
			case BIT:
				bit_changes = chdesc_bit_changes((*chdesc)->block, (*chdesc)->bit.offset);
				assert(bit_changes);
				chdesc_remove_depend(bit_changes, *chdesc);
				(*chdesc)->type = NOOP;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, *chdesc);
				/* fall through */
			case NOOP:
				break;
			default:
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
				return -E_INVAL;
		}
		
	}
	else
	{
		while((*chdesc)->dependents)
			chdesc_meta_remove((*chdesc)->dependents);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, *chdesc, CHDESC_WRITTEN);
		(*chdesc)->flags |= CHDESC_WRITTEN;
		
		/* we don't need the data in byte change descriptors anymore */
		if((*chdesc)->type == BYTE)
		{
			if((*chdesc)->byte.data)
			{
				free((*chdesc)->byte.data);
				(*chdesc)->byte.data = NULL;
				/* data == NULL does not mean "cannot be rolled back" since the chdesc is satisfied */
			}
		}
		
		/* make sure we're not already destroying this chdesc */
		if(!((*chdesc)->flags & CHDESC_FREEING))
		{
			assert(!(*chdesc)->free_prev && !(*chdesc)->free_next);
			chdesc_free_push(*chdesc);
		}
	}
	
	chdesc_unlink_ready_changes(*chdesc);
	chdesc_unlink_all_changes(*chdesc);
	
	chdesc_weak_collect(*chdesc);
	
	if((*chdesc)->type == NOOP)
	{
		if((*chdesc)->flags & CHDESC_BIT_NOOP)
		{
			assert((*chdesc)->noop.bit_changes);
			/* it should already be NULL from the weak reference */
			assert(!hash_map_find_val((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key));
			hash_map_erase((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key);
			KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdesc, CHDESC_BIT_NOOP);
			(*chdesc)->flags &= ~CHDESC_BIT_NOOP;
		}
	}
	
	*chdesc = NULL;
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
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_RETAIN, chdesc, location);
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
			kdprintf(STDERR_FILENO, "%s: (%s:%d) weak release/forget of non-weak chdesc pointer!\n", __FUNCTION__, __FILE__, __LINE__);
			return;
		}
		*prev = scan->next;
		free(scan);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_FORGET, *location, location);
	}
}

void chdesc_weak_release(chdesc_t ** location)
{
	chdesc_weak_forget(location);
	*location = NULL;
}

void chdesc_destroy(chdesc_t ** chdesc)
{
	/* were we recursively called by chdesc_remove_depend()? */
	if((*chdesc)->flags & CHDESC_FREEING)
		return;
	(*chdesc)->flags |= CHDESC_FREEING;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, *chdesc, CHDESC_FREEING);
	
	if((*chdesc)->flags & CHDESC_WRITTEN)
	{
		assert(!(*chdesc)->dependents && !(*chdesc)->dependencies);
		if(free_head == *chdesc || (*chdesc)->free_prev)
			chdesc_free_remove(*chdesc);
	}
	else
	{
		/* this is perfectly allowed, but while we are switching to this new system, print a warning */
		if((*chdesc)->type != NOOP)
		{
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
			if((*chdesc)->flags & CHDESC_OVERLAP)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying completely overlapping unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
		}
		else if(free_head == *chdesc || (*chdesc)->free_prev)
		{
			assert(!(*chdesc)->dependencies);
			chdesc_free_remove(*chdesc);
		}
	}
	
	if((*chdesc)->dependencies && (*chdesc)->dependents)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying chdesc with both dependents and dependencies!\n", __FUNCTION__, __FILE__, __LINE__);
	/* remove dependencies first, so chdesc_satisfy() won't just turn it to a NOOP */
	while((*chdesc)->dependencies)
		chdesc_meta_remove((*chdesc)->dependencies);
	if((*chdesc)->dependents)
	{
		/* chdesc_satisfy will set it to NULL */
		chdesc_t * desc = *chdesc;
		chdesc_satisfy(&desc);
	}

	chdesc_unlink_ready_changes(*chdesc);
	chdesc_unlink_all_changes(*chdesc);
	
	chdesc_weak_collect(*chdesc);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_DESTROY, *chdesc);
	
	switch((*chdesc)->type)
	{
		case BYTE:
			assert(!(*chdesc)->byte.data);
			if((*chdesc)->byte.data)
				free((*chdesc)->byte.data);
			break;
		case NOOP:
			if((*chdesc)->flags & CHDESC_BIT_NOOP)
			{
				assert((*chdesc)->noop.bit_changes);
				/* it should already be NULL from the weak reference */
				assert(!hash_map_find_val((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key));
				hash_map_erase((*chdesc)->noop.bit_changes, (*chdesc)->noop.hash_key);
			}
			/* fall through */
		case BIT:
			break;
		default:
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
	}
	
	if((*chdesc)->block)
		bdesc_release(&(*chdesc)->block);
	
	memset(*chdesc, 0, sizeof(**chdesc));
	free(*chdesc);
	*chdesc = NULL;
}

void chdesc_claim_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->dependencies);
	assert(chdesc_before_level(chdesc) == BDLEVEL_NONE);
	if(chdesc->free_prev || free_head == chdesc)
		chdesc_free_remove(chdesc);
}

void chdesc_autorelease_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->dependencies && !(chdesc->flags & CHDESC_WRITTEN));
	assert(chdesc_before_level(chdesc) == BDLEVEL_NONE);
	while(chdesc->dependents)
		chdesc_meta_remove(chdesc->dependents);
	if(!chdesc->free_prev && free_head != chdesc)
		chdesc_free_push(chdesc);
}

void chdesc_reclaim_written(void)
{
	while(free_head)
	{
		chdesc_t * first = free_head;
		chdesc_free_remove(first);
		chdesc_destroy(&first);
	}
}

static BD_t * stamps[32] = {0};

uint32_t chdesc_register_stamp(BD_t * bd)
{
	int i;
	for(i = 0; i != 32; i++)
		if(!stamps[i])
		{
			stamps[i] = bd;
			return 1 << i;
		}
	return 0;
}

void chdesc_release_stamp(uint32_t stamp)
{
	if(stamp)
	{
		int i;
		for(i = -1; stamp; i++)
			stamp >>= 1;
		stamps[i] = NULL;
	}
}
