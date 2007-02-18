#include <lib/error.h>
#include <lib/assert.h>
#include <lib/kdprintf.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>
#include <lib/memdup.h>
#include <lib/stdarg.h>

#include <kfs/debug.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

/* Set to check for chdesc dependency cycles. Values: 0 (disable), 1 (enable) */
#define CHDESC_CYCLE_CHECK 0

/* Set to count change descriptors by type and display periodic output */
#define COUNT_CHDESCS 0

/* Change descriptor multigraphs allow more than one dependency between the same
 * two change descriptors. This currently saves us the trouble of making sure we
 * don't create a duplicate dependency between chdescs, though it also causes us
 * to allocate somewhat more memory in many cases where we would otherwise
 * detect the duplicate dependency. Allowing multigraphs results in a reasonable
 * speedup, even though we use more memory, so it is enabled by default. */
#define CHDESC_ALLOW_MULTIGRAPH 1

/* Set to track the nrb chdesc merge stats and print them after shutdown */
#define CHDESC_NRB_MERGE_STATS (CHDESC_NRB && 0)

/* Set to merge all existing RBs into a NRB when creating a NRB on the block */
#define CHDESC_MERGE_RBS_NRB (CHDESC_NRB && 0)
/* Set to track RBs->NRB merge stats */
#define CHDESC_MERGE_RBS_NRB_STATS (CHDESC_MERGE_RBS_NRB && 0)

/* Set to merge a simple overlapping RB into the underlying chdesc */
#define CHDESC_MERGE_OVERLAP 1

static void chdesc_dep_remove(chdepdesc_t * dep);

#if COUNT_CHDESCS
#include <lib/jiffies.h>
/* indices match chdesc->type */
static uint32_t chdesc_counts[3];
static void dump_counts(void)
{
	static int last_count_dump = 0;
	int jiffies = jiffy_time();
	if(!last_count_dump)
		last_count_dump = jiffies;
	else if(jiffies - last_count_dump >= HZ)
	{
		while(jiffies - last_count_dump >= HZ)
			last_count_dump += HZ;
		printk(KERN_ERR "Bit: %4d, Byte: %4d, Noop: %4d\n", chdesc_counts[BIT], chdesc_counts[BYTE], chdesc_counts[NOOP]);
	}
}
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
	int r;
	assert(block);
	
	if(block->ddesc->overlaps)
	{
		assert(block->ddesc->overlaps->type == NOOP);
		return 0;
	}
	
	r = chdesc_create_noop_list(NULL, NULL, &chdesc, NULL);
	if(r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, chdesc, "overlaps");
	r = chdesc_weak_retain(chdesc, &block->ddesc->overlaps);
	if(r < 0)
	{
		chdesc_destroy(&chdesc);
		return r;
	}
	
	return 0;
}

/* ensure bdesc->ddesc->bit_changes[offset] has a noop chdesc */
static chdesc_t * ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset)
{
	chdesc_t * chdesc;
	hash_map_elt_t * elt;
	void * key = (void *) (uint32_t) offset;
	int r;
	assert(block);
	
	chdesc = (chdesc_t *) hash_map_find_val(block->ddesc->bit_changes, key);
	if(chdesc)
	{
		assert(chdesc->type == NOOP);
		return chdesc;
	}
	
	r = chdesc_create_noop_list(NULL, NULL, &chdesc, NULL);
	if(r < 0)
		return NULL;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, chdesc, "bit_changes");
	
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

/* Helper macro for recursion-on-the-heap support:
 * Increment 'state' pointer and, if needed, enlarge the 'states'
 * array (and 'states_capacity', accordingly) */
#define INCREMENT_STATE(state, static_states, states, states_capacity) \
	do { \
		size_t next_index = 1 + state - &states[0]; \
		if(next_index < states_capacity) \
			state++; \
		else \
		{ \
			size_t cur_size = states_capacity * sizeof(*states); \
			states_capacity *= 2; \
			if(states == static_states) \
			{ \
				states = smalloc(states_capacity * sizeof(*states)); \
				if(states) \
					memcpy(states, static_states, cur_size); \
			} \
			else \
				states = srealloc(states, cur_size, states_capacity * sizeof(*states)); \
			if(!states) \
				kpanic("smalloc/srealloc(%u bytes) failed", states_capacity * sizeof(states)); \
			state = &states[next_index]; \
		} \
	} while(0)

#define STATIC_STATES_CAPACITY 1024 /* 1024 is fairly arbitrary */

/* propagate a level change through the noop after,
 * to update the ready state */
static void propagate_level_change_thru_noop(chdesc_t * noop_after, uint16_t prev_level, uint16_t new_level)
{
	/* recursion-on-the-heap support */
	struct state {
		chdepdesc_t * noops_afters;
		uint16_t prev_level;
		uint16_t new_level;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;

  recurse_enter:
	assert(!noop_after->owner);
	assert(prev_level != new_level);
	assert(prev_level != BDLEVEL_NONE || new_level != BDLEVEL_NONE);

	chdepdesc_t * noops_afters = noop_after->afters;
	for(; noops_afters; noops_afters = noops_afters->after.next)
	{
		chdesc_t * after = noops_afters->after.desc;
		uint16_t after_prev_level = chdesc_level(after);

		if(prev_level != BDLEVEL_NONE)
		{
			assert(after->nbefores[prev_level]);
			after->nbefores[prev_level]--;
		}
		if(new_level != BDLEVEL_NONE)
		{
			after->nbefores[new_level]++;
			assert(after->nbefores[new_level]);
		}
		chdesc_update_ready_changes(after);

		if(!after->owner)
		{
			uint16_t after_new_level = chdesc_level(after);
			if(after_prev_level != after_new_level)
			{
				/* Recursively propagate the level change; equivalent to
				 * propagate_level_change_thru_noop
				 *  (after, after_prev_level, after_new_level) */
				state->noops_afters = noops_afters;
				state->prev_level = prev_level;
				state->new_level = new_level;

				noop_after = after;
				prev_level = after_prev_level;
				new_level = after_new_level;

				INCREMENT_STATE(state, static_states, states, states_capacity);
				goto recurse_enter;

			  recurse_resume:
				(void) 0; /* placate compiler re deprecated end labels */
			}
		}
	}

	if(state != &states[0])
	{
		state--;
		noops_afters = state->noops_afters;
		prev_level = state->prev_level;
		new_level = state->new_level;
		goto recurse_resume;
	}

	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
}

#if BDESC_EXTERN_AFTER_COUNT
/* return whether 'chdesc' is on a different block than 'block' */
static bool chdesc_is_external(const chdesc_t * chdesc, const bdesc_t * block)
{
	assert(chdesc);
	assert(block && block->ddesc);
	if(chdesc->type == NOOP)
	{
		if(chdesc->block && chdesc->block->ddesc != block->ddesc)
			return 1;
	}
	else if(chdesc->block->ddesc != block->ddesc)
		return 1;
	return 0;
}

#define BDESC_EXTERN_AFTER_COUNT_DEBUG 0
#if BDESC_EXTERN_AFTER_COUNT_DEBUG
/* return the number of external afters 'chdesc' has with respect
 * to 'block' */
static bool count_chdesc_external_afters(const chdesc_t * chdesc, const bdesc_t * block)
{
	const chdepdesc_t * afters;
	uint32_t n = 0;
	for(afters = chdesc->afters; afters; afters = afters->after.next)
	{
		const chdesc_t * after = afters->after.desc;
		if(!after->block)
		{
			/* XXX: stack usage */
			n += count_chdesc_external_afters(after, block);
		}
		else if(after->block->ddesc != block->ddesc)
			n++;
	}
	return n;
}

/* return the number of external afters for 'block' */
static uint32_t count_bdesc_external_afters(const bdesc_t * block)
{
	const chdesc_t * c;
	uint32_t n = 0;
	for(c = block->ddesc->all_changes; c; c = c->ddesc_next)
		n += count_chdesc_external_afters(c, block);
	return n;
}

/* return whether the external after count in 'block' agrees with
 * an actual count */
static bool extern_after_count_is_correct(const bdesc_t * block)
{
	return !block || (count_bdesc_external_afters(block) == block->ddesc->extern_after_count);
}
#endif /* BDESC_EXTERN_AFTER_COUNT_DEBUG */

/* propagate a depend add/remove through a noop after,
 * to increment/decrement extern_after_count for 'block' */
static void propagate_extern_after_change_thru_noop_after(const chdesc_t * noop_after, bdesc_t * block, bool add)
{
	chdepdesc_t * dep;
	assert(noop_after->type == NOOP && !noop_after->block);
	assert(block);
	for(dep = noop_after->afters; dep; dep = dep->after.next)
	{
		chdesc_t * after = dep->after.desc;
		if(!after->block)
		{
			assert(after->type == NOOP);
			/* XXX: stack usage */
			propagate_extern_after_change_thru_noop_after(after, block, add);
		}
		else if(chdesc_is_external(after, block))
		{
			if(add)
			{
				block->ddesc->extern_after_count++;
				assert(block->ddesc->extern_after_count);
			}
			else
			{
				assert(block->ddesc->extern_after_count);
				block->ddesc->extern_after_count--;
			}
		}
	}
}

/* propagate a depend add through a noop before,
 * to increment extern_after_count for before's block */
static void propagate_extern_after_change_thru_noop_before(chdesc_t * noop_before, const chdesc_t * after, bool add)
{
	chdepdesc_t * dep;
	assert(noop_before->type == NOOP && !noop_before->block);
	assert(after->type != NOOP);
	for(dep = noop_before->befores; dep; dep = dep->before.next)
	{
		chdesc_t * before = dep->before.desc;
		if(!before->block)
		{
			assert(before->type == NOOP);
			/* XXX: stack usage */
			propagate_extern_after_change_thru_noop_before(before, after, add);
		}
		else if(chdesc_is_external(after, before->block))
		{
			if(add)
			{
				before->block->ddesc->extern_after_count++;
				assert(before->block->ddesc->extern_after_count);
			}
			else
			{
				assert(before->block->ddesc->extern_after_count);
				before->block->ddesc->extern_after_count--;
			}
		}
	}
}
#endif /* BDESC_EXTERN_AFTER_COUNT */

/* propagate a depend add, to update ready and extern_after state */
static void propagate_depend_add(chdesc_t * after, chdesc_t * before)
{
	uint16_t before_level = chdesc_level(before);
	uint16_t after_prev_level;
	
	if(before_level == BDLEVEL_NONE)
		return;
	after_prev_level = chdesc_level(after);
	
	after->nbefores[before_level]++;
	assert(after->nbefores[before_level]);
	chdesc_update_ready_changes(after);
	if(!after->owner && (before_level > after_prev_level || after_prev_level == BDLEVEL_NONE))
			propagate_level_change_thru_noop(after, after_prev_level, before_level);
#if BDESC_EXTERN_AFTER_COUNT
	if(!after->block)
	{
		if(before->block)
			propagate_extern_after_change_thru_noop_after(after, before->block, 1);
		else
		{
			/* Note: if we allow adding a dependency from data->NOOP
			 * to NOOP->data, propagate_extern_after_change_thru_noop_after()
			 * should also propagate thru before noops to before blocks.
			 * It seems likely that the propagate_extern_after_*() functions
			 * should be merged into a single interface function that handles
			 * all data/noop -before and -after combinations */
			assert(!after->afters || !before->befores);
		}
	}
	else if(!before->block)
		propagate_extern_after_change_thru_noop_before(before, after, 1);
	else if(chdesc_is_external(after, before->block))
	{
		before->block->ddesc->extern_after_count++;
		assert(before->block->ddesc->extern_after_count);
	}
#endif
}

/* propagate a depend remove, to update ready and extern_after state */
static void propagate_depend_remove(chdesc_t * after, const chdesc_t * before)
{
	uint16_t before_level = chdesc_level(before);
	uint16_t after_prev_level;
	
	if(before_level == BDLEVEL_NONE)
		return;
	after_prev_level = chdesc_level(after);
	
	assert(after->nbefores[before_level]);
	after->nbefores[before_level]--;
	chdesc_update_ready_changes(after);
	if(!after->owner && (before_level == after_prev_level && !after->nbefores[before_level]))
		propagate_level_change_thru_noop(after, after_prev_level, chdesc_level(after));
#if BDESC_EXTERN_AFTER_COUNT
	if(!after->block)
	{
		if(before->block)
			propagate_extern_after_change_thru_noop_after(after, before->block, 0);
		else
		{
			/* Note: if we remove a dependency from data->NOOP to NOOP->data,
			 * propagate_extern_after_change_thru_noop_after() should
			 * also propagate thru noops to before blocks, as in
			 * propagate_depend_add().
			 * TODO: Can move_befores_for_merge() remove such dependencies? */
			assert(!after->afters || !before->befores);
		}
	}
	else if(!before->block)
	{
		/* TODO: don't we need to propagate the extern_after remove through
		 * a noop before? (The mirror of propagate_depend_add()'s action.) */
		assert(!before->befores);
	}
	else if(chdesc_is_external(after, before->block))
	{
		assert(before->block->ddesc->extern_after_count);
		before->block->ddesc->extern_after_count--;
	}
#endif
}

/* propagate a level change, to update ready state */
void chdesc_propagate_level_change(chdesc_t * chdesc, uint16_t prev_level, uint16_t new_level)
{
	chdepdesc_t * afters = chdesc->afters;
	assert(prev_level < NBDLEVEL || prev_level == BDLEVEL_NONE);
	assert(new_level < NBDLEVEL || new_level == BDLEVEL_NONE);
	assert(prev_level != new_level);
	for(; afters; afters = afters->after.next)
	{
		chdesc_t * after = afters->after.desc;
		uint16_t after_prev_level = chdesc_level(after);

		if(prev_level != BDLEVEL_NONE)
		{
			assert(after->nbefores[prev_level]);
			after->nbefores[prev_level]--;
		}
		if(new_level != BDLEVEL_NONE)
		{
			after->nbefores[new_level]++;
			assert(after->nbefores[new_level]);
		}
		chdesc_update_ready_changes(after);

		if(!after->owner)
		{
			uint16_t after_new_level = chdesc_level(after);
			if(after_prev_level != after_new_level)
				propagate_level_change_thru_noop(after, after_prev_level, after_new_level);
		}
	}
}

/* add a dependency between change descriptors without checking for cycles */
static int chdesc_add_depend_fast(chdesc_t * after, chdesc_t * before)
{
	chdepdesc_t * dep;
	
	if(!(after->flags & CHDESC_SAFE_AFTER))
	{
		assert(after->type == NOOP && !after->afters); /* quickly catch bugs for now */
		if(after->type != NOOP || after->afters)
			return -E_INVAL;
	}
	
#if !CHDESC_ALLOW_MULTIGRAPH
	/* make sure it's not already there */
	for(dep = after->befores; dep; dep = dep->before.next)
		if(dep->desc == before)
			return 0;
	/* shouldn't be there */
	for(dep = before->afters; dep; dep = dep->after.next)
		assert(dep->desc != after);
#endif
	
	dep = malloc(sizeof(*dep));
	if(!dep)
		return -E_NO_MEM;
	
	propagate_depend_add(after, before);
	
	/* add the before to the after */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_BEFORE, after, before);
	dep->before.desc = before;
	dep->before.next = NULL;
	dep->before.ptr = after->befores_tail;
	*after->befores_tail = dep;
	after->befores_tail = &dep->before.next;
	
	/* add the after to the before */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_ADD_AFTER, before, after);
	dep->after.desc = after;
	dep->after.next = NULL;
	dep->after.ptr = before->afters_tail;
	*before->afters_tail = dep;
	before->afters_tail = &dep->after.next;
	
	/* virgin NOOP chdesc getting its first before */
	if(free_head == after || after->free_prev)
	{
		assert(after->type == NOOP);
		assert(!(after->flags & CHDESC_WRITTEN));
		chdesc_free_remove(after);
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
		kdprintf(STDERR_FILENO, "Attempt to overlap a new chdesc (%p) with a rolled-back chdesc (%p)! (debug = %d)\n", recent, original, KFS_DEBUG_COUNT());
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
	chdepdesc_t * list = list_chdesc->befores;
	chdepdesc_t * next = list;
	while((list = next))
	{
		int r;
		
		/* this loop is tricky, because we might remove the item we're
		 * looking at currently if we overlap it entirely - so we
		 * prefetch the next pointer at the top of the loop */
		next = list->before.next;
		
		list_chdesc = list->before.desc;
		
		if(chdesc == list_chdesc)
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

/* NOOP chdescs may have:
 * - NULL block and owner, in which case it is a "normal" NOOP.
 *   (propagates before/after counts. propagates external counts.)
 * - NULL block and non-NULL owner: has a device level and thus prevents its afters
 *   from going lower than that device.
 *   (counts towards before/after counts. propagates external counts.)
 * - non-NULL block and owner: makes the block dirty and can prevent the block from
 *   being evicted from a cache, is internal/external.
 *   (counts towards before/after counts. counts towards external counts.) */

int chdesc_create_noop_array(bdesc_t * block, BD_t * owner, chdesc_t ** tail, size_t nbefores, chdesc_t * befores[])
{
	chdesc_t * chdesc;
	size_t i;
	int r;
	
	assert(tail);
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_NOOP, chdesc, block, owner);
#if COUNT_CHDESCS
	chdesc_counts[NOOP]++;
	dump_counts();
#endif
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->befores = NULL;
	chdesc->befores_tail = &chdesc->befores;
	chdesc->afters = NULL;
	chdesc->afters_tail = &chdesc->afters;
	chdesc->weak_refs = NULL;
	memset(chdesc->nbefores, 0, sizeof(chdesc->nbefores));
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
	chdesc->flags = CHDESC_SAFE_AFTER;
	
	if(block)
	{
		/* add chdesc to block's befores */
		chdesc_link_all_changes(chdesc);
		chdesc_link_ready_changes(chdesc);
		
		/* make sure our block sticks around */
		bdesc_retain(block);
	}
	
	chdesc_free_push(chdesc);
	
	for(i = 0; i < nbefores; i++)
		/* it is convenient to allow NULL and written chdescs,
		   so make sure here to not add these as befores: */
		if(befores[i] && !(befores[i]->flags & CHDESC_WRITTEN))
			if((r = chdesc_add_depend_fast(chdesc, befores[i])) < 0)
			{
				chdesc_destroy(&chdesc);
				return r;
			}
	chdesc->flags &= ~CHDESC_SAFE_AFTER;
	*tail = chdesc;
	
	return 0;
}

#define STATIC_BEFORES_CAPACITY 10 /* 10 should cover most cases */

int chdesc_create_noop_list(bdesc_t * block, BD_t * owner, chdesc_t ** tail, ...)
{
	static chdesc_t * static_befores[STATIC_BEFORES_CAPACITY];
	chdesc_t ** befores;
	size_t nbefores = 0;
	size_t i;
	va_list ap;
	int r;
	
	va_start(ap, tail);
	while(va_arg(ap, chdesc_t *))
		nbefores++;
	va_end(ap);
	/* TODO: consider doing this instead of copying the array */
	/*
		va_start(ap, tail);
		&va_arg(ap, chdesc_t *)
	*/
	
	if(nbefores <= STATIC_BEFORES_CAPACITY)
		befores = static_befores;
	else
	{
		befores = smalloc(nbefores * sizeof(befores[0]));
		if(!befores)
			return -E_NO_MEM;
	}
	
	va_start(ap, tail);
	for(i = 0; i < nbefores; i++)
		befores[i] = va_arg(ap, chdesc_t *);
	va_end(ap);
	
	r = chdesc_create_noop_array(block, owner, tail, nbefores, befores);
	
	if(befores != static_befores)
		sfree(befores, nbefores * sizeof(befores[0]));
	return r;
}

static bool new_chdescs_require_data(const bdesc_t * block)
{
#if CHDESC_NRB
	/* Rule: When adding chdesc C to block B,
	 * and forall C' on B, with C' != C: C' has no afters on blocks != B,
	 * then C will never need to be rolled back. */
	return block->ddesc->extern_after_count > 0;
#else
	return 1;
#endif
}

/* chdesc merge stat tracking definitions */
#if CHDESC_NRB_MERGE_STATS
# define N_CHDESC_NRB_MERGE_STATS 3
static uint32_t chdesc_nrb_merge_stats[N_CHDESC_NRB_MERGE_STATS] = {0, 1, 2};
static unsigned chdesc_nrb_merge_stats_idx = -1;
static bool chdesc_nrb_merge_stats_callback_registered = 0;

static void print_chdesc_nrb_merge_stats(void * ignore)
{
	unsigned i;
	uint32_t nchdescs = 0;
	uint32_t nchdescs_notmerged = 0;
	(void) ignore;

	for(i = 0; i < N_CHDESC_NRB_MERGE_STATS; i++)
	{
		nchdescs += chdesc_nrb_merge_stats[i];
		if(i > 0)
			nchdescs_notmerged += chdesc_nrb_merge_stats[i];
	}
	
	printf("chdescs merge stats:\n");
	
	if(!nchdescs)
	{
		/* protect against divide by zero */
		printf("\tno chdescs created\n");
		return;
	}
	printf("\tmerged: %u (%3.1f%% all)\n", chdesc_nrb_merge_stats[0], 100 * ((float) chdesc_nrb_merge_stats[0]) / ((float) nchdescs));

	if(!nchdescs_notmerged)
	{
		/* protect against divide by zero */
		printf("\tall chdescs merged?!\n");
		return;
	}
	for(i = 1; i < N_CHDESC_NRB_MERGE_STATS; i++)
		printf("\tnot merged case %u: %u (%3.1f%% non-merged)\n", i, chdesc_nrb_merge_stats[i], 100 * ((float) chdesc_nrb_merge_stats[i]) / ((float) nchdescs_notmerged));
}

# include <kfs/kfsd.h>
static void chdesc_nrb_merge_stats_log(unsigned idx)
{
	if(!chdesc_nrb_merge_stats_callback_registered)
	{
		int r = kfsd_register_shutdown_module(print_chdesc_nrb_merge_stats, NULL, SHUTDOWN_POSTMODULES);
		if(r < 0)
			kpanic("kfsd_register_shutdown_module() = %i", r);
		chdesc_nrb_merge_stats_callback_registered = 1;
	}
	chdesc_nrb_merge_stats_idx = idx;
	chdesc_nrb_merge_stats[idx]++;
}
# define CHDESC_NRB_MERGE_STATS_LOG(_idx) chdesc_nrb_merge_stats_log(_idx)
#else
# define CHDESC_NRB_MERGE_STATS_LOG(_idx) do { } while(0)
#endif

#if CHDESC_NRB
/* Determine whether a new chdesc on 'block' and with the before 'before'
 * can be merged into an existing chdesc.
 * Return such a chdesc if so, else return NULL. */
static chdesc_t * select_chdesc_merger(const bdesc_t * block, const chdesc_t * before)
{
	if(new_chdescs_require_data(block))
	{
		/* rollbackable chdesc dep relations can be complicated, give up */
		CHDESC_NRB_MERGE_STATS_LOG(1);
		return NULL;
	}
	if(!block->ddesc->nrb)
	{
		CHDESC_NRB_MERGE_STATS_LOG(2);
		return NULL;
	}
	CHDESC_NRB_MERGE_STATS_LOG(0);
	assert(!(block->ddesc->nrb->flags & CHDESC_INFLIGHT));
	return block->ddesc->nrb;
}

static void chdesc_move_before_fast(chdesc_t * old_after, chdesc_t * new_after, chdesc_t * before)
{
	int r;
	chdesc_remove_depend(old_after, before);
	r = chdesc_add_depend_fast(new_after, before);
	assert(r >= 0); /* failure should be impossible */
}

/* Move chdesc's (transitive) befores that cannot reach merge_target
 * to be merge_target's befores, so that a merge into merge_target
 * maintains the needed befores. */
static void move_befores_for_merge(chdesc_t * chdesc, chdesc_t * merge_target)
{
	/* recursion-on-the-heap support */
	struct state {
		chdepdesc_t * dep;
		chdesc_t * chdesc;
		bool reachable;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;
	
	uint16_t saved_flags;
	
	chdepdesc_t * dep;
	bool reachable = 0; /* whether target is reachable from chdesc */
	
	saved_flags = merge_target->flags;
	merge_target->flags |= CHDESC_SAFE_AFTER;
	
  recurse_enter:
	/* Use CHDESC_MARKED to indicate merge_target is reachable */
	if(chdesc->flags & CHDESC_MARKED)
	{
		reachable = 1;
		goto recurse_return;
	}
	if(chdesc == merge_target)
	{
		chdesc->flags |= CHDESC_MARKED;
		reachable = 1;
		goto recurse_return;
	}
	if(chdesc_is_external(chdesc, merge_target->block))
		goto recurse_return;
	if(chdesc->type != NOOP)
	{
		/* Treat same-block, data chdescs as able to reach merge_target for
		 * move_befores_for_rb_merge(), which will take care of them. */
		chdesc->flags |= CHDESC_MARKED;
		reachable = 1;
		goto recurse_return;
	}
	
	/* discover the subset of befores that cannot reach target */
	/* TODO: do not scan a given dep->before.desc multiple times? */	
	for(dep = chdesc->befores; dep; dep = dep->before.next)
	{
		// Recursively move befores; equivalent to:
		// reachable |= move_befores_for_merge(dep->before.desc, merge_target)
		state->dep = dep;
		state->chdesc = chdesc;
		state->reachable = reachable;
		
		chdesc = dep->before.desc;
		reachable = 0;
		
		INCREMENT_STATE(state, static_states, states, states_capacity);
		goto recurse_enter;
			
	  recurse_resume:
		(void) 0; /* placate compiler re deprecated end labels */
	}
	
	/* if only some befores can reach target, move them.
	 * if none can reach target, caller will move chdesc. */
	if(reachable)
	{
		chdesc->flags |= CHDESC_MARKED;
		for(dep = chdesc->befores; dep; dep = dep->before.next)
			if(!(dep->before.desc->flags & CHDESC_MARKED))
				chdesc_move_before_fast(chdesc, merge_target, dep->before.desc);
		/* remove marks only after the preceding loop because of multipaths */
		for(dep = chdesc->befores; dep; dep = dep->before.next)
			dep->before.desc->flags &= ~CHDESC_MARKED;
	}
	
  recurse_return:
	if(state != &states[0])
	{
		state--;
		dep = state->dep;
		chdesc = state->chdesc;
		reachable |= state->reachable;
		goto recurse_resume;
	}
	
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));

	/* take care of the intial before */
	if(reachable)
		chdesc->flags &= ~CHDESC_MARKED;
	else
	{
		int r = chdesc_add_depend_fast(merge_target, chdesc);
		assert(r >= 0);
	}
	
	merge_target->flags = saved_flags;
}
#endif

#if CHDESC_MERGE_RBS_NRB
/* Return whether 'after' depends on any data chdesc on its block.
 * Requires 'bdesc' to have no external afters. */
static bool chdesc_has_block_befores(const chdesc_t * after, const bdesc_t * bdesc)
{
	chdepdesc_t * dep;
	for(dep = after->befores; dep; dep = dep->before.next)
	{
		const chdesc_t * before = dep->before.desc;
		if(chdesc_is_external(before, bdesc) || (before->flags & CHDESC_INFLIGHT))
			continue;
		if(before->type != NOOP)
			return 1;
		if(chdesc_has_block_befores(before, bdesc))
			return 1;
	}
	return 0;
}

/* Return the address of the chdesc containing the pointed to ddesc_next */
static chdesc_t * pprev2chdesc(chdesc_t ** chdesc_ddesc_pprev)
{
	return container_of(chdesc_ddesc_pprev, chdesc_t, ddesc_next);
}

/* Return a data chdesc on 'block' that has no before path to a chdesc
 * on its block. Return NULL if there are no data chdescs on 'block'. */
static chdesc_t * find_chdesc_without_block_befores(bdesc_t * block)
{
	/* The last data chdesc should be the oldest chdesc on 'block'
	 * and, since it is not an NRB, thus have no block befores */
	chdesc_t ** pprev = block->ddesc->all_changes_tail;
	chdesc_t * chdesc;
	for(; pprev != &block->ddesc->all_changes; pprev = chdesc->ddesc_pprev)
	{
		chdesc = pprev2chdesc(pprev);
		if(chdesc->type != NOOP && !(chdesc->flags & CHDESC_INFLIGHT))
		{
			assert(chdesc->type == BYTE || chdesc->type == BIT);
			assert(!chdesc_has_block_befores(chdesc, block));
			return chdesc;
		}
		if(chdesc == block->ddesc->all_changes)
			break;
	}
	return NULL;
}

/* Remove all block bit_changes befores */
static void empty_bit_changes(bdesc_t * block)
{
	hash_map_it_t it;
	chdesc_t * chdesc;
	if(!block->ddesc->bit_changes)
		return;
	hash_map_it_init(&it, block->ddesc->bit_changes);
	while((chdesc = hash_map_val_next(&it)))
	{
		chdesc_destroy(&chdesc);
		/* TODO: re-init is necessary since chdesc_destroy() removes chdesc,
		 * but it may be more efficient to not have to re-init... */
		hash_map_it_init(&it, block->ddesc->bit_changes);
	}
}

/* Merge all RBs on 'block' into a single NRB */
static void merge_rbs(bdesc_t * block)
{
# if CHDESC_MERGE_RBS_NRB_STATS
	static uint32_t ncalls = 0, nmerged_total = 0;
	uint32_t nmerged = 0;
# endif
	chdesc_t * merger, * chdesc, * next;
	int r;
	
	/* choose merger so that it does not depend on any other data chdescs
	 * on the block to simplify before merging */ 
	if(!(merger = find_chdesc_without_block_befores(block)))
		return;

	/* move the befores of each RB for their merge */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc->ddesc_next)
		if(chdesc != merger && chdesc->type != NOOP && !(chdesc->flags & CHDESC_INFLIGHT))
			move_befores_for_merge(chdesc, merger);
	
	/* convert RB merger into a NRB (except overlaps, done later) */
	if(merger->type == BYTE)
		free(merger->byte.data);
	else if(merger->type == BIT)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_BYTE, merger, 0, merger->owner->level);
# if COUNT_CHDESCS
		chdesc_counts[BIT]--;
		chdesc_counts[BYTE]++;
		dump_counts();
# endif
		merger->type = BYTE;
	}
	else
		assert(0);
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OFFSET, chdesc, 0);
	merger->byte.offset = 0;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_LENGTH, chdesc, block->ddesc->length);
	merger->byte.length = block->ddesc->length;
	merger->byte.data = NULL;
# if CHDESC_BYTE_SUM
	merger->byte.old_sum = 0;
	merger->byte.new_sum = 0;
# endif
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, merger, CHDESC_OVERLAP);
	merger->flags &= ~CHDESC_OVERLAP;
	assert(!block->ddesc->nrb);
	r = chdesc_weak_retain(merger, &block->ddesc->nrb);
	assert(r >= 0);
	
	/* convert non-merger data chdescs into noops so that pointers to them
	 * remain valid.
	 * TODO: could we destroy the noops with no afters after the runloop? */

	/* first: remove all overlaps in one run (for O(N) instead of O(N^2) time)
	 * and add merger to overlaps to complete NRB construction.
	 * this will remove deps on internal inflight chdescs, but that is ok. */
	empty_bit_changes(block);
	while(block->ddesc->overlaps)
	{
		assert(block->ddesc->overlaps->befores);
		chdesc_dep_remove(block->ddesc->overlaps->befores);
	}
	r = ensure_bdesc_has_overlaps(block);
	assert(r >= 0);
	r = chdesc_add_depend_fast(block->ddesc->overlaps, merger);
	assert(r >= 0);

	/* second: convert data chdescs into noops
	 * part a: unpropagate extern after counts (no more data chdesc afters)
	 * (do before rest of conversion to correctly (not) recurse) */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc->ddesc_next)
	{
		chdepdesc_t * dep;
		if(chdesc == merger || chdesc->type == NOOP || (chdesc->flags & CHDESC_INFLIGHT))
			continue;
		for(dep = chdesc->befores; dep; dep = dep->before.next)
		{
			chdesc_t * before = dep->before.desc;
			if(!before->block)
				propagate_extern_after_change_thru_noop_before(before, chdesc, 0);
			else if(chdesc_is_external(chdesc, before->block))
			{
				assert(before->block->ddesc->extern_after_count);
				before->block->ddesc->extern_after_count--;
			}
		}
	}
	/* second, part b: convert into noops */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = next)
	{
		uint16_t level;
		uint16_t flags;
		next = chdesc->ddesc_next;
		if(chdesc == merger || chdesc->type == NOOP || (chdesc->flags & CHDESC_INFLIGHT))
			continue;
		
# if CHDESC_MERGE_RBS_NRB_STATS
		nmerged++;
# endif
		
		/* ensure chdesc depends on merger. add dep prior to noop conversion
		 * to do correct level propagation inside chdesc_add_depend(). */
		flags = chdesc->flags;
		chdesc->flags |= CHDESC_SAFE_AFTER;
		r = chdesc_add_depend(chdesc, merger);
		assert(r >= 0);
		chdesc->flags = flags;
		
		chdesc_unlink_ready_changes(chdesc);
		chdesc_unlink_all_changes(chdesc);		
		if(chdesc->type == BYTE)
			free(chdesc->byte.data);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, chdesc);
# if COUNT_CHDESCS
		chdesc_counts[chdesc->type]--;
		chdesc_counts[NOOP]++;
		dump_counts();
# endif
		chdesc->type = NOOP;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_BLOCK, chdesc, NULL);
		bdesc_release(&chdesc->block);
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, chdesc, NULL);
		chdesc->owner = NULL;
		chdesc->noop.bit_changes = NULL;
		chdesc->noop.hash_key = NULL;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, chdesc, CHDESC_OVERLAP);
		chdesc->flags &= ~CHDESC_OVERLAP;
		
		if(merger->owner->level != (level = chdesc_level(chdesc)))
			propagate_level_change_thru_noop(chdesc, merger->owner->level, level);
	}
# if CHDESC_MERGE_RBS_NRB_STATS
	if(nmerged)
	{
		ncalls++;
		nmerged_total += nmerged;
		printf("%s(block %u). merged: %u now, %u total, %u avg.\n", __FUNCTION__, block->number, nmerged, nmerged_total, nmerged_total / ncalls);
	}
# endif
}
#endif

/* Attempt to merge into an existing chdesc instead of create a new chdesc.
 * Returns 1 on successful merge (*merged points merged chdesc),
 * 0 if no merge could be made, or < 0 upon error. */
static int chdesc_create_merge(bdesc_t * block, chdesc_t * before, chdesc_t ** merged)
{
#if CHDESC_NRB
	chdesc_t * merger;
# if CHDESC_MERGE_RBS_NRB
	if(!new_chdescs_require_data(block) && !block->ddesc->nrb)
		merge_rbs(block);
# endif
	if(!(merger = select_chdesc_merger(block, before)))
		return 0;
	if(before && !(before->flags & CHDESC_WRITTEN))
		move_befores_for_merge(before, merger);
	*merged = merger;
	return 1;
#else
	return 0;
#endif
}

#if CHDESC_MERGE_OVERLAP
/* A simple RB merge opportunity:
 * chdesc has no explicit befores and has a single overlap.
 * Returns 1 on successful merge (*head points to merged chdesc),
 * 0 if no merge could be made, or < 0 upon error. */
static int chdesc_create_merge_overlap(chdesc_t ** new, chdesc_t ** head)
{
	chdepdesc_t * dep;
	chdesc_t * overlap = NULL;
	uint16_t overlap_end, new_end, merge_offset, merge_length, merge_end;
	datadesc_t * ddesc = (*new)->block->ddesc;
	int r;
	
	/* determine whether we can merge new into an overlap */
	/* NOTE: if *head has a before and there are many overlaps, it may be
	 * be wise to check *head for befores before looking at overlaps */
	for(dep = (*new)->befores; dep; dep = dep->before.next)
	{
		chdesc_t * before = dep->before.desc;
		if(before->flags & CHDESC_WRITTEN)
			continue;
		if(chdesc_overlap_check(*new, before))
		{
			/* note: *new may overlap *head */
			if((before->flags & CHDESC_INFLIGHT) || before->type != BYTE)
				return 0;
			if(overlap && overlap != before)
				return 0;
			overlap = before;
		}
		else
		{
			assert(*head == before);
			if(before->befores)
				return 0;
		}
	}
	if(!overlap)
		return 0;
	
	overlap_end = overlap->byte.offset + overlap->byte.length;
	new_end = (*new)->byte.offset + (*new)->byte.length;
	merge_offset = MIN(overlap->byte.offset, (*new)->byte.offset);
	merge_length = MAX(overlap_end, new_end) - merge_offset;
	merge_end = merge_offset + merge_length;
	
	//printf("overlap merge %c (%u, %u)@%p to (%u, %u) for block %u\n", chdesc_is_rollbackable(overlap) ? ' ' : 'N', overlap->byte.offset, overlap->byte.length, overlap, merge_offset, merge_length, (*new)->block->number);
	
	if(*head && overlap != *head)
	{
		uint16_t flags = overlap->flags;
		overlap->flags |= CHDESC_SAFE_AFTER;
		if((r = chdesc_add_depend(overlap, *head)) < 0)
			return r;
		overlap->flags = flags;
	}
	
	/* Restore ddesc->overlaps if new completely overlapped.
	 * Restore before the size update to simplify error recovery. */
	if((*new)->flags & CHDESC_OVERLAP)
	{
		if((r = ensure_bdesc_has_overlaps(overlap->block)) < 0)
			return r;
		if((r = chdesc_add_depend_fast(ddesc->overlaps, overlap)) < 0)
			return r;
	}
	
	if(merge_offset != overlap->byte.offset || merge_length != overlap->byte.length)
	{
		/* handle updated data size change */
		void * merge_data;
		assert(chdesc_is_rollbackable(overlap));
		assert(!(overlap->flags & CHDESC_ROLLBACK));
		
		if(!(merge_data = malloc(merge_length)))
		{
			if((*new)->flags & CHDESC_OVERLAP)
				chdesc_remove_depend(ddesc->overlaps, overlap);
			return -E_NO_MEM;
		}
		memcpy(merge_data + overlap->byte.offset - merge_offset, overlap->byte.data, overlap->byte.length);
		if(merge_offset < overlap->byte.offset)
			memcpy(merge_data, &ddesc->data[merge_offset], overlap->byte.offset - merge_offset);
		if(overlap_end < merge_end)
			memcpy(merge_data + overlap_end - merge_offset, &ddesc->data[overlap_end], merge_end - overlap_end);
		free(overlap->byte.data);
		overlap->byte.data = merge_data;
		
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OFFSET, overlap, merge_offset);
		overlap->byte.offset = merge_offset;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_LENGTH, overlap, merge_length);
		overlap->byte.length = merge_length;
# if CHDESC_BYTE_SUM
		overlap->byte.old_sum = chdesc_byte_sum(overlap->byte.data, merge_length);
		overlap->byte.new_sum = chdesc_byte_sum(&ddesc->data[merge_offset], merge_length);
# endif
	}
	
	memcpy(&ddesc->data[(*new)->byte.offset], (*new)->byte.data, (*new)->byte.length);

	chdesc_destroy(new);
	*head = overlap;
	return 1;
}
#endif

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
	
	r = chdesc_create_merge(block, *head, head);
	if(r < 0)
		return r;
	else if(r == 1)
	{
		if(data)
			memcpy(&block->ddesc->data[offset], data, length);
		else
			memset(&block->ddesc->data[offset], 0, length);
		return 0;
	}
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return -E_NO_MEM;
	
	chdesc->owner = owner;		
	chdesc->block = block;
	chdesc->type = BYTE;
	if(data_required)
	{
		chdesc->byte.offset = offset;
		chdesc->byte.length = length;
	}
	else
	{
		/* Expand to cover entire block. This is safe since all chdescs on
		 * this block at least implicitly have all nonrollbackables as befores.
		 * Leave 'offset' and 'length' as is to copy source data. */
		chdesc->byte.offset = 0;
		chdesc->byte.length = block->ddesc->length;
	}
	
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
	
	chdesc->befores = NULL;
	chdesc->befores_tail = &chdesc->befores;
	chdesc->afters = NULL;
	chdesc->afters_tail = &chdesc->afters;
	chdesc->weak_refs = NULL;
	memset(chdesc->nbefores, 0, sizeof(chdesc->nbefores));
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
	chdesc->flags = CHDESC_ROLLBACK | CHDESC_SAFE_AFTER;
		
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BYTE, chdesc, block, owner, chdesc->byte.offset, chdesc->byte.length);
#if COUNT_CHDESCS
	chdesc_counts[BYTE]++;
	dump_counts();
#endif
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	chdesc_link_all_changes(chdesc);
	chdesc_link_ready_changes(chdesc);
	
	/* this is a new chdesc, so we don't need to check for loops.
	 * but we should check to make sure head has not already been written. */
	if(*head && !((*head)->flags & CHDESC_WRITTEN))
		if((r = chdesc_add_depend_fast(chdesc, *head)) < 0)
		{
			chdesc_destroy(&chdesc);
			return r;
		}
	
	/* make sure it is after upon any pre-existing chdescs */
	if((r = chdesc_overlap_multiattach(chdesc, block)) < 0)
	{
		chdesc_destroy(&chdesc);
		return r;
	}
	
#if CHDESC_MERGE_OVERLAP
	/* after the above work towards chdesc to avoid multiple overlap scans */
	if(data_required)
	{
		if((r = chdesc_create_merge_overlap(&chdesc, head)) < 0)
		{
			chdesc_destroy(&chdesc);
			return r;
		}
		else if(r == 1)
			return 0;
	}
#endif
	
	if((r = ensure_bdesc_has_overlaps(block)) < 0)
	{
		chdesc_destroy(&chdesc);
		return r;
	}
	if((r = chdesc_add_depend_fast(block->ddesc->overlaps, chdesc)) < 0)
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
#if CHDESC_NRB
		if(data)
			memcpy(&chdesc->block->ddesc->data[offset], data, length);
		else
			memset(&chdesc->block->ddesc->data[offset], 0, length);
		chdesc->flags &= ~CHDESC_ROLLBACK;
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_APPLY, chdesc);
		assert(!block->ddesc->nrb);
		if((r = chdesc_weak_retain(chdesc, &block->ddesc->nrb)) < 0)
		{
			chdesc_destroy(&chdesc);
			return r;
		}
#else
		assert(0);
#endif
	}
	
	chdesc->flags &= ~CHDESC_SAFE_AFTER;
	*head = chdesc;
	block->ddesc->synthetic = 0;
	
	return 0;
}

int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	if(&block->ddesc->data[offset] == data)
		kpanic("Cannot create a change descriptor in place!");
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
	
	r = chdesc_create_merge(block, *head, head);
	if(r < 0)
		return r;
	else if(r == 1)
	{
		((uint32_t *) block->ddesc->data)[offset] ^= xor;
		return 0;
	}
	
	if(!data_required)
	{
		uint32_t data = ((uint32_t *) block->ddesc->data)[offset] ^ xor;
#if CHDESC_NRB_MERGE_STATS
		chdesc_nrb_merge_stats[chdesc_nrb_merge_stats_idx]--; /* don't double count */
#endif
		return _chdesc_create_byte(block, owner, offset * 4, 4, (uint8_t *) &data, head);
	}
	
	chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return -E_NO_MEM;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CREATE_BIT, chdesc, block, owner, offset, xor);
#if COUNT_CHDESCS
	chdesc_counts[BIT]++;
	dump_counts();
#endif
	
	chdesc->owner = owner;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	chdesc->befores = NULL;
	chdesc->befores_tail = &chdesc->befores;
	chdesc->afters = NULL;
	chdesc->afters_tail = &chdesc->afters;
	chdesc->weak_refs = NULL;
	memset(chdesc->nbefores, 0, sizeof(chdesc->nbefores));
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
	chdesc->flags = CHDESC_ROLLBACK | CHDESC_SAFE_AFTER;

	chdesc_link_ready_changes(chdesc);
	
	/* make sure it is after upon any pre-existing chdescs */
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
	
	/* add chdesc to block's befores */
	chdesc_link_all_changes(chdesc);
	if(!(bit_changes = ensure_bdesc_has_bit_changes(block, offset)))
	{
		r = -E_NO_MEM;
		goto error;
	}
	if((r = chdesc_add_depend_fast(bit_changes, chdesc)) < 0)
		goto error;
	
	chdesc->flags &= ~CHDESC_SAFE_AFTER;
	*head = chdesc;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	block->ddesc->synthetic = 0;
	
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
/* NOTE: chdesc_create_byte() with NRBs could likely replace this function if
 * more general merging were supported (eg merge if no additional befores). */
int chdesc_rewrite_byte(chdesc_t * chdesc, uint16_t offset, uint16_t length, void * data)
{
	/* sanity checks */
	if(chdesc->type != BYTE)
		return -E_INVAL;
	if(offset + length > chdesc->byte.offset + chdesc->byte.length)
		return -E_INVAL;
	
	/* scan for overlapping chdescs - they will all have us as a before, or at
	 * least, if there are any, at least one will have us as a direct before */
	if(chdesc->afters)
	{
		chdepdesc_t * dep;
		for(dep = chdesc->afters; dep; dep = dep->after.next)
		{
			/* no block? doesn't overlap */
			if(!dep->after.desc->block)
				continue;
			/* not the same block? doesn't overlap */
			if(dep->after.desc->block->ddesc != chdesc->block->ddesc)
				continue;
			/* chdesc_overlap_check doesn't check that the block is
			 * the same, which is why we just checked it by hand */
			if(!chdesc_overlap_check(dep->after.desc, chdesc))
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
static int chdesc_has_before(chdesc_t * after, chdesc_t * before)
{
	/* recursion-on-the-heap support */
	struct state {
		chdesc_t * after;
		chdepdesc_t * dep;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;
	
	int has_before = 0;
	chdepdesc_t * dep;
	
  recurse_enter:
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, after, CHDESC_MARKED);
	after->flags |= CHDESC_MARKED;
	for(dep = after->befores; dep; dep = dep->before.next)
	{
		if(dep->before.desc == before)
		{
			has_before = 1;
			goto recurse_done;
		}
		if(!(dep->before.desc->flags & CHDESC_MARKED))
		{
			/* Recursively check befores; equivalent to:
			 * if(chdesc_has_before(dep->before.desc, before))
			 *	return 1; */
			state->after = after;
			state->dep = dep;
			
			after = dep->before.desc;
			
			INCREMENT_STATE(state, static_states, states, states_capacity);
			goto recurse_enter;
			
		  recurse_resume:
			(void) 0; /* placate compiler re deprecated end labels */
		}
	}
	
	if(state != &states[0])
	{
		state--;
		after = state->after;
		dep = state->dep;
		/* the chdesc graph is a DAG, so unmarking here would defeat the purpose */
		goto recurse_resume;
	}
	
  recurse_done:
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
	return has_before;
}
#endif

/* add a dependency between change descriptors */
int chdesc_add_depend(chdesc_t * after, chdesc_t * before)
{
	/* compensate for Heisenberg's uncertainty principle */
	if(!after || !before)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided use of NULL pointer!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	/* make sure we're not fiddling with chdescs that are already written */
	if(after->flags & CHDESC_WRITTEN)
	{
		if(before->flags & CHDESC_WRITTEN)
			return 0;
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Attempt to add before to already written data!\n", __FUNCTION__, __FILE__, __LINE__);
		return -E_INVAL;
	}
	if(before->flags & CHDESC_WRITTEN)
		return 0;
	if(after->flags & CHDESC_INFLIGHT)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Adding non-written before to in flight after!\n", __FUNCTION__, __FILE__, __LINE__);
	
	/* avoid creating a dependency loop */
#if CHDESC_CYCLE_CHECK
	if(after == before || chdesc_has_before(before, after))
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		assert(0);
		return -E_INVAL;
	}
	/* chdesc_has_before() marks the DAG rooted at "before" so we must unmark it */
	chdesc_unmark_graph(before);
#endif
	
	return chdesc_add_depend_fast(after, before);
}

static void chdesc_dep_remove(chdepdesc_t * dep)
{
	propagate_depend_remove(dep->after.desc, dep->before.desc);
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_BEFORE, dep->after.desc, dep->before.desc);
	*dep->before.ptr = dep->before.next;
	if(dep->before.next)
		dep->before.next->before.ptr = dep->before.ptr;
	else
		dep->after.desc->befores_tail = dep->before.ptr;
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_REM_AFTER, dep->before.desc, dep->after.desc);
	*dep->after.ptr = dep->after.next;
	if(dep->after.next)
		dep->after.next->after.ptr = dep->after.ptr;
	else
		dep->before.desc->afters_tail = dep->after.ptr;
	
	if(dep->after.desc->type == NOOP && !dep->after.desc->befores)
		/* we just removed the last before of a NOOP chdesc, so satisfy it */
		chdesc_satisfy(&dep->after.desc);
	
	memset(dep, 0, sizeof(*dep));
	free(dep);
}

void chdesc_remove_depend(chdesc_t * after, chdesc_t * before)
{
	chdepdesc_t * scan_befores = after->befores;
	chdepdesc_t * scan_afters = before->afters;
	while(scan_befores && scan_afters &&
	      scan_befores->before.desc != before &&
	      scan_afters->after.desc != after)
	{
		scan_befores = scan_befores->before.next;
		scan_afters = scan_afters->after.next;
	}
	if(scan_befores && scan_befores->before.desc == before)
		chdesc_dep_remove(scan_befores);
	else if(scan_afters && scan_afters->after.desc == after)
		chdesc_dep_remove(scan_afters);
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
			kdprintf(STDERR_FILENO, "%s: (%s:%d): dangling chdesc weak reference! (of %p)\n", __FUNCTION__, __FILE__, __LINE__, chdesc);
			chdesc->weak_refs = next->next;
			free(next);
		}
	}
}

/* satisfy a change descriptor, i.e. remove it from all afters and add it to the list of written chdescs */
int chdesc_satisfy(chdesc_t ** chdesc)
{
	if((*chdesc)->flags & CHDESC_WRITTEN)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfaction of already satisfied chdesc!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_INFO, KDB_CHDESC_SATISFY, *chdesc);
	
	if((*chdesc)->befores)
	{
		chdesc_t * bit_changes;
		/* We are trying to satisfy a chdesc with befores, which
		 * can happen if we have modules generating out-of-order chdescs
		 * but no write-back caches. We need to convert it to a NOOP so
		 * that any of its afters will still have the befores of this
		 * chdescs as indirect befores. However, we
		 * still need to collect any weak references to it in case
		 * anybody was watching it to see when it got satisfied. */
		if((*chdesc)->type != NOOP)
			kdprintf(STDERR_FILENO, "%s(): (%s:%d): satisfying chdesc %p of type %d with befores!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc, (*chdesc)->type);
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
#if COUNT_CHDESCS
				chdesc_counts[BYTE]--;
				chdesc_counts[NOOP]++;
				dump_counts();
#endif
				break;
			case BIT:
				bit_changes = chdesc_bit_changes((*chdesc)->block, (*chdesc)->bit.offset);
				assert(bit_changes);
				chdesc_remove_depend(bit_changes, *chdesc);
				(*chdesc)->type = NOOP;
				KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CONVERT_NOOP, *chdesc);
				/* fall through */
#if COUNT_CHDESCS
				chdesc_counts[BIT]--;
				chdesc_counts[NOOP]++;
				dump_counts();
#endif
			case NOOP:
				break;
			default:
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): unexpected chdesc of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*chdesc)->type);
				return -E_INVAL;
		}
		
	}
	else
	{
		while((*chdesc)->afters)
			chdesc_dep_remove((*chdesc)->afters);
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
	if(*location && *location == chdesc)
	{
		chrefdesc_t * ref = chdesc->weak_refs;
		for(; ref; ref = ref->next)
			if(ref->desc == location)
				return 0;
	}
	
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
		assert(!(*chdesc)->afters && !(*chdesc)->befores);
		if(free_head == *chdesc || (*chdesc)->free_prev)
			chdesc_free_remove(*chdesc);
	}
	else
	{
		if((*chdesc)->type != NOOP)
		{
			if((*chdesc)->afters)
			{
				/* this is perfectly allowed, but while we are switching to this new system, print a warning */
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
				if((*chdesc)->flags & CHDESC_OVERLAP)
					kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying completely overlapping unwritten chdesc: %p!\n", __FUNCTION__, __FILE__, __LINE__, *chdesc);
			}
		}
		else if(free_head == *chdesc || (*chdesc)->free_prev)
		{
			assert(!(*chdesc)->befores);
			chdesc_free_remove(*chdesc);
		}
	}
	
	if((*chdesc)->befores && (*chdesc)->afters)
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): destroying chdesc with both afters and befores! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, KFS_DEBUG_COUNT());
	/* remove befores first, so chdesc_satisfy() won't just turn it to a NOOP */
	while((*chdesc)->befores)
		chdesc_dep_remove((*chdesc)->befores);
	if((*chdesc)->afters)
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
			/* chdesc_satisfy() does free 'data', but error cases may not */
			free((*chdesc)->byte.data);
			(*chdesc)->byte.data = NULL;
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
	
#if COUNT_CHDESCS
	chdesc_counts[(*chdesc)->type]--;
	dump_counts();
#endif
	memset(*chdesc, 0, sizeof(**chdesc));
	free(*chdesc);
	*chdesc = NULL;
}

void chdesc_claim_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->befores);
	assert(chdesc_before_level(chdesc) == BDLEVEL_NONE);
	if(chdesc->free_prev || free_head == chdesc)
		chdesc_free_remove(chdesc);
}

void chdesc_autorelease_noop(chdesc_t * chdesc)
{
	assert(chdesc->type == NOOP && !chdesc->befores && !(chdesc->flags & CHDESC_WRITTEN));
	assert(chdesc_before_level(chdesc) == BDLEVEL_NONE);
	while(chdesc->afters)
		chdesc_dep_remove(chdesc->afters);
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
