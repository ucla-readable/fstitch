/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/pool.h>

#include <fscore/debug.h>
#include <fscore/bdesc.h>
#include <fscore/fstitchd.h>
#include <fscore/revision.h>
#include <fscore/patch.h>

/* Set to print out patch cycles when they are discovered by
 * PATCH_CYCLE_CHECK. */
#define PATCH_CYCLE_PRINT 1

/* Set to count patches by type and display periodic output */
#define COUNT_PATCHES 0
/* Set for count to be a total instead of the current */
#define COUNT_PATCHES_IS_TOTAL 0

/* Patch multigraphs allow more than one dependency between the same
 * two patches. This currently saves us the trouble of making sure we
 * don't create a duplicate dependency between patches, though it also causes us
 * to allocate somewhat more memory in many cases where we would otherwise
 * detect the duplicate dependency. Allowing multigraphs results in a reasonable
 * speedup, even though we use more memory, so it is enabled by default. */
#define PATCH_ALLOW_MULTIGRAPH 1

/* Set to track the nrb patch merge stats and print them after shutdown */
#define PATCH_NRB_MERGE_STATS (PATCH_NRB && 0)

/* Set to merge all existing RBs into a NRB when creating a NRB on the block */
#define PATCH_MERGE_RBS_NRB PATCH_RB_NRB_READY
/* Set to track RBs->NRB merge stats */
#define PATCH_MERGE_RBS_NRB_STATS (PATCH_MERGE_RBS_NRB && 0)

/* Set to merge a simple overlapping RB into the underlying patch */
#define PATCH_BYTE_MERGE_OVERLAP 1
#define PATCH_BIT_MERGE_OVERLAP 1
#define PATCH_OVERLAPS2 (PATCH_BYTE_MERGE_OVERLAP && 1)

/* Set to allow swapping of full-block byte data with pointers instead of memxchg() */
#define SWAP_FULLBLOCK_DATA 0

#if SWAP_FULLBLOCK_DATA && !REVISION_TAIL_INPLACE
#error SWAP_FULLBLOCK_DATA is incompatible with !REVISION_TAIL_INPLACE
#endif

/* Set to enable patch accounting */
#define PATCH_ACCOUNT 0

/* Allow malloc in recursion-on-the-heap support */
#define HEAP_RECURSION_ALLOW_MALLOC 0

#if PATCH_ACCOUNT
#ifdef __KERNEL__
#include <asm/tsc.h>
#else
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles(void)
{
	cycles_t ret;
#ifdef __i386__
	__asm__ __volatile__("rdtsc" : "=A" (ret));
#else
# warning get_cycles() will return 0 on this architecture
	ret = 0;
#endif
	return ret;
}
#endif

static inline uint64_t u64_diff(uint64_t start, uint64_t end)
{
	if(start <= end)
		return end - start;
	return ULLONG_MAX - end + start;
}

typedef struct account {
	const char * name;
	size_t size;
	bool valid_space_time;
	uint64_t space_time; /* total 'space * time' */
	uint64_t space_total; /* total allocated */
	uint64_t space_total_realloc; /* total allocated, minus realloc effect */
	uint32_t space_max, space_last;
	cycles_t time_first, time_last;
} account_t;

static inline void account_init(const char * name, size_t size, account_t * act)
{
	act->name = name;
	act->size = size;
	act->valid_space_time = 1;
	act->space_time = 0;
	act->space_total = 0;
	act->space_total_realloc = 0;
	act->space_max = act->space_last = 0;
	act->time_first = act->time_last = 0;
}

static inline void account_update_realloc(account_t * act, int32_t prev_space_change, int32_t space_change)
{
	cycles_t time_current = get_cycles();
	uint64_t time_diff = u64_diff(act->time_last, time_current);
	uint64_t spacetime_prev = act->space_time;
	
	if(!act->time_first)
		act->time_first = act->time_last = get_cycles();
	
	act->space_time += act->space_last * time_diff;
	if(act->space_time < spacetime_prev)
		act->valid_space_time = 0;
	act->space_last += space_change;
	act->time_last = time_current;
	if(act->space_last > act->space_max)
		act->space_max = act->space_last;
	if(space_change > 0)
	{
		act->space_total += space_change; /* FIXME: sort of */
		act->space_total_realloc += space_change - prev_space_change; /* FIXME: sort of (?) */
	}	
}

static inline void account_update(account_t * act, int32_t space_change)
{
	return account_update_realloc(act, 0, space_change);
}

static account_t act_npatches[6], act_ndeps;
static account_t act_data;
//static account_t act_nnrb;

#define NC_CONVERT_BIT_BYTE 3
#define NC_CONVERT_EMPTY 4
#define NC_TOTAL 5
static account_t * act_all[] =
    { &act_npatches[BIT], &act_npatches[BYTE], &act_npatches[EMPTY],
      &act_npatches[NC_CONVERT_BIT_BYTE], &act_npatches[NC_CONVERT_EMPTY],
      &act_npatches[NC_TOTAL],
      &act_ndeps, &act_data };

static inline void account_npatches(int type, int add)
{
	account_update(&act_npatches[type], add);
	account_update(&act_npatches[NC_TOTAL], add);
}

static inline void account_npatches_undo(int type)
{
	/* count undone in 'total space'? (do not decrement space_total) */
	account_update(&act_npatches[type], -1);
	act_npatches[type].space_total--;
	act_npatches[type].space_total_realloc--;
	account_update(&act_npatches[NC_TOTAL], -1);
	act_npatches[NC_TOTAL].space_total--;
	act_npatches[NC_TOTAL].space_total_realloc--;
}

static inline void account_npatches_convert(int type_old, int type_new)
{
	account_update(&act_npatches[type_old], -1);
	account_update(&act_npatches[type_new], 1);
	if(type_old == BIT && type_new == BYTE)
		account_update(&act_npatches[NC_CONVERT_BIT_BYTE], 1);
	else if(type_new == EMPTY)
		account_update(&act_npatches[NC_CONVERT_EMPTY], 1);
	else
		assert(0);
}

static uint64_t do_div64(uint64_t n, uint64_t base)
{
	uint64_t count = 0;
	uint64_t prev;
	if(!n)
		return 0;
	if(!base)
		return ULLONG_MAX;
	do {
		prev = n;
		n -= base;
		count++;
	} while (n <= prev);
	return count - 1;
}

static void account_print(const account_t * act)
{
	printf("account: %s: mean=", act->name);
	if(act->valid_space_time)
	{
		uint64_t mean = do_div64(act->space_time, u64_diff(act->time_first, act->time_last));
		printf("%llu", mean);
	}
	else
		printf("-1");
	printf(" max=%u total=%llu total_realloc=%llu sizeof=%u\n", act->space_max, act->space_total, act->space_total_realloc, act->size);
}

static void account_print_all(void * ignore)
{
	int i;
	for(i = 0; i < sizeof(act_all) / sizeof(*act_all); i++)
		account_print(act_all[i]);
}

static int account_init_all(void)
{
	account_init("npatches (byte)", sizeof(patch_t), &act_npatches[BYTE]);
	account_init("npatches (bit)", sizeof(patch_t), &act_npatches[BIT]);
	account_init("npatches (empty)", sizeof(patch_t), &act_npatches[EMPTY]);
	account_init("npatches (bit->byte)", 0, &act_npatches[NC_CONVERT_BIT_BYTE]);
	account_init("npatches (->empty)", 0, &act_npatches[NC_CONVERT_EMPTY]);
	account_init("npatches (total)", sizeof(patch_t), &act_npatches[NC_TOTAL]);
	//account_init("nnrb", &act_nnrb);
	account_init("data", 1, &act_data);
	account_init("ndeps", sizeof(patchdep_t), &act_ndeps);
	return fstitchd_register_shutdown_module(account_print_all, NULL, SHUTDOWN_POSTMODULES);
}
#else
# define account_init(x) do {} while(0)
# define account_update_realloc(act, sc, ad) do {} while(0)
# define account_update(act, sc) do {} while(0)
# define account_npatches(type, add) do {} while (0)
# define account_npatches_undo(type) do {} while (0)
# define account_npatches_convert(type_old, type_new) do {} while (0)
# define account_init_all() 0
#endif

DECLARE_POOL(patch, patch_t);
DECLARE_POOL(patchdep, patchdep_t);

static void patchpools_free_all(void * ignore)
{
	patch_free_all();
	patchdep_free_all();
}


#if COUNT_PATCHES
#include <lib/jiffies.h>
/* indices match patch->type */
static uint32_t patch_counts[3];
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
		printf("Bit: %4d, Byte: %4d, Empty: %4d\n", patch_counts[BIT], patch_counts[BYTE], patch_counts[EMPTY]);
	}
}
#endif


static __inline void patch_free_byte_data(patch_t *patch) __attribute((always_inline));
static __inline void patch_free_byte_data(patch_t *patch)
{
	assert(patch->type == BYTE);
	if(patch->length > PATCH_LOCALDATA && patch->byte.data)
	{
		free(patch->byte.data);
		account_update(&act_data, -patch->length);
	}
}


static patch_t * free_head = NULL;

static void patch_free_push(patch_t * patch)
{
	assert(free_head != patch && !patch->free_prev);
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_NEXT, patch, free_head);
	patch->free_next = free_head;
	if(free_head)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_PREV, free_head, patch);
		free_head->free_prev = patch;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_HEAD, patch);
	free_head = patch;
}

static void patch_free_remove(patch_t * patch)
{
	assert(patch->free_prev || free_head == patch);
	if(patch->free_prev)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_NEXT, patch->free_prev, patch->free_next);
		patch->free_prev->free_next = patch->free_next;
	}
	else
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_HEAD, patch->free_next);
		free_head = patch->free_next;
	}
	if(patch->free_next)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_PREV, patch->free_next, patch->free_prev);
		patch->free_next->free_prev = patch->free_prev;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_PREV, patch, NULL);
	patch->free_prev = NULL;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FREE_NEXT, patch, NULL);
	patch->free_next = NULL;
}

static inline int patch_overlap_list(const patch_t *c)
{
	int sz = c->block->length >> OVERLAP1SHIFT;
	if (c->length == 0)
		return -1;
	if ((c->offset ^ (c->offset + c->length - 1)) & ~(sz - 1))
		return 0;
	return c->offset / sz + 1;
}

static inline void patch_link_overlap(patch_t *patch)
{
	assert(patch->type == BYTE);
	assert(!patch->overlap_pprev && !patch->overlap_next);
	int list = patch_overlap_list(patch);
	assert(list >= 0);
	patch->overlap_pprev = &patch->block->overlap1[list];
	patch->overlap_next = *patch->overlap_pprev;
	*patch->overlap_pprev = patch;
	if(patch->overlap_next)
		patch->overlap_next->overlap_pprev = &patch->overlap_next;
}

static inline void patch_unlink_overlap(patch_t *patch)
{
	assert((!patch->overlap_pprev && !patch->overlap_next) || patch->block);
	if(patch->overlap_pprev)
		*patch->overlap_pprev = patch->overlap_next;
	if(patch->overlap_next)
		patch->overlap_next->overlap_pprev = patch->overlap_pprev;
	patch->overlap_next = NULL;
	patch->overlap_pprev = NULL;
}

/* ensure bdesc->bit_patches[offset] has a empty patch */
static patch_t * ensure_bdesc_has_bit_patches(bdesc_t * block, uint16_t offset)
{
	patch_t * patch;
	void * key = (void *) (uint32_t) (offset << 2);
	int r;
	assert(block);
	
	if(!block->bit_patches)
	{
		block->bit_patches = hash_map_create();
		if(!block->bit_patches)
			return NULL;
	}
	
	patch = (patch_t *) hash_map_find_val(block->bit_patches, key);
	if(patch)
	{
		assert(patch->type == EMPTY);
		return patch;
	}
	
	r = patch_create_empty_list(NULL, &patch, NULL);
	if(r < 0)
		return NULL;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "bit_patches");
	
	if(hash_map_insert(block->bit_patches, key, patch) < 0)
	{
		patch_destroy(&patch);
		return NULL;
	}
	
	/* we don't really need a flag for this, since we could just use the
	 * empty.bit_patches field to figure it out... but that would be error-prone */
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, patch, PATCH_BIT_EMPTY);
	patch->flags |= PATCH_BIT_EMPTY;
	patch->empty.bit_patches = block->bit_patches;
	patch->empty.hash_key = key;
	
	return patch;
}

/* get bdesc->bit_patches[offset] */
static patch_t * patch_bit_patches(bdesc_t * block, uint16_t offset)
{
	if(!block->bit_patches)
		return NULL;
	return hash_map_find_val(block->bit_patches, (void *) (uint32_t) offset);
}

#if HEAP_RECURSION_ALLOW_MALLOC
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
				kpanic("smalloc/srealloc(%u bytes) failed", (unsigned) (states_capacity * sizeof(states))); \
			state = &states[next_index]; \
		} \
	} while(0)
#else
/* Helper macro for recursion-on-the-heap support */
#define INCREMENT_STATE(state, static_states, states, states_capacity) \
	do { \
		size_t next_index = 1 + state - &states[0]; \
		if(next_index < states_capacity) \
			state++; \
		else \
			kpanic("recursion-on-the-heap needs %u bytes!", (unsigned) (states_capacity * sizeof(*states))); \
	} while(0)
#endif

#define STATIC_STATES_CAPACITY 1024 /* 1024 is fairly arbitrary */

/* propagate a level change through the empty after,
 * to update the ready state */
static void propagate_level_change_thru_empty(patch_t * empty_after, uint16_t prev_level, uint16_t new_level)
{
	/* recursion-on-the-heap support */
	struct state {
		patchdep_t * emptys_afters;
		uint16_t prev_level;
		uint16_t new_level;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;

  recurse_enter:
	assert(!empty_after->owner);
	assert(prev_level != new_level);
	assert(prev_level != BDLEVEL_NONE || new_level != BDLEVEL_NONE);

	patchdep_t * emptys_afters = empty_after->afters;
	for(; emptys_afters; emptys_afters = emptys_afters->after.next)
	{
		patch_t * after = emptys_afters->after.desc;
		uint16_t after_prev_level = patch_level(after);

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
		patch_update_ready_patches(after);

		if(!after->owner)
		{
			uint16_t after_new_level = patch_level(after);
			if(after_prev_level != after_new_level)
			{
				/* Recursively propagate the level change; equivalent to
				 * propagate_level_change_thru_empty
				 *  (after, after_prev_level, after_new_level) */
				state->emptys_afters = emptys_afters;
				state->prev_level = prev_level;
				state->new_level = new_level;

				empty_after = after;
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
		emptys_afters = state->emptys_afters;
		prev_level = state->prev_level;
		new_level = state->new_level;
		goto recurse_resume;
	}

	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
}

#if BDESC_EXTERN_AFTER_COUNT
/* return whether 'patch' is on a different block than 'block' */
static inline bool patch_is_external(const patch_t * patch, const bdesc_t * block)
{
	assert(patch);
	assert(block);
	if(patch->type == EMPTY)
	{
		if(patch->block && patch->block != block)
			return 1;
	}
	else if(patch->block != block)
		return 1;
	return 0;
}

#define BDESC_EXTERN_AFTER_COUNT_DEBUG 0
#if BDESC_EXTERN_AFTER_COUNT_DEBUG
/* return the number of external afters 'patch' has with respect
 * to 'block' */
static bool count_patch_external_afters(const patch_t * patch, const bdesc_t * block)
{
	const patchdep_t * afters;
	uint32_t n = 0;
	for(afters = patch->afters; afters; afters = afters->after.next)
	{
		const patch_t * after = afters->after.desc;
		if(!after->block)
		{
			/* XXX: stack usage */
			n += count_patch_external_afters(after, block);
		}
		else if(after->block != block)
			n++;
	}
	return n;
}

/* return the number of external afters for 'block' */
static uint32_t count_bdesc_external_afters(const bdesc_t * block)
{
	const patch_t * c;
	uint32_t n = 0;
	for(c = block->all_patches; c; c = c->ddesc_next)
		if(!(c->flags & PATCH_INFLIGHT))
			n += count_patch_external_afters(c, block);
	return n;
}

/* return whether the external after count in 'block' agrees with
 * an actual count */
static bool extern_after_count_is_correct(const bdesc_t * block)
{
	return !block || (count_bdesc_external_afters(block) == block->extern_after_count);
}
#endif /* BDESC_EXTERN_AFTER_COUNT_DEBUG */

/* propagate a depend add/remove through a empty after,
 * to increment/decrement extern_after_count for 'block' */
static void propagate_extern_after_change_thru_empty_after(const patch_t * empty_after, bdesc_t * block, bool add)
{
	patchdep_t * dep;
	assert(empty_after->type == EMPTY && !empty_after->block);
	assert(block);
	for(dep = empty_after->afters; dep; dep = dep->after.next)
	{
		patch_t * after = dep->after.desc;
		if(!after->block)
		{
			assert(after->type == EMPTY);
			/* XXX: stack usage */
			propagate_extern_after_change_thru_empty_after(after, block, add);
		}
		else if(patch_is_external(after, block))
		{
			if(add)
			{
				block->extern_after_count++;
				assert(block->extern_after_count);
			}
			else
			{
				assert(block->extern_after_count);
				block->extern_after_count--;
			}
		}
	}
}

/* propagate a depend add/remove through a empty before,
 * to increment extern_after_count for before's block */
static void propagate_extern_after_change_thru_empty_before(patch_t * empty_before, const patch_t * after, bool add)
{
	patchdep_t * dep;
	assert(empty_before->type == EMPTY && !empty_before->block);
	assert(after->type != EMPTY);
	for(dep = empty_before->befores; dep; dep = dep->before.next)
	{
		patch_t * before = dep->before.desc;
		if(!before->block)
		{
			assert(before->type == EMPTY);
			/* XXX: stack usage */
			propagate_extern_after_change_thru_empty_before(before, after, add);
		}
		else if(patch_is_external(after, before->block) && !(before->flags & PATCH_INFLIGHT))
		{
			if(add)
			{
				before->block->extern_after_count++;
				assert(before->block->extern_after_count);
			}
			else
			{
				assert(before->block->extern_after_count);
				before->block->extern_after_count--;
			}
		}
	}
}

/* Return whether patch has any afters that are on a block */
static bool has_block_afters(const patch_t * patch)
{
	const patchdep_t * dep;
	for(dep = patch->afters; dep; dep = dep->after.next)
	{
		if(dep->after.desc->block)
			return 1;
		/* XXX: stack usage */
		else if(has_block_afters(dep->after.desc))
			return 1;
	}
	return 0;
}

/* Return whether patch has any befores that are on a block */
static bool has_block_befores(const patch_t * patch)
{
	const patchdep_t * dep;
	for(dep = patch->befores; dep; dep = dep->before.next)
	{
		if(dep->before.desc->block)
			return 1;
		/* XXX: stack usage */
		else if(has_block_befores(dep->before.desc))
			return 1;
	}
	return 0;
}

/* propagate extern_after_count changes for a depend add/remove */
static inline void propagate_extern_after_change(patch_t * after, patch_t * before, bool add)
{
	if(!after->block)
	{
		if(before->block)
			propagate_extern_after_change_thru_empty_after(after, before->block, add);
		else if(after->afters && before->befores)
		{
			/* If both after and before are emptys and after has an on-block
			 * after and before an on-block before then we need to update the
			 * extern after count for each of before's on-block befores,
			 * updating for each of after's on-block afters.
			 * This seems complicated and slow and it turns out we do not
			 * actually do this (for now?), so just assert that it does not
			 * occur.
			 * We assert 'either no on-block afters or befores', instead
			 * of the simpler assert 'either no afters or befores', because
			 * move_befores_for_merge() can remove the dependency between
			 * two emptys with the after having afters, before having
			 * befores, but the after not having any on-block afters. */
			assert(!has_block_afters(after) || !has_block_befores(before));
		}
	}
	else if(!before->block)
		propagate_extern_after_change_thru_empty_before(before, after, add);
	else if(patch_is_external(after, before->block))
	{
		if(add)
		{
			before->block->extern_after_count++;
			assert(before->block->extern_after_count);
		}
		else
		{
			assert(before->block->extern_after_count);
			before->block->extern_after_count--;
		}
	}
}
#endif /* BDESC_EXTERN_AFTER_COUNT */

/* propagate a depend add, to update ready and extern_after state */
static void propagate_depend_add(patch_t * after, patch_t * before)
{
	uint16_t before_level = patch_level(before);
	uint16_t after_prev_level;
	
	if(before_level == BDLEVEL_NONE)
		return;
	after_prev_level = patch_level(after);
	
	after->nbefores[before_level]++;
	assert(after->nbefores[before_level]);
	patch_update_ready_patches(after);
	if(!after->owner && (before_level > after_prev_level || after_prev_level == BDLEVEL_NONE))
			propagate_level_change_thru_empty(after, after_prev_level, before_level);
#if BDESC_EXTERN_AFTER_COUNT
	/* an inflight patch does not contribute to its block's
	 * extern_after_count */
	if(!(before->flags & PATCH_INFLIGHT))
		propagate_extern_after_change(after, before, 1);
#endif
}

/* propagate a depend remove, to update ready and extern_after state */
static void propagate_depend_remove(patch_t * after, patch_t * before)
{
	uint16_t before_level = patch_level(before);
	uint16_t after_prev_level;
	
	if(before_level == BDLEVEL_NONE)
		return;
	after_prev_level = patch_level(after);
	
	assert(after->nbefores[before_level]);
	after->nbefores[before_level]--;
	patch_update_ready_patches(after);
	if(!after->owner && (before_level == after_prev_level && !after->nbefores[before_level]))
		propagate_level_change_thru_empty(after, after_prev_level, patch_level(after));
#if BDESC_EXTERN_AFTER_COUNT
	/* extern_after_count is pre-decremented when a patch goes inflight */
	if(!(before->flags & PATCH_INFLIGHT))
		propagate_extern_after_change(after, before, 0);
#endif
}

/* propagate a level change, to update ready state */
void patch_propagate_level_change(patch_t * patch, uint16_t prev_level, uint16_t new_level)
{
	patchdep_t * afters = patch->afters;
	assert(prev_level < NBDLEVEL || prev_level == BDLEVEL_NONE);
	assert(new_level < NBDLEVEL || new_level == BDLEVEL_NONE);
	assert(prev_level != new_level);
	for(; afters; afters = afters->after.next)
	{
		patch_t * after = afters->after.desc;
		uint16_t after_prev_level = patch_level(after);

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
		patch_update_ready_patches(after);

		if(!after->owner)
		{
			uint16_t after_new_level = patch_level(after);
			if(after_prev_level != after_new_level)
				propagate_level_change_thru_empty(after, after_prev_level, after_new_level);
		}
	}
}

/* add a dependency between patches without checking for cycles */
int patch_add_depend_no_cycles(patch_t * after, patch_t * before)
{
	patchdep_t * dep;
	
	if(!(after->flags & PATCH_SAFE_AFTER))
	{
		assert(after->type == EMPTY && !after->afters); /* quickly catch bugs for now */
		if(after->type != EMPTY || after->afters)
			return -EINVAL;
	}
	
	/* in-flight and on-disk patches cannot (generally) safely gain befores */
	if(after->flags & PATCH_INFLIGHT)
	{
		printf("%s(): (%s:%d): Adding before to in flight after!\n", __FUNCTION__, __FILE__, __LINE__);
		return -EINVAL;
	}
	if(after->flags & PATCH_WRITTEN)
	{
		if(before->flags & PATCH_WRITTEN)
			return 0;
		printf("%s(): (%s:%d): Attempt to add before to already written data!\n", __FUNCTION__, __FILE__, __LINE__);
		return -EINVAL;
	}
	
	/* no need to actually create a dependency on a written patch */
	if(before->flags & PATCH_WRITTEN)
		return 0;
	
	/* the block cannot be written until 'before' is on disk, so an explicit
	 * dependency from a same-block patch is unnecessary */
	if(after->block && before->block && after->block == before->block && (before->flags & PATCH_INFLIGHT))
		return 0;
	
#if !PATCH_ALLOW_MULTIGRAPH
	/* make sure it's not already there */
	for(dep = after->befores; dep; dep = dep->before.next)
		if(dep->desc == before)
			return 0;
	/* shouldn't be there */
	for(dep = before->afters; dep; dep = dep->after.next)
		assert(dep->desc != after);
#else
	/* how frequently do these happen? more frequently than you'd think! */
	if(before->afters && before->afters->after.desc == after)
		return 0;
	if(after->befores && after->befores->before.desc == before)
		return 0;
	
	if(before->afters && container_of(before->afters_tail, patchdep_t, after.next)->after.desc == after)
		return 0;
	if(after->befores && container_of(after->befores_tail, patchdep_t, before.next)->before.desc == before)
		return 0;
#endif
	
	if(before->flags & PATCH_SET_EMPTY)
	{
		int r = 0;
		assert(before->type == EMPTY);
		assert(!before->afters);
		for(dep = before->befores; dep; dep = dep->before.next)
			if((r = patch_add_depend_no_cycles(after, dep->before.desc)) < 0)
				break;
		return r;
	}
	
	dep = patchdep_alloc();
	if(!dep)
		return -ENOMEM;
	account_update(&act_ndeps, 1);
	
	propagate_depend_add(after, before);
	
	/* add the before to the after */
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_ADD_BEFORE, after, before);
	dep->before.desc = before;
	dep->before.next = NULL;
	dep->before.ptr = after->befores_tail;
	*after->befores_tail = dep;
	after->befores_tail = &dep->before.next;
	
	/* add the after to the before */
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_ADD_AFTER, before, after);
	dep->after.desc = after;
	dep->after.next = NULL;
	dep->after.ptr = before->afters_tail;
	*before->afters_tail = dep;
	before->afters_tail = &dep->after.next;
	
	/* virgin EMPTY patch getting its first before */
	if(free_head == after || after->free_prev)
	{
		assert(after->type == EMPTY);
		assert(!(after->flags & PATCH_WRITTEN));
		patch_free_remove(after);
	}
	
	return 0;
}

/* Conservatively return true iff 'after' directly depends on 'before' */
static inline bool quick_depends_on(const patch_t * after, const patch_t * before)
{
	/* Quick (bidirectional width-2) check for after->before */
	if(!after->befores || !before->afters)
		return 0;
	if(before->afters->after.desc == after)
		return 1;
	if(before->afters->after.next && before->afters->after.next->after.desc == after)
		return 1;
	if(after->befores->before.desc == before)
		return 1;
	if(after->befores->before.next && after->befores->before.next->before.desc == before)
		return 1;
	return 0; /* No after->before found */
}

/* make the recent patch depend on the given earlier patch in the same
 * block if it overlaps.
 * Return non-negative on success:
 * 0 if no overlap
 * 1 if there overlap and recent now (in)directly depends on original. */
static int patch_overlap_attach(patch_t * recent, patch_t * middle, patch_t * original)
{
	int r, overlap;
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_INFO, FDB_PATCH_OVERLAP_ATTACH, recent, original);
	
	/* if either is a EMPTY patch, warn about it */
	if(recent->type == EMPTY || original->type == EMPTY)
		printf("%s(): (%s:%d): Unexpected EMPTY patch\n", __FUNCTION__, __FILE__, __LINE__);
	
	/* if they don't overlap, we are done */
	overlap = patch_overlap_check(recent, original);
	if(!overlap)
		return 0;
	
	if(original->flags & PATCH_ROLLBACK)
	{
		/* it's not clear what to do in this case... just fail with a warning for now */
		printf("Attempt to overlap a new patch (%p) with a rolled-back patch (%p)! (debug = %d)\n", recent, original, FSTITCH_DEBUG_COUNT());
		return -EBUSY;
	}
	
	if(!middle || !quick_depends_on(middle, original))
	{
		r = patch_add_depend(recent, original);
		if(r < 0)
			return r;
	}
	
	/* if it overlaps completely, remove original from ddesc->overlaps or ddesc->bit_patches */
	if(overlap == 2)
	{
		if(original->type == BYTE)
			patch_unlink_overlap(original);
		else if(original->type == BIT)
		{
			patch_t * bit_patches = patch_bit_patches(original->block, original->offset);
			assert(bit_patches);
			patch_remove_depend(bit_patches, original);
		}
		else
			kpanic("Complete overlap of unhandled patch type!");
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, recent, PATCH_OVERLAP);
		recent->flags |= PATCH_OVERLAP;
	}
	
	return 1;
}

static int _patch_overlap_multiattach(patch_t * patch, patch_t * list_patch)
{
	patchdep_t * list = list_patch->befores;
	patchdep_t * next = list;
	while((list = next))
	{
		int r;
		
		/* this loop is tricky, because we might remove the item we're
		 * looking at currently if we overlap it entirely - so we
		 * prefetch the next pointer at the top of the loop */
		next = list->before.next;
		
		list_patch = list->before.desc;
		
		if(patch == list_patch)
			continue;
		r = patch_overlap_attach(patch, NULL, list_patch);
		if(r < 0)
			return r;
	}
	return 0;
}

static __inline int _patch_overlap_multiattach_x(patch_t * patch, patch_t ** middle, patch_t **list) __attribute__((always_inline));
static __inline int _patch_overlap_multiattach_x(patch_t * patch, patch_t ** middle, patch_t **list)
{
	int r;
	while(*list) {
		patch_t *c = *list;
		if(c != patch)
		{
			if((r = patch_overlap_attach(patch, *middle, c)) < 0)
				return r;
			if(r == 1)
				*middle = c;
		}
		if(*list == c)
			list = &c->overlap_next;
	}
	return 0;
}

static int patch_overlap_multiattach(patch_t * patch, bdesc_t * block)
{
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_INFO, FDB_PATCH_OVERLAP_MULTIATTACH, patch, block);
	
	if(patch->type == BIT)
	{
		patch_t * bit_patches = patch_bit_patches(block, patch->offset);
		if(bit_patches)
		{
			int r = _patch_overlap_multiattach(patch, bit_patches);
			if(r < 0)
				return r;
		}
	}
	else if(patch->type == BYTE && block->bit_patches)
	{
		hash_map_it2_t it = hash_map_it2_create(block->bit_patches);
		while(hash_map_it2_next(&it))
		{
			patch_t * bit_patches = it.val;
			int r;
			if(patch_overlap_check(patch, bit_patches->befores->before.desc))
				if((r = _patch_overlap_multiattach(patch, bit_patches)) < 0)
					return r;
		}
	}

	// get range of buckets touched by this patch
	int list1 = patch_overlap_list(patch), list2;
	assert(list1 >= 0);
	if (list1 == 0) {
		assert(patch->type == BYTE);
		int sz = block->length >> OVERLAP1SHIFT;
		list1 = patch->offset / sz + 1;
		list2 = (patch->offset + patch->length - 1) / sz + 1;
	} else
		list2 = list1;

	int r;
	patch_t * middle = NULL;
	for (; list1 <= list2; list1++)
		if ((r = _patch_overlap_multiattach_x(patch, &middle, &block->overlap1[list1])) < 0)
			return r;

	return _patch_overlap_multiattach_x(patch, &middle, &block->overlap1[0]);
}


#if PATCH_OVERLAPS2
static patch_t *patch_find_overlaps(bdesc_t * block, uint32_t offset, uint32_t length, uint32_t mask)
{
	patch_t *olist = NULL;
	patch_t *oprev = NULL;
	patch_t **opprev = &olist;
	patch_t *c;

	if (block->bit_patches) {
		uint32_t o;
		for (o = (offset & ~3); o < offset + length; o += 4) {
			if (!(c = patch_bit_patches(block, o)))
				continue;
			patchdep_t *dep;
			for (dep = c->befores; dep; dep = dep->before.next) {
				c = dep->before.desc;
				if (!(mask & c->bit.or))
					continue;
				if (oprev && quick_depends_on(oprev, c))
					continue;
				if ((mask & c->bit.or) == c->bit.or)
					c->flags |= PATCH_FULLOVERLAP;
				else
					c->flags &= ~PATCH_FULLOVERLAP;
				*opprev = oprev = c;
				opprev = &c->tmp_next;
			}
		}
	}

	// get range of buckets touched by this patch
	int sz = block->length >> OVERLAP1SHIFT;
	int list1 = offset / sz + 1;
	int list2 = (offset + length - 1) / sz + 1;

 buckets_again:
	for (; list1 <= list2; list1++)
		for (c = block->overlap1[list1]; c; c = c->overlap_next)
			if (!(c->offset >= offset + length
			      || offset >= c->offset + c->length)) {
				if (oprev && quick_depends_on(oprev, c))
					continue;
				if (offset <= c->offset
				    && c->offset + c->length <= offset + length)
					c->flags |= PATCH_FULLOVERLAP;
				else
					c->flags &= ~PATCH_FULLOVERLAP;
				*opprev = oprev = c;
				opprev = &c->tmp_next;
			}

	if (list2) {
		list1 = list2 = 0;
		goto buckets_again;
	}

	*opprev = NULL;
	return olist;
}

static int patch_apply_overlaps(patch_t *patch, patch_t *overlap_list)
{
	int r = 0;
	patch_t *next;

	for (; overlap_list; overlap_list = next) {
		next = overlap_list->tmp_next;
		overlap_list->tmp_next = NULL;

		r = patch_add_depend(patch, overlap_list);
		if(r < 0)
			return r;

		if (overlap_list->flags & PATCH_FULLOVERLAP) {
			if (overlap_list->type == BYTE)
				patch_unlink_overlap(overlap_list);
			else if (overlap_list->type == BIT) {
				patch_t * bit_patches = patch_bit_patches(patch->block, overlap_list->offset);
				assert(bit_patches);
				patch_remove_depend(bit_patches, overlap_list);
			}
		}
	}

	return r;
}
#endif /* PATCH_OVERLAPS2 */


void patch_tmpize_all_patches(patch_t * patch)
{
	assert(!patch->tmp_next && !patch->tmp_pprev);

	if(patch->ddesc_pprev)
	{
		patch->tmp_next = patch->ddesc_next;
		patch->tmp_pprev = patch->ddesc_pprev;
		if(patch->ddesc_next)
			patch->ddesc_next->ddesc_pprev = patch->ddesc_pprev;
		else
			patch->block->all_patches_tail = patch->ddesc_pprev;
		*patch->ddesc_pprev = patch->ddesc_next;

		patch->ddesc_next = NULL;
		patch->ddesc_pprev = NULL;
	}
	else
		assert(!patch->ddesc_next);
}

void patch_untmpize_all_patches(patch_t * patch)
{
	assert(!patch->ddesc_next && !patch->ddesc_pprev);

	if(patch->tmp_pprev)
	{
		patch->ddesc_next = patch->tmp_next;
		patch->ddesc_pprev = patch->tmp_pprev;
		if(patch->ddesc_next)
			patch->ddesc_next->ddesc_pprev = &patch->ddesc_next;
		else
			patch->block->all_patches_tail = &patch->ddesc_next;
		*patch->ddesc_pprev = patch;

		patch->tmp_next = NULL;
		patch->tmp_pprev = NULL;
	}
	else
		assert(!patch->tmp_next);
}

/* EMPTY patches may have:
 * - NULL block and owner, in which case it is a "normal" EMPTY.
 *   (propagates before/after counts. propagates external counts.)
 * - NULL block and non-NULL owner: has a device level and thus prevents its afters
 *   from going lower than that device.
 *   (counts towards before/after counts. propagates external counts.)
 * - non-NULL block and owner: makes the block dirty and can prevent the block from
 *   being evicted from a cache, is internal/external.
 *   (counts towards before/after counts. counts towards external counts.) */

int patch_create_empty_set(BD_t * owner, patch_t ** tail, patch_pass_set_t * befores)
{
	patch_t * patch;
	int r;
	
	assert(tail);
	
	patch = patch_alloc();
	if(!patch)
		return -ENOMEM;
	account_npatches(EMPTY, 1);
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CREATE_EMPTY, patch, owner);
#if COUNT_PATCHES
	patch_counts[EMPTY]++;
	dump_counts();
#endif
	
	patch->owner = owner;
	patch->block = NULL;
	patch->type = EMPTY;
	patch->offset = 0;
	patch->length = 0;
	patch->befores = NULL;
	patch->befores_tail = &patch->befores;
	patch->afters = NULL;
	patch->afters_tail = &patch->afters;
	patch->weak_refs = NULL;
	memset(patch->nbefores, 0, sizeof(patch->nbefores));
	patch->free_prev = NULL;
	patch->free_next = NULL;
	patch->ddesc_next = NULL;
	patch->ddesc_pprev = NULL;
	patch->ddesc_ready_next = NULL;
	patch->ddesc_ready_pprev = NULL;
	patch->ddesc_index_next = NULL;
	patch->ddesc_index_pprev = NULL;
	patch->tmp_next = NULL;
	patch->tmp_pprev = NULL;
	patch->overlap_next = NULL;
	patch->overlap_pprev = NULL;
	patch->flags = PATCH_SAFE_AFTER;
	
	patch_free_push(patch);
	
	for(; befores; befores = befores->next)
	{
		size_t i, size;
		patch_t ** array;
		if(befores->size > 0)
		{
			size = befores->size;
			array = befores->array;
		}
		else
		{
			size = -befores->size;
			array = befores->list;
		}
		for(i = 0; i < size; i++)
			/* it is convenient to allow NULL and written patches,
			   so make sure here to not add these as befores: */
			if(array[i] && !(array[i]->flags & PATCH_WRITTEN))
				if((r = patch_add_depend_no_cycles(patch, array[i])) < 0)
				{
					patch_destroy(&patch);
					return r;
				}
	}
	patch->flags &= ~PATCH_SAFE_AFTER;
	*tail = patch;
	
	return 0;
}

int patch_create_empty_array(BD_t * owner, patch_t ** tail, size_t nbefores, patch_t * befores[])
{
	patch_pass_set_t set = {.next = NULL, .size = -nbefores};
	set.list = befores;
	return patch_create_empty_set(owner, tail, &set);
}

#define STATIC_BEFORES_CAPACITY 10 /* 10 should cover most cases */

int patch_create_empty_list(BD_t * owner, patch_t ** tail, ...)
{
	static patch_t * static_befores[STATIC_BEFORES_CAPACITY];
	patch_t ** befores;
	size_t nbefores = 0;
	size_t i;
	va_list ap;
	int r;
	
	va_start(ap, tail);
	while(va_arg(ap, patch_t *))
		nbefores++;
	va_end(ap);
	/* TODO: consider doing this instead of copying the array */
	/*
		va_start(ap, tail);
		&va_arg(ap, patch_t *)
	*/
	
	if(nbefores <= STATIC_BEFORES_CAPACITY)
		befores = static_befores;
	else
	{
		befores = smalloc(nbefores * sizeof(befores[0]));
		if(!befores)
			return -ENOMEM;
	}
	
	va_start(ap, tail);
	for(i = 0; i < nbefores; i++)
		befores[i] = va_arg(ap, patch_t *);
	va_end(ap);
	
	r = patch_create_empty_array(owner, tail, nbefores, befores);
	
	if(befores != static_befores)
		sfree(befores, nbefores * sizeof(befores[0]));
	return r;
}

static __inline bool new_patches_require_data(const bdesc_t * block) __attribute__((always_inline));
static __inline bool new_patches_require_data(const bdesc_t * block)
{
#if PATCH_NRB
	/* Rule: When adding patch C to block B,
	 * and forall C' on B, with C' != C: C' has no afters on blocks != B,
	 * then C will never need to be rolled back. */
	return block->extern_after_count > 0;
#else
	return 1;
#endif
}

/* patch merge stat tracking definitions */
#if PATCH_NRB_MERGE_STATS
# define N_PATCH_NRB_MERGE_STATS 3
static uint32_t patch_nrb_merge_stats[N_PATCH_NRB_MERGE_STATS] = {0, 1, 2};
static unsigned patch_nrb_merge_stats_idx = -1;
static bool patch_nrb_merge_stats_callback_registered = 0;

static void print_patch_nrb_merge_stats(void * ignore)
{
	unsigned i;
	uint32_t npatches = 0;
	uint32_t npatches_notmerged = 0;
	(void) ignore;

	for(i = 0; i < N_PATCH_NRB_MERGE_STATS; i++)
	{
		npatches += patch_nrb_merge_stats[i];
		if(i > 0)
			npatches_notmerged += patch_nrb_merge_stats[i];
	}
	
	printf("patches merge stats:\n");
	
	if(!npatches)
	{
		/* protect against divide by zero */
		printf("\tno patches created\n");
		return;
	}
	printf("\tmerged: %u (%3.1f%% all)\n", patch_nrb_merge_stats[0], 100 * ((float) patch_nrb_merge_stats[0]) / ((float) npatches));

	if(!npatches_notmerged)
	{
		/* protect against divide by zero */
		printf("\tall patches merged?!\n");
		return;
	}
	for(i = 1; i < N_PATCH_NRB_MERGE_STATS; i++)
		printf("\tnot merged case %u: %u (%3.1f%% non-merged)\n", i, patch_nrb_merge_stats[i], 100 * ((float) patch_nrb_merge_stats[i]) / ((float) npatches_notmerged));
}

# include <fscore/fstitchd.h>
static void patch_nrb_merge_stats_log(unsigned idx)
{
	if(!patch_nrb_merge_stats_callback_registered)
	{
		int r = fstitchd_register_shutdown_module(print_patch_nrb_merge_stats, NULL, SHUTDOWN_POSTMODULES);
		if(r < 0)
			kpanic("fstitchd_register_shutdown_module() = %i", r);
		patch_nrb_merge_stats_callback_registered = 1;
	}
	patch_nrb_merge_stats_idx = idx;
	patch_nrb_merge_stats[idx]++;
}
# define PATCH_NRB_MERGE_STATS_LOG(_idx) patch_nrb_merge_stats_log(_idx)
#else
# define PATCH_NRB_MERGE_STATS_LOG(_idx) do { } while(0)
#endif

#if PATCH_NRB
/* Determine whether a new patch on 'block' and with the before 'before'
 * can be merged into an existing patch.
 * Return such a patch if so, else return NULL. */
static patch_t * select_patch_merger(const bdesc_t * block)
{
	if(new_patches_require_data(block))
	{
		/* rollbackable patch dep relations can be complicated, give up */
		PATCH_NRB_MERGE_STATS_LOG(1);
		return NULL;
	}
	if(!WEAK(block->nrb))
	{
		PATCH_NRB_MERGE_STATS_LOG(2);
		return NULL;
	}
	PATCH_NRB_MERGE_STATS_LOG(0);
	assert(!(WEAK(block->nrb)->flags & PATCH_INFLIGHT));
	return WEAK(block->nrb);
}

static void patch_move_before_fast(patch_t * old_after, patch_t * new_after, patchdep_t * depbefore)
{
	patch_t * before = depbefore->before.desc;
	int r;
	patch_dep_remove(depbefore);
	r = patch_add_depend_no_cycles(new_after, before);
	assert(r >= 0); /* failure should be impossible */
}

/* Move patch's (transitive) befores that cannot reach merge_target
 * to be merge_target's befores, so that a merge into merge_target
 * maintains the needed befores. root_patch_stays == 0 implies that
 * patch is a before (the new patch does not yet exist) and == 1 implies
 * that patch is a pre-existing patch (that can thus not be moved)*/
static void move_befores_for_merge(patch_t * patch, patch_t * merge_target, bool root_patch_stays)
{
	/* recursion-on-the-heap support */
	struct state {
		patchdep_t * dep;
		patch_t * patch;
		bool reachable;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;
	
	patch_t * root_patch = patch;
	uint16_t saved_flags;
	patch_t * marked_head;
	
	patchdep_t * dep;
	bool reachable = 0; /* whether target is reachable from patch */
	
	saved_flags = merge_target->flags;
	merge_target->flags |= PATCH_SAFE_AFTER;
	
	assert(!merge_target->tmp_next && !merge_target->tmp_pprev);
	merge_target->tmp_pprev = &marked_head;
	marked_head = merge_target;
	
  recurse_enter:
	/* Use tmp list to indicate merge_target is reachable.
	 * We don't need the pprev links, but they simplify this first check
	 * (removing the need to also check for 'patch==marked_head'). */
	if(patch->tmp_pprev)
	{
		reachable = 1;
		goto recurse_return;
	}
	if(patch_is_external(patch, merge_target->block))
		goto recurse_return;
	if(root_patch_stays && patch != root_patch)
	{
		if(patch->flags & PATCH_INFLIGHT)
			goto recurse_return;
		if(patch->type != EMPTY)
		{
			/* Treat same-block, data patches as able to reach merge_target.
			 * Caller will ensure they do reach merge_target. */
			patch->tmp_next = marked_head;
			patch->tmp_pprev = &marked_head;
			marked_head->tmp_pprev = &patch->tmp_next;
			marked_head = patch;
			reachable = 1;
			goto recurse_return;
		}
	}
	
	/* discover the subset of befores that cannot reach target */
	/* TODO: do not scan a given dep->before.desc that cannot reach
	 * merge_target multiple times? */
	for(dep = patch->befores; dep; dep = dep->before.next)
	{
		// Recursively move befores; equivalent to:
		// reachable |= move_befores_for_merge(dep->before.desc, merge_target)
		state->dep = dep;
		state->patch = patch;
		state->reachable = reachable;
		
		patch = dep->before.desc;
		reachable = 0;
		
		INCREMENT_STATE(state, static_states, states, states_capacity);
		goto recurse_enter;
			
	  recurse_resume:
		(void) 0; /* placate compiler re deprecated end labels */
	}
	
	/* if only some befores can reach target, move them.
	 * if none can reach target, caller will move patch. */
	if(reachable)
	{
		patchdep_t * next;
		
		assert(!patch->tmp_pprev);
		patch->tmp_next = marked_head;
		patch->tmp_pprev = &marked_head;
		marked_head->tmp_pprev = &patch->tmp_next;
		marked_head = patch;
		
		for(dep = patch->befores; dep; dep = next)
		{
			next = dep->before.next;
			if(!dep->before.desc->tmp_pprev)
				patch_move_before_fast(patch, merge_target, dep);
		}
	}
	
  recurse_return:
	if(state != &states[0])
	{
		state--;
		dep = state->dep;
		patch = state->patch;
		reachable |= state->reachable;
		goto recurse_resume;
	}
	
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
	
	/* remove patches from the marked list only after all traversals
	 * because of multipaths */
	while(marked_head)
	{
		patch_t * head = marked_head;
		marked_head = marked_head->tmp_next;
		head->tmp_next = NULL;
		head->tmp_pprev = NULL;
	}
	
	/* take care of the intial before/patch */
	if(!reachable)
	{
		if(!root_patch_stays)
		{
			int r = patch_add_depend_no_cycles(merge_target, patch);
			assert(r >= 0); (void) r;
		}
		else
		{
			patchdep_t * next;
			for(dep = patch->befores; dep; dep = next)
			{
				patch_t * before = dep->before.desc;
				next = dep->before.next;
				if(patch_is_external(before, merge_target->block) || (before->flags & PATCH_INFLIGHT))
					patch_move_before_fast(patch, merge_target, dep);
			}
		}
	}
	
	merge_target->flags = saved_flags;
}
#endif

#if PATCH_MERGE_RBS_NRB
/* Return whether 'after' depends on any data patch on its block.
 * Requires 'bdesc' to have no external afters. */
static bool patch_has_block_befores(const patch_t * after, const bdesc_t * bdesc)
{
	patchdep_t * dep;
	for(dep = after->befores; dep; dep = dep->before.next)
	{
		const patch_t * before = dep->before.desc;
		if(patch_is_external(before, bdesc) || (before->flags & PATCH_INFLIGHT))
			continue;
		if(before->type != EMPTY)
			return 1;
		if(patch_has_block_befores(before, bdesc))
			return 1;
	}
	return 0;
}

/* Return the address of the patch containing the pointed to ddesc_next */
static patch_t * pprev2patch(patch_t ** patch_ddesc_pprev)
{
	return container_of(patch_ddesc_pprev, patch_t, ddesc_next);
}

/* Return a data patch on 'block' that has no before path to a patch
 * on its block. Return NULL if there are no data patches on 'block'. */
static patch_t * find_patch_without_block_befores(bdesc_t * block)
{
	/* The last data patch should be the oldest patch on 'block'
	 * and, since it is not an NRB, thus have no block befores */
	patch_t ** pprev = block->all_patches_tail;
	patch_t * patch;
	for(; pprev != &block->all_patches; pprev = patch->ddesc_pprev)
	{
		patch = pprev2patch(pprev);
		if(patch->type != EMPTY && !(patch->flags & PATCH_INFLIGHT) && !patch_has_block_befores(patch, block))
		{
			assert(patch->type == BYTE || patch->type == BIT);
			return patch;
		}
		if(patch == block->all_patches)
			break;
	}
	return NULL;
}

/* Remove all block bit_patches befores */
static void clear_bit_patches(bdesc_t * block)
{
	if(block->bit_patches)
	{
		hash_map_it2_t it = hash_map_it2_create(block->bit_patches);
		while(hash_map_it2_next(&it))
			patch_destroy((patch_t **) &it.val);
		assert(hash_map_empty(block->bit_patches));
	}
}

/* Merge all RBs on 'block' into a single NRB */
/* TODO: if this function ends up being heavily used during runtime,
 * its two dependency move algorithms can be much simpler. */
static void merge_rbs(bdesc_t * block)
{
# if PATCH_MERGE_RBS_NRB_STATS
	static uint32_t ncalls = 0, nmerged_total = 0;
	uint32_t nmerged = 0;
# endif
	patch_t * merger, * patch, * next;
	int r;
	
	/* choose merger so that it does not depend on any other data patches
	 * on the block to simplify before merging */ 
	if(!(merger = find_patch_without_block_befores(block)))
		return;
	
	/* move the befores of each RB for their merge */
	for(patch = block->all_patches; patch; patch = patch->ddesc_next)
		if(patch != merger && patch->type != EMPTY && !(patch->flags & PATCH_INFLIGHT))
			move_befores_for_merge(patch, merger, 1);
	
	/* convert RB merger into a NRB (except overlaps, done later) */
	if(merger->type == BYTE)
		patch_free_byte_data(merger);
	else if(merger->type == BIT)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CONVERT_BYTE, merger, 0, merger->owner->level);
		account_npatches_convert(BIT, BYTE);
# if COUNT_PATCHES
		patch_counts[BIT]--;
		patch_counts[BYTE]++;
		dump_counts();
# endif
		merger->type = BYTE;
	}
	else
		assert(0);
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_OFFSET, merger, 0);
	merger->offset = 0;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_LENGTH, merger, block->length);
	merger->length = block->length;
	merger->byte.data = NULL;
# if PATCH_BYTE_SUM
	merger->byte.old_sum = 0;
	merger->byte.new_sum = 0;
# endif
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CLEAR_FLAGS, merger, PATCH_OVERLAP);
	merger->flags &= ~PATCH_OVERLAP;
	assert(!WEAK(block->nrb));
	patch_weak_retain(merger, &block->nrb, NULL, NULL);
	//account_update(&act_nnrb, 1);

	/* ensure merger is in overlaps (to complete NRB construction) and remove
	 * all bit overlaps (to complete NRB construction and for non-mergers) */
	clear_bit_patches(block);
	patch_unlink_overlap(merger);
	patch_link_overlap(merger);
	
	/* convert non-merger data patches into emptys so that pointers to them
	 * remain valid.
	 * TODO: could we destroy the emptys with no afters after the runloop? */
	/* part a: unpropagate extern after counts (no more data patch afters)
	 * (do before rest of conversion to correctly (not) recurse) */
	for(patch = block->all_patches; patch; patch = patch->ddesc_next)
	{
		patchdep_t * dep, * next;
		if(patch == merger || patch->type == EMPTY || (patch->flags & PATCH_INFLIGHT))
			continue;
		for(dep = patch->befores; dep; dep = next)
		{
			patch_t * before = dep->before.desc;
			next = dep->before.next;
			if(before->flags & PATCH_INFLIGHT)
				continue;
			if(!before->block)
				propagate_extern_after_change_thru_empty_before(before, patch, 0);
			else if(patch_is_external(patch, before->block))
			{
				assert(before->block->extern_after_count);
				before->block->extern_after_count--;
			}
			else
			{
				/* intra-block empty dependencies, other than empty->merger, are
				 * unnecessary & can lead to empty path blowup. Remove them. */
				patch_dep_remove(dep);
			}
		}
	}
	/* parb b: convert into emptys */
	for(patch = block->all_patches; patch; patch = next)
	{
		uint16_t level;
		uint16_t flags;
		next = patch->ddesc_next;
		if(patch == merger || patch->type == EMPTY || (patch->flags & PATCH_INFLIGHT))
			continue;
		
# if PATCH_MERGE_RBS_NRB_STATS
		nmerged++;
# endif
		
		/* ensure patch depends on merger. add dep prior to empty conversion
		 * to do correct level propagation inside patch_add_depend(). */
		flags = patch->flags;
		patch->flags |= PATCH_SAFE_AFTER;
		r = patch_add_depend(patch, merger);
		assert(r >= 0);
		patch->flags = flags;

		patch_unlink_overlap(patch);
		patch_unlink_index_patches(patch);
		patch_unlink_ready_patches(patch);
		patch_unlink_all_patches(patch);
		if(patch->type == BYTE)
			patch_free_byte_data(patch);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CONVERT_EMPTY, patch);
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "rb->nrb mergee");
		account_npatches_convert(patch->type, EMPTY);
# if COUNT_PATCHES
		patch_counts[patch->type]--;
		patch_counts[EMPTY]++;
		dump_counts();
# endif
		patch->type = EMPTY;
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_BLOCK, patch, NULL);
		bdesc_release(&patch->block);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_OWNER, patch, NULL);
		patch->owner = NULL;
		patch->empty.bit_patches = NULL;
		patch->empty.hash_key = NULL;
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CLEAR_FLAGS, patch, PATCH_OVERLAP);
		patch->flags &= ~PATCH_OVERLAP;
		
		if(merger->owner->level != (level = patch_level(patch)))
			propagate_level_change_thru_empty(patch, merger->owner->level, level);
	}
# if PATCH_MERGE_RBS_NRB_STATS
	if(nmerged)
	{
		ncalls++;
		nmerged_total += nmerged;
		printf("%s(block %u). merged: %u now, %u total, %u avg.\n", __FUNCTION__, block->number, nmerged, nmerged_total, nmerged_total / ncalls);
	}
# endif
}
#endif

/* Attempt to merge into an existing patch instead of create a new patch.
 * Returns 1 on successful merge (*merged points merged patch),
 * 0 if no merge could be made, or < 0 upon error. */
static int patch_create_merge(bdesc_t * block, BD_t * owner, patch_t ** tail, patch_pass_set_t * befores)
{
#if PATCH_NRB
	patch_t * merger;
# if PATCH_MERGE_RBS_NRB
	if(!new_patches_require_data(block) && !WEAK(block->nrb))
		merge_rbs(block);
# endif
	if(!(merger = select_patch_merger(block)))
		return 0;
	for(; befores; befores = befores->next)
	{
		size_t i, size;
		patch_t ** array;
		if(befores->size > 0)
		{
			size = befores->size;
			array = befores->array;
		}
		else
		{
			size = -befores->size;
			array = befores->list;
		}
		for(i = 0; i < size; i++)
			if(array[i])
				move_befores_for_merge(array[i], merger, 0);
	}
	
	/* move merger to correct owner */
	if (merger->owner != owner) {
		patch_unlink_index_patches(merger);
		merger->owner = owner;
		patch_link_index_patches(merger);
	}
	
	*tail = merger;
	return 1;
#else
	return 0;
#endif
}

#if PATCH_BYTE_MERGE_OVERLAP
/* Conservatively return true iff left's befores are a subset of right's befores */
static bool quick_befores_subset(const patch_t * left, const patch_t * right)
{
	const size_t max_nleft_befores = 2;
	const patchdep_t * left_dep = left->befores;
	size_t i;

	if(!left->befores)
		return 1;
	if(!right->befores)
		return 0;

	for(left_dep = left->befores, i = 0; left_dep; left_dep = left_dep->before.next, i++)
	{
		if (i >= max_nleft_befores)
			return 0;
		if(!quick_depends_on(right, left_dep->before.desc))
			return 0;
	}
	return 1;
}

#if !PATCH_OVERLAPS2
/* A simple RB merge opportunity:
 * patch has no explicit befores and has a single overlap.
 * Returns 1 on successful merge (*tail points to merged patch),
 * 0 if no merge could be made, or < 0 upon error. */
static int patch_create_byte_merge_overlap(patch_t ** tail, patch_t ** new, patch_pass_set_t * befores)
{
	patchdep_t * dep;
	patch_t * overlap = NULL;
	uint16_t overlap_end, new_end, merge_offset, merge_length, merge_end;
	bdesc_t * bdesc = (*new)->block;
	patch_pass_set_t * scan;
	int r;
	
	/* determine whether we can merge new into an overlap */
	/* NOTE: if a befores[i] has a before and there are many overlaps, it may
	 * be wise to check befores[i] for befores before looking at overlaps */
	for(dep = (*new)->befores; dep; dep = dep->before.next)
	{
		patch_t * before = dep->before.desc;
		if(before->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
			continue;
		if(before->block && (*new)->block == before->block
		   && patch_overlap_check(*new, before))
		{
			/* note: *new may overlap a before[i] */
			if(before->type != BYTE)
				return 0;
			if(overlap && overlap != before)
			{
#if PATCH_RB_NRB_READY
				/* TODO: does this actually require PATCH_RB_NRB_READY? */
				/* nrb depends on nothing on this block so an above is ok */
				if(before == WEAK(before->block->nrb))
					continue;
				if(overlap == WEAK(before->block->nrb))
				{
					overlap = before;
					continue;
				}
#endif
				return 0;
			}
			overlap = before;
		}
		else
		{
#ifndef NDEBUG
			// assert(exists i: befores[i] == before):
			for(scan = befores; scan; scan = scan->next)
			{
				size_t i, size;
				patch_t ** array;
				if(scan->size > 0)
				{
					size = scan->size;
					array = scan->array;
				}
				else
				{
					size = -scan->size;
					array = scan->list;
				}
				for(i = 0; i < size; i++)
				{
					if(array[i] && array[i]->flags & PATCH_SET_EMPTY)
					{
						patchdep_t * dep;
						for(dep = array[i]->befores; dep; dep = dep->before.next)
							if(dep->before.desc == before)
								goto match;
					}
					else if(array[i] == before)
						goto match;
				}
			}
			assert(!befores);
		  match:
			(void) 0; /* placate compiler re deprecated end labels */
#endif
		}
	}
	if(!overlap)
		return 0;
	
	/* Check that *new's explicit befores will not induce patch cycles */
	for(scan = befores; scan; scan = scan->next)
	{
		size_t i, size;
		patch_t ** array;
		if(scan->size > 0)
		{
			size = scan->size;
			array = scan->array;
		}
		else
		{
			size = -scan->size;
			array = scan->list;
		}
		for(i = 0; i < size; i++)
		{
			patch_t * before = array[i];
			
			if(!before)
				continue;
			if(before->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
				continue;
			if(before->block && (*new)->block == before->block
			   && patch_overlap_check(*new, before))
				continue;
			
			if(before->befores)
			{
				/* Check that before's befores will not induce patch cycles */
				patch_t * before2 = before->befores->before.desc;
				/* there cannot be a cycle if overlap already depends on before
				 * or depends on all of before's befores */
				if(!quick_depends_on(overlap, before) && !quick_befores_subset(before, overlap))
				{
					/* we did not detect that overlap depends on before or its befores,
					 * so we must check before's befores for patch cycles: */
					if(before2->block && before2->block == bdesc)
						return 0;
					if(before->befores->before.next)
						return 0; /* could iterate, but it has not helped */
					if(before2->befores)
						return 0; /* could recurse, but it has not helped */
				}
			}
		}
	}

	/* could support this, but it is not necessary to do so */
	assert(!(overlap->flags & PATCH_ROLLBACK));
	
	overlap_end = overlap->offset + overlap->length;
	new_end = (*new)->offset + (*new)->length;
	merge_offset = MIN(overlap->offset, (*new)->offset);
	merge_length = MAX(overlap_end, new_end) - merge_offset;
	merge_end = merge_offset + merge_length;
	
	//printf("overlap merge %c (%u, %u)@%p to (%u, %u) for block %u\n", patch_is_rollbackable(overlap) ? ' ' : 'N', overlap->offset, overlap->length, overlap, merge_offset, merge_length, (*new)->block->number);

	for(scan = befores; scan; scan = scan->next)
	{
		size_t i, size;
		patch_t ** array;
		if(scan->size > 0)
		{
			size = scan->size;
			array = scan->array;
		}
		else
		{
			size = -scan->size;
			array = scan->list;
		}
		for(i = 0; i < size; i++)
			if(array[i] && overlap != array[i])
			{
				uint16_t flags = overlap->flags;
				overlap->flags |= PATCH_SAFE_AFTER;
				if((r = patch_add_depend(overlap, array[i])) < 0)
					return r;
				overlap->flags = flags;
			}
	}
	
	/* Restore ddesc->overlaps if new fully overlapped some patch(s).
	 * Restore before the size update to simplify error recovery. */
	if((*new)->flags & PATCH_OVERLAP)
	{
		if(patch_overlap_check(*new, overlap) == 2)
		{
			patch_link_overlap(overlap);
		}
		/* note: no need to restore fully overlapped WRITTENs or INFLIGHTs */
		(*new)->flags &= ~PATCH_OVERLAP;
	}
	
	if(merge_offset != overlap->offset || merge_length != overlap->length)
	{
		/* handle updated data size change */
		void * merge_data;
		assert(patch_is_rollbackable(overlap));

		if(merge_length <= PATCH_LOCALDATA)
			merge_data = &overlap->byte.ldata[0];
		else
		{
			if(!(merge_data = malloc(merge_length)))
			{
				if((*new)->flags & PATCH_OVERLAP)
					patch_unlink_overlap(overlap); /* XXX? */
				return -ENOMEM;
			}
			account_update_realloc(&act_data, overlap->length, merge_length);
		}
		memmove(merge_data + overlap->offset - merge_offset, overlap->byte.data, overlap->length);
		if(merge_offset < overlap->offset)
			memcpy(merge_data, &bdesc->data[merge_offset], overlap->offset - merge_offset);
		if(overlap_end < merge_end)
			memcpy(merge_data + overlap_end - merge_offset, &bdesc->data[overlap_end], merge_end - overlap_end);
		patch_free_byte_data(overlap);
		overlap->byte.data = merge_data;

		patch_unlink_overlap(overlap);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_OFFSET, overlap, merge_offset);
		overlap->offset = merge_offset;
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_LENGTH, overlap, merge_length);
		overlap->length = merge_length;
# if PATCH_BYTE_SUM
		overlap->byte.old_sum = patch_byte_sum(overlap->byte.data, merge_length);
		overlap->byte.new_sum = patch_byte_sum(&bdesc->data[merge_offset], merge_length);
# endif
		patch_link_overlap(overlap);
	}
	
	/* move merger to correct owner */
	if (overlap->owner != (*new)->owner) {
		patch_unlink_index_patches(overlap);
		overlap->owner = (*new)->owner;
		patch_link_index_patches(overlap);
	}
	
	patch_destroy(new);
	*tail = overlap;
	return 1;
}

#else

#if __GNUC__ == 4 && (__GNUC_MINOR__ == 0 || __GNUC_MINOR__ == 1)
/* gcc 4.0, and 4.1 for the Linux kernel, detect this inlining opportunity but
 * can't handle it; older versions ignore it and newer versions support it */
#define recursive_inline
#else
#define recursive_inline inline
#endif

/* Return true if after may depend on before. External callers pass depth=0. */
static recursive_inline bool patch_may_have_before(const patch_t * after, const patch_t * before, unsigned depth)
{
/* Limit the search.
 * These values do not use noticable cpu and give pretty good answers. */
#define MAX_DEPTH 10
#define MAX_DIRECT_BEFORES 10
	
	patchdep_t * dep = after->befores;
	int i = 0;
	for(; dep; dep = dep->before.next, i++)
	{
		if(i >= MAX_DIRECT_BEFORES)
			return 1;
		if(dep->before.desc == before)
			return 1;
		if(dep->before.desc->befores)
		{
			if(depth >= MAX_DEPTH)
				return 1;
			if(patch_may_have_before(dep->before.desc, before, depth + 1))
				return 1;
		}
	}
	return 0;
}

/* A simple RB merge opportunity:
 * patch has no explicit befores and has a single overlap.
 * Returns 1 on successful merge (*tail points to merged patch),
 * 0 if no merge could be made, or < 0 upon error. */
static int patch_create_byte_merge_overlap2(patch_t ** tail, BD_t *owner, patch_t *overlaps, uint32_t offset, uint32_t length, patch_pass_set_t * befores)
{
	patch_t * overlap = NULL, *o, *next;
	uint16_t overlap_end, merge_offset, merge_length, merge_end;
	patch_pass_set_t * scan;
	int r;
	
	/* determine whether we can merge new into an overlap */
	/* NOTE: if a befores[i] has a before and there are many overlaps, it may
	 * be wise to check befores[i] for befores before looking at overlaps */
	for (o = overlaps; o; o = o->tmp_next) {
		if (o->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
			continue;
		if (o->type != BYTE)
			return 0;
		if (overlap) {
#if PATCH_RB_NRB_READY
			/* TODO: does this actually require PATCH_RB_NRB_READY? */
			/* nrb depends on nothing on this block so an above is ok */
			if (o == WEAK(o->block->nrb))
				continue;
			if (overlap == WEAK(o->block->nrb)) {
				overlap = o;
				continue;
			}
#endif
			return 0;
		}
		overlap = o;
	}

	if(!overlap)
		return 0;
	
	/* Check that *new's explicit befores will not induce patch cycles */
	for(scan = befores; scan; scan = scan->next)
	{
		size_t i, size;
		patch_t ** array;
		if(scan->size > 0)
		{
			size = scan->size;
			array = scan->array;
		}
		else
		{
			size = -scan->size;
			array = scan->list;
		}
		for(i = 0; i < size; i++)
		{
			patch_t * before = array[i];
			
			if(!before)
				continue;
			if(before->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
				continue;
			/* note: overlaps are not explicitly on the list any more */
			
			if(before->befores)
			{
				/* Check that before's befores will not induce patch cycles */
				/* There cannot be a cycle if overlap already depends on before
				 * or depends on all of before's befores */
				if(!quick_depends_on(overlap, before) && !quick_befores_subset(before, overlap))
				{
					/* We did not detect that overlap depends on before or
					 * its befores, so we must check before's befores for
					 * a possible path to overlap (would-be patch cycle).
					 * Deep, newly created directory hierarchies in SU
					 * benefit from descending their dependencies. */
					if(patch_may_have_before(before, overlap, 0))
						return 0;
				}
			}
		}
	}

	/* could support this, but it is not necessary to do so */
	assert(!(overlap->flags & PATCH_ROLLBACK));
	
	/* clear overlap tmp_next entries; do this now because all error exits
	   are NOMEM (really bad, fuck semantics) */
	for (o = overlaps; o; o = next) {
		next = o->tmp_next;
		o->tmp_next = NULL;
	}
	
	overlap_end = overlap->offset + overlap->length;
	merge_offset = MIN(overlap->offset, offset);
	merge_length = MAX(overlap_end, offset + length) - merge_offset;
	merge_end = merge_offset + merge_length;
	
	//printf("overlap merge %c (%u, %u)@%p to (%u, %u) for block %u\n", patch_is_rollbackable(overlap) ? ' ' : 'N', overlap->offset, overlap->length, overlap, merge_offset, merge_length, (*new)->block->number);

	for(scan = befores; scan; scan = scan->next)
	{
		size_t i, size;
		patch_t ** array;
		if(scan->size > 0)
		{
			size = scan->size;
			array = scan->array;
		}
		else
		{
			size = -scan->size;
			array = scan->list;
		}
		for(i = 0; i < size; i++)
			if(array[i] && overlap != array[i])
			{
				uint16_t flags = overlap->flags;
				overlap->flags |= PATCH_SAFE_AFTER;
				if((r = patch_add_depend(overlap, array[i])) < 0)
					return r;
				overlap->flags = flags;
			}
	}
	
	if(merge_offset != overlap->offset || merge_length != overlap->length)
	{
		/* handle updated data size change */
		void * merge_data;
		assert(patch_is_rollbackable(overlap));

		if(merge_length <= PATCH_LOCALDATA)
			merge_data = &overlap->byte.ldata[0];
		else
		{
			if(!(merge_data = malloc(merge_length)))
				return -ENOMEM;
			account_update_realloc(&act_data, overlap->length, merge_length);
		}
		memmove(merge_data + overlap->offset - merge_offset, overlap->byte.data, overlap->length);
		if(merge_offset < overlap->offset)
			memcpy(merge_data, &bdesc_data(overlap->block)[merge_offset], overlap->offset - merge_offset);
		if(overlap_end < merge_end)
			memcpy(merge_data + overlap_end - merge_offset, &bdesc_data(overlap->block)[overlap_end], merge_end - overlap_end);
		patch_free_byte_data(overlap);
		overlap->byte.data = merge_data;

		patch_unlink_overlap(overlap);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_OFFSET, overlap, merge_offset);
		overlap->offset = merge_offset;
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_LENGTH, overlap, merge_length);
		overlap->length = merge_length;
# if PATCH_BYTE_SUM
		overlap->byte.old_sum = patch_byte_sum(overlap->byte.data, merge_length);
		overlap->byte.new_sum = patch_byte_sum(&bdesc_data(overlap->block)[merge_offset], merge_length);
# endif
		patch_link_overlap(overlap);
	}
	
	/* move merger to correct owner */
	if (overlap->owner != owner) {
		patch_unlink_index_patches(overlap);
		overlap->owner = owner;
		patch_link_index_patches(overlap);
	}

	*tail = overlap;
	return 1;
}
#endif /* !PATCH_OVERLAPS2 */
#endif /* PATCH_BYTE_MERGE_OVERLAP */

int patch_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** head)
{
	uint16_t atomic_size = owner->atomicsize;
	uint16_t init_offset = offset % atomic_size;
	uint16_t count = (length + init_offset + atomic_size - 1) / atomic_size;
	
	if(count == 1)
		return patch_create_byte(block, owner, offset, length, data, head);
	return -EINVAL;
}

/* common code to create a byte patch */
int patch_create_byte_basic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, patch_t ** tail, patch_pass_set_t * befores)
{
	bool data_required = new_patches_require_data(block);
	patch_pass_set_t * scan;
	patch_t * patch;
#if PATCH_OVERLAPS2
	patch_t * overlap_list;
#endif
	int r;
	
	assert(block && owner && tail);
	assert(offset + length <= block->length);
	
	r = patch_create_merge(block, owner, tail, befores);
	if(r < 0)
		return r;
	else if(r == 1)
		return 0;

#if PATCH_OVERLAPS2
	overlap_list = patch_find_overlaps(block, offset, length, ~0U);

	if (overlap_list) {
		r = patch_create_byte_merge_overlap2(tail, owner, overlap_list, offset, length, befores);
		if (r < 0)
			return r;
		else if (r == 1)
			return 0;
	}
#endif
	
	patch = patch_alloc();
	if(!patch)
		return -ENOMEM;
	account_npatches(BYTE, 1);	
	
	patch->owner = owner;		
	patch->block = block;
	patch->type = BYTE;
	//patch->byte.satisfy_freed = 0;
	
	if(data_required)
	{
		patch->offset = offset;
		patch->length = length;
		patch->byte.data = NULL; /* XXX Is this OK? */
	}
	else
	{
		/* Expand to cover entire block. This is safe since all patches on
		 * this block at least implicitly have all nonrollbackables as befores.
		 * Leave 'offset' and 'length' as is to copy source data. */
		patch->offset = 0;
		patch->length = block->length;
		patch->byte.data = NULL;
#if PATCH_BYTE_SUM
		patch->byte.old_sum = 0;
		patch->byte.new_sum = 0;
#endif
		//account_update(&act_nnrb, 1);
	}
	
	patch->befores = NULL;
	patch->befores_tail = &patch->befores;
	patch->afters = NULL;
	patch->afters_tail = &patch->afters;
	patch->weak_refs = NULL;
	memset(patch->nbefores, 0, sizeof(patch->nbefores));
	patch->free_prev = NULL;
	patch->free_next = NULL;
	patch->ddesc_next = NULL;
	patch->ddesc_pprev = NULL;
	patch->ddesc_ready_next = NULL;
	patch->ddesc_ready_pprev = NULL;
	patch->ddesc_index_next = NULL;
	patch->ddesc_index_pprev = NULL;
	patch->tmp_next = NULL;
	patch->tmp_pprev = NULL;
	patch->overlap_next = NULL;
	patch->overlap_pprev = NULL;
	patch->flags = PATCH_SAFE_AFTER;
		
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CREATE_BYTE, patch, block, owner, patch->offset, patch->length);
#if COUNT_PATCHES
	patch_counts[BYTE]++;
	dump_counts();
#endif
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	
	patch_link_all_patches(patch);
	patch_link_ready_patches(patch);
	patch_link_index_patches(patch);
	
	/* this is a new patch, so we don't need to check for loops. but we
	 * should check to make sure each before has not already been written. */
	for(scan = befores; scan; scan = scan->next)
	{
		size_t i, size;
		patch_t ** array;
		if(scan->size > 0)
		{
			size = scan->size;
			array = scan->array;
		}
		else
		{
			size = -scan->size;
			array = scan->list;
		}
		for(i = 0; i < size; i++)
		{
			if(array[i] && !((array[i])->flags & PATCH_WRITTEN))
				if((r = patch_add_depend_no_cycles(patch, array[i])) < 0)
				{
					patch_destroy(&patch);
					return r;
				}
		}
	}

	patch_link_overlap(patch);

#if PATCH_OVERLAPS2
	patch_apply_overlaps(patch, overlap_list);
#else
	/* make sure it is after upon any pre-existing patches */
	if((r = patch_overlap_multiattach(patch, block)) < 0)
	{
		patch_destroy(&patch);
		return r;
	}
	
# if (PATCH_BYTE_MERGE_OVERLAP && !PATCH_OVERLAPS2)
	/* after the above work towards patch to avoid multiple overlap scans */
	if(data_required)
	{
		if((r = patch_create_byte_merge_overlap(tail, &patch, befores)) < 0)
		{
			patch_destroy(&patch);
			return r;
		}
		else if(r == 1)
			return 0;
	}
# endif
#endif /* PATCH_OVERLAPS2 */
	
	if(data_required)
	{	
		void * block_data = &bdesc_data(patch->block)[offset];

		if(length <= PATCH_LOCALDATA)
			patch->byte.data = &patch->byte.ldata[0];
		else
		{
			if(!(patch->byte.data = malloc(length)))
			{
				patch_destroy(&patch);
				return -ENOMEM;
			}
			account_update(&act_data, length);
		}

		memcpy(patch->byte.data, block_data, length);
#if PATCH_BYTE_SUM
		patch->byte.new_sum = patch_byte_sum(block_data, length);
		patch->byte.old_sum = patch_byte_sum(patch->byte.data, length);
#endif
	}
	else
	{
#if PATCH_NRB
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_APPLY, patch);
		assert(!WEAK(block->nrb));
		patch_weak_retain(patch, &block->nrb, NULL, NULL);
#else
		assert(0);
#endif
	}
	
	patch->flags &= ~PATCH_SAFE_AFTER;
	*tail = patch;
	block->synthetic = 0;
	
	return 0;
}

#if PATCH_BIT_MERGE_OVERLAP
/* Quickly check whether creating head->merge may induce a cycle:
 * determine (heuristically) whether there exist patches x,y such that
 * merge->x->y and head->y and (conservatively) check that head~>merge
 * does not exist. */
static bool merge_head_dep_safe(const patch_t * head, const patch_t * merge)
{
#define MAX_WIDTH 2
	patch_t * common[MAX_WIDTH + 1] = {NULL, NULL, NULL};
	size_t common_index = 0;
	patchdep_t * head_b = head->befores;
	size_t i = 0;

	/* Find some common befores */
	for(; head_b && i < MAX_WIDTH; head_b = head_b->before.next)
	{
		patchdep_t * merge_b = merge->befores;
		size_t j = 0;
		for(; merge_b && j < MAX_WIDTH; merge_b = merge_b->before.next)
		{
			patchdep_t * merge_b_b = merge_b->before.desc->befores;
			size_t k = 0;
			for(; merge_b_b && k < MAX_WIDTH; merge_b_b = merge_b_b->before.next)
			{
				if(head_b->before.desc == merge_b_b->before.desc)
				{
					common[common_index++] = head_b->before.desc;
					if(common_index > MAX_WIDTH)
					{
						printf("%s(): More common patches found than can handle\n", __FUNCTION__);
						i = MAX_WIDTH; /* end search since 'common' is full */
					}
					goto next_head_b;
				}
			}
		}
	  next_head_b:
		(void) 0; /* placate compiler re end of compound statement labels */
	}
	if(!common_index)
		return 0;
	
	/* Check for head~>merge paths */
	for(head_b = head->befores; head_b; head_b = head_b->before.next)
	{
		patch_t * before = head_b->before.desc;
		for(i = 0; i < MAX_WIDTH; i++)
			if(before == common[i])
				break;
		if(i < MAX_WIDTH)
			continue;
		if(before == merge)
			return 0;
		if(before->befores)
		{
			if(before->befores->before.next)
				return 0;
			for(i = 0; i < MAX_WIDTH; i++)
				if(before->befores->before.desc == common[i])
					break;
			if(i == MAX_WIDTH)
				return 0;
		}
	}
	return 1;
}

/* Return whether it is safe, patch dependency wise, to merge
 * a new bit patch with the before 'head' into 'overlap'. */
static bool bit_merge_overlap_ok_head(const patch_t * head, const patch_t * overlap)
{
	if(head && head != overlap && !(head->flags & PATCH_INFLIGHT))
	{
		/* Check whether creating overlap->head may induce a cycle.
		 * If overlap->head already exists the answer is of course no: */
		if(!(overlap->befores
		     && (overlap->befores->before.desc == head
		         || (overlap->befores->before.next
		             && overlap->befores->before.next->before.desc == head))))
		{
			/* We did not detect that overlap->head already exists,
			 * so see if head->overlap cannot exist: */
			if(head->befores && !merge_head_dep_safe(head, overlap))
				return 0;
		}
	}
	return 1;
}
#endif

#if PATCH_BIT_MERGE_OVERLAP
static int patch_create_bit_merge_overlap(BD_t * owner, uint32_t xor, patch_t * bit_patches, patch_t ** head)
{
	patchdep_t * dep;
	patch_t * overlap_bit = NULL;
	patch_t * overlap_word = NULL;
	patch_t * overlap;
	uint16_t flags;
	int r;
	
	for(dep = bit_patches->befores; dep; dep = dep->before.next)
	{
		patch_t * before = dep->before.desc;
		if(before->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
			continue;
		overlap_word = overlap_word ? (patch_t *) 1 : before;
		if(xor & before->bit.or)
			overlap_bit = overlap_bit ? (patch_t *) 1 : before;
	}
	if(overlap_bit > (patch_t *) 1)
		overlap = overlap_bit;
	else if(overlap_word > (patch_t *) 1)
		overlap = overlap_word;
	else
		return 0;
	
	if(!bit_merge_overlap_ok_head(*head, overlap))
		return 0;
	
	{
		int list = 0;
		patch_t *before;

	    retry:
		for (before = overlap->block->overlap1[list]; before; before = before->overlap_next) {
#if PATCH_RB_NRB_READY
			/* NOTE: this wouldn't need PATCH_RB_NRB_READY if an NRB
			 * PATCH_OVERLAPed the underlying bits */
			/* nrb is guaranteed to not depend on overlap */
			if(before == WEAK(overlap->block->nrb))
				continue;
#endif
			if(before->flags & (PATCH_WRITTEN | PATCH_INFLIGHT))
				continue;
			if(patch_overlap_check(overlap, before))
				/* uncommon. 'before' may need a rollback update. */
				return 0;
		}

		if (list == 0 && (list = patch_overlap_list(overlap)))
			goto retry;
	}
	
	if(*head && overlap != *head)
	{
		flags = overlap->flags;
		overlap->flags |= PATCH_SAFE_AFTER;
		if((r = patch_add_depend(overlap, *head)) < 0)
			return r;
		overlap->flags = flags;
	}
	
	overlap->bit.or |= xor;
	overlap->bit.xor ^= xor;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_XOR, overlap, overlap->bit.xor);
	*((uint32_t *) (bdesc_data(overlap->block) + overlap->offset)) ^= xor;
	
	/* move merger to correct owner */
	if (overlap->owner != owner) {
		patch_unlink_index_patches(overlap);
		overlap->owner = owner;
		patch_link_index_patches(overlap);
	}
	
	*head = overlap;
	return 1;
}
#endif

#if PATCH_BIT_MERGE_OVERLAP
/* Returns whether patch has in-ram befores */
static bool has_inram_befores(const patch_t * patch)
{
	patchdep_t * dep;
	for(dep = patch->befores; dep; dep = dep->before.next)
		if(!(dep->before.desc->flags & PATCH_INFLIGHT))
			return 1;
	return 0;
}

# if PATCH_NRB
/* Returns whether patch is the only patch on its ddesc and in ram */
static bool is_sole_inram_patch(const patch_t * patch)
{
	patch_t * c;
	for(c = patch->block->all_patches; c; c = c->ddesc_next)
		if(c != patch && !(c->flags & PATCH_INFLIGHT))
			return 0;
	return 1;
}
# endif
#endif

int patch_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor, patch_t ** head)
{
	//uint32_t data = ((uint32_t *) bdesc_data(block))[offset] ^ xor;
	//return _patch_create_byte(block, owner, offset * 4, 4, (uint8_t *) &data, head);

	int r;
	bool data_required = new_patches_require_data(block);
	patch_t * patch;
	patch_t * bit_patches = NULL;
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;

	r = patch_create_merge(block, owner, head, PASS_PATCH_SET(set));
	if(r < 0)
		return r;
	else if(r == 1)
	{
		((uint32_t *) bdesc_data(block))[offset] ^= xor;
		return 0;
	}
	
	if(!data_required)
	{
		uint32_t data = ((uint32_t *) bdesc_data(block))[offset] ^ xor;
		set.array[0] = *head;
#if PATCH_NRB_MERGE_STATS
		patch_nrb_merge_stats[patch_nrb_merge_stats_idx]--; /* don't double count */
#endif
		return patch_create_byte_set(block, owner, offset << 2, 4, (uint8_t *) &data, head, PASS_PATCH_SET(set));
	}
	
#if PATCH_BIT_MERGE_OVERLAP
	bit_patches = patch_bit_patches(block, offset << 2);
	if(bit_patches && has_inram_befores(bit_patches))
	{
		r = patch_create_bit_merge_overlap(owner, xor, bit_patches, head);
		if(r < 0)
			return r;
		else if(r == 1)
			return 0;
	}
# if PATCH_NRB
	else if(WEAK(block->nrb) &&
	        is_sole_inram_patch(WEAK(block->nrb)) &&
	        bit_merge_overlap_ok_head(*head, WEAK(block->nrb)))
	{
		uint32_t data = ((uint32_t *) bdesc_data(block))[offset] ^ xor;
		DEFINE_PATCH_PASS_SET(set, 1, NULL);
		set.array[0] = *head;
		return patch_create_byte_set(block, owner, offset * 4, 4, (uint8_t *) &data, head, PASS_PATCH_SET(set));
	}
# endif
#endif
	
	patch = patch_alloc();
	if(!patch)
		return -ENOMEM;
	account_npatches(BIT, 1);
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CREATE_BIT, patch, block, owner, offset, xor);
#if COUNT_PATCHES
	patch_counts[BIT]++;
	dump_counts();
#endif
	
	patch->owner = owner;
	patch->block = block;
	patch->type = BIT;
	patch->offset = offset << 2;
	patch->length = 4;
	patch->bit.xor = xor;
	patch->bit.or = xor;
	patch->befores = NULL;
	patch->befores_tail = &patch->befores;
	patch->afters = NULL;
	patch->afters_tail = &patch->afters;
	patch->weak_refs = NULL;
	memset(patch->nbefores, 0, sizeof(patch->nbefores));
	patch->free_prev = NULL;
	patch->free_next = NULL;
	patch->ddesc_next = NULL;
	patch->ddesc_pprev = NULL;
	patch->ddesc_ready_next = NULL;
	patch->ddesc_ready_pprev = NULL;
	patch->ddesc_index_next = NULL;
	patch->ddesc_index_pprev = NULL;
	patch->tmp_next = NULL;
	patch->tmp_pprev = NULL;
	patch->overlap_next = NULL;
	patch->overlap_pprev = NULL;
	patch->flags = PATCH_SAFE_AFTER;

	patch_link_all_patches(patch);
	patch_link_ready_patches(patch);
	patch_link_index_patches(patch);
	
	/* add patch to block's befores */
	if(!bit_patches && !(bit_patches = ensure_bdesc_has_bit_patches(block, offset)))
	{
		r = -ENOMEM;
		goto error;
	}
	if((r = patch_add_depend_no_cycles(bit_patches, patch)) < 0)
		goto error;
	
	/* make sure it is after upon any pre-existing patches */
	if((r = patch_overlap_multiattach(patch, block)) < 0)
		goto error;
	
	/* this is a new patch, so we don't need to check for loops.
	 * but we should check to make sure head has not already been written. */
	if(*head && !((*head)->flags & PATCH_WRITTEN))
		if((r = patch_add_depend_no_cycles(patch, *head)) < 0)
			goto error;
	
	/* apply the change manually */
	((uint32_t *) bdesc_data(block))[offset] ^= xor;
	
	patch->flags &= ~PATCH_SAFE_AFTER;
	*head = patch;
	
	/* make sure our block sticks around */
	bdesc_retain(block);
	block->synthetic = 0;
	
	return 0;
	
  error:
	patch_destroy(&patch);
	return r;
}

#if PATCH_CYCLE_CHECK
static int patch_has_before(patch_t * after, patch_t * before)
{
	/* recursion-on-the-heap support */
	struct state {
		patch_t * after;
		patchdep_t * dep;
	};
	typedef struct state state_t;
	static state_t static_states[STATIC_STATES_CAPACITY];
	size_t states_capacity = STATIC_STATES_CAPACITY;
	state_t * states = static_states;
	state_t * state = states;
	
	int has_before = 0;
	patchdep_t * dep;
	
  recurse_enter:
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, after, PATCH_MARKED);
	after->flags |= PATCH_MARKED;
	for(dep = after->befores; dep; dep = dep->before.next)
	{
		if(dep->before.desc == before)
		{
#if PATCH_CYCLE_PRINT
			static const char * names[] = {[BIT] = "BIT", [BYTE] = "BYTE", [EMPTY] = "EMPTY"};
			state_t * scan = state;
			printf("%p[%s] <- %p[%s]", before, names[before->type], after, names[after->type]);
			do {
				scan--;
				printf(" <- %p[%s]", scan->after, names[scan->after->type]);
			} while(scan != states);
			printf("\n");
#endif
			has_before = 1;
			goto recurse_done;
		}
		if(!(dep->before.desc->flags & PATCH_MARKED))
		{
			/* Recursively check befores; equivalent to:
			 * if(patch_has_before(dep->before.desc, before))
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
		/* the patch graph is a DAG, so unmarking here would defeat the purpose */
		goto recurse_resume;
	}
	
  recurse_done:
	if(states != static_states)
		sfree(states, states_capacity * sizeof(*state));
	return has_before;
}
#endif

void patch_dep_remove(patchdep_t * dep)
{
	propagate_depend_remove(dep->after.desc, dep->before.desc);
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_REM_BEFORE, dep->after.desc, dep->before.desc);
	*dep->before.ptr = dep->before.next;
	if(dep->before.next)
		dep->before.next->before.ptr = dep->before.ptr;
	else
		dep->after.desc->befores_tail = dep->before.ptr;
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_REM_AFTER, dep->before.desc, dep->after.desc);
	*dep->after.ptr = dep->after.next;
	if(dep->after.next)
		dep->after.next->after.ptr = dep->after.ptr;
	else
		dep->before.desc->afters_tail = dep->after.ptr;
	
	if(dep->after.desc->type == EMPTY && !dep->after.desc->befores)
		/* we just removed the last before of a EMPTY patch, so satisfy it */
		patch_satisfy(&dep->after.desc);

#if 0 /* YOU_HAVE_TIME_TO_WASTE */
	memset(dep, 0, sizeof(*dep));
#endif
	patchdep_free(dep);
	account_update(&act_ndeps, -1);
}

void patch_remove_depend(patch_t * after, patch_t * before)
{
	patchdep_t * scan_befores = after->befores;
	patchdep_t * scan_afters = before->afters;
	while(scan_befores && scan_afters &&
	      scan_befores->before.desc != before &&
	      scan_afters->after.desc != after)
	{
		scan_befores = scan_befores->before.next;
		scan_afters = scan_afters->after.next;
	}
	if(scan_befores && scan_befores->before.desc == before)
		patch_dep_remove(scan_befores);
	else if(scan_afters && scan_afters->after.desc == after)
		patch_dep_remove(scan_afters);
}

#if REVISION_TAIL_INPLACE
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
#endif

int patch_apply(patch_t * patch)
{
	if(!(patch->flags & PATCH_ROLLBACK))
		return -EINVAL;
#if REVISION_TAIL_INPLACE
	switch(patch->type)
	{
		case BIT:
			*(uint32_t *) (bdesc_data(patch->block) + patch->offset) ^= patch->bit.xor;
			break;
		case BYTE:
			if(!patch->byte.data)
				return -EINVAL;
#if PATCH_BYTE_SUM
			if(patch_byte_sum(patch->byte.data, patch->length) != patch->byte.new_sum)
				printf("%s(): (%s:%d): BYTE patch %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, patch, FSTITCH_DEBUG_COUNT());
#endif
#if SWAP_FULLBLOCK_DATA
			if(patch->length == patch->block->length)
			{
				uint8_t * old_block = bdesc_data(patch->block);
				assert(!patch->offset);
				/* NOTE: these three lines need to be updated for the
				 * integrated linux-fstitch buffer cache */
				assert(patch->byte.data != patch->byte.ldata);
				patch->block->data = patch->byte.data;
				patch->byte.data = old_block;
			}
			else
#endif
				memxchg(&patch->block->data[patch->offset], patch->byte.data, patch->length);
#if PATCH_BYTE_SUM
			if(patch_byte_sum(patch->byte.data, patch->length) != patch->byte.old_sum)
				printf("%s(): (%s:%d): BYTE patch %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, patch, FSTITCH_DEBUG_COUNT());
#endif
			break;
		case EMPTY:
			/* EMPTY application is easy! */
			break;
		default:
			printf("%s(): (%s:%d): unexpected patch of type %d!\n", __FUNCTION__, __FILE__, __LINE__, patch->type);
			return -EINVAL;
	}
#endif /* REVISION_TAIL_INPLACE */
	patch->flags &= ~PATCH_ROLLBACK;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_APPLY, patch);
	return 0;
}

#if REVISION_TAIL_INPLACE
int patch_rollback(patch_t * patch)
#else
int patch_rollback(patch_t * patch, uint8_t * buffer)
#endif
{
	if(patch->flags & PATCH_ROLLBACK)
		return -EINVAL;
	switch(patch->type)
	{
		case BIT:
#if REVISION_TAIL_INPLACE
			*(uint32_t *) (patch->block->data + patch->offset) ^= patch->bit.xor;
#else
			*(uint32_t *) (buffer + patch->offset) ^= patch->bit.xor;
#endif
			break;
		case BYTE:
			if(!patch->byte.data)
				return -EINVAL;
#if PATCH_BYTE_SUM
			if(patch_byte_sum(patch->byte.data, patch->length) != patch->byte.old_sum)
				printf("%s(): (%s:%d): BYTE patch %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, patch, FSTITCH_DEBUG_COUNT());
#endif
#if SWAP_FULLBLOCK_DATA
			if(patch->length == patch->block->length)
			{
				uint8_t * new_block = patch->block->data;
				assert(!patch->offset);
				assert(patch->byte.data != patch->byte.ldata);
				patch->block->data = patch->byte.data;
				patch->byte.data = new_block;
			}
			else
#endif
#if REVISION_TAIL_INPLACE
				memxchg(&patch->block->data[patch->offset], patch->byte.data, patch->length);
#else
			memcpy(&buffer[patch->offset], patch->byte.data, patch->length);
#endif
#if PATCH_BYTE_SUM
			if(patch_byte_sum(patch->byte.data, patch->length) != patch->byte.new_sum)
				printf("%s(): (%s:%d): BYTE patch %p is corrupted! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, patch, FSTITCH_DEBUG_COUNT());
#endif
			break;
		case EMPTY:
			/* EMPTY rollback is easy! */
			break;
		default:
			printf("%s(): (%s:%d): unexpected patch of type %d!\n", __FUNCTION__, __FILE__, __LINE__, patch->type);
			return -EINVAL;
	}
	patch->flags |= PATCH_ROLLBACK;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_ROLLBACK, patch);
	return 0;
}

void patch_set_inflight(patch_t * patch)
{
	uint16_t owner_level = patch_level(patch);
#if BDESC_EXTERN_AFTER_COUNT
	patchdep_t * dep;
#endif
	
	assert(!(patch->flags & PATCH_INFLIGHT));
	assert(patch->type != EMPTY);
	
#if BDESC_EXTERN_AFTER_COUNT
	/* Pre-decrement extern_after_count to give a more useful view for
	 * optimizations (eg allow a new NRB on patch's block).
	 * propagate_depend_remove() takes this pre-decrement into account. */
	for(dep = patch->afters; dep; dep = dep->after.next)
		propagate_extern_after_change(dep->after.desc, patch, 0);
#endif
	
#if PATCH_NRB
	/* New patches cannot be merged into an inflight patch so allow
	 * for a new NRB */
	if(patch == WEAK(patch->block->nrb))
		(void) patch_weak_release(&patch->block->nrb, 0);
#endif
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, patch, PATCH_INFLIGHT);
	patch->flags |= PATCH_INFLIGHT;
	
	/* in-flight patches +1 their level to prevent afters from following */
	patch_propagate_level_change(patch, owner_level, patch_level(patch));
}

static inline void patch_weak_collect(patch_t * patch)
{
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_INFO, FDB_PATCH_WEAK_COLLECT, patch);
	while(patch->weak_refs)
	{
		assert(patch->weak_refs->patch == patch);
		assert(patch->weak_refs->pprev == &patch->weak_refs);
		patch_weak_release(patch->weak_refs, 1);
	}
}

/* satisfy a patch, i.e. remove it from all afters and add it to the list of written patches */
int patch_satisfy(patch_t ** patch)
{
	if((*patch)->flags & PATCH_WRITTEN)
	{
		printf("%s(): (%s:%d): satisfaction of already satisfied patch!\n", __FUNCTION__, __FILE__, __LINE__);
		return 0;
	}
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_INFO, FDB_PATCH_SATISFY, *patch);
	
	if((*patch)->befores)
	{
		/* We are trying to satisfy a patch with befores, which means
		 * we are writing data out of order. If it is a EMPTY, allow it
		 * silently, but otherwise this is an error. If it is a EMPTY,
		 * collect any weak references to it in case anybody is watching
		 * it to see when it gets "satisfied." */
		assert((*patch)->type == EMPTY);
	}
	else
	{
		while((*patch)->afters)
			patch_dep_remove((*patch)->afters);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, *patch, PATCH_WRITTEN);
		(*patch)->flags |= PATCH_WRITTEN;
		
		/* we don't need the data in byte patches anymore */
		if((*patch)->type == BYTE)
		{
			if((*patch)->byte.data)
			{
				patch_free_byte_data(*patch);
				(*patch)->byte.data = NULL;
				/* data == NULL does not mean "cannot be rolled back" since the patch is satisfied */
			}
		}
		
		/* make sure we're not already destroying this patch */
		if(!((*patch)->flags & PATCH_FREEING))
		{
			assert(!(*patch)->free_prev && !(*patch)->free_next);
			patch_free_push(*patch);
		}
	}
	
	patch_unlink_overlap(*patch);
	patch_unlink_index_patches(*patch);
	patch_unlink_ready_patches(*patch);
	patch_unlink_all_patches(*patch);
	
	patch_weak_collect(*patch);
	
	if((*patch)->flags & PATCH_BIT_EMPTY)
	{
		assert((*patch)->empty.bit_patches);
		assert(hash_map_find_val((*patch)->empty.bit_patches, (*patch)->empty.hash_key) == *patch);
		hash_map_erase((*patch)->empty.bit_patches, (*patch)->empty.hash_key);
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CLEAR_FLAGS, *patch, PATCH_BIT_EMPTY);
		(*patch)->flags &= ~PATCH_BIT_EMPTY;
	}
	
	*patch = NULL;
	return 0;
}

void patch_weak_retain(patch_t * patch, patchweakref_t * weak, patch_satisfy_callback_t callback, void * callback_data)
{
	if(weak->patch)
	{
		if(weak->patch == patch)
		{
#if PATCH_WEAKREF_CALLBACKS
			weak->callback = callback;
			weak->callback_data = callback_data;
#endif
			return;
		}
		else
			patch_weak_release(weak, 0);
	}
	
	if(patch)
	{
		assert(!(patch->flags & PATCH_SET_EMPTY));
		weak->patch = patch;
#if PATCH_WEAKREF_CALLBACKS
		weak->callback = callback;
		weak->callback_data = callback_data;
#endif
		weak->pprev = &patch->weak_refs;
		weak->next = patch->weak_refs;
		if(patch->weak_refs)
			patch->weak_refs->pprev = &weak->next;
		patch->weak_refs = weak;
		FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_WEAK_RETAIN, patch, weak);
	}
}

void patch_destroy(patch_t ** patch)
{
	/* were we recursively called by patch_remove_depend()? */
	if((*patch)->flags & PATCH_FREEING)
		return;
	(*patch)->flags |= PATCH_FREEING;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, *patch, PATCH_FREEING);
	
	if((*patch)->flags & PATCH_WRITTEN)
	{
		assert(!(*patch)->afters && !(*patch)->befores);
		if(free_head == *patch || (*patch)->free_prev)
			patch_free_remove(*patch);
		account_npatches((*patch)->type, -1);
	}
	else
	{
		if((*patch)->type != EMPTY)
		{
			if((*patch)->afters && (*patch)->flags & PATCH_OVERLAP)
			{
				/* this is perfectly allowed, but while we are switching to this new system, print a warning */
				printf("%s(): (%s:%d): destroying completely overlapping unwritten patch: %p!\n", __FUNCTION__, __FILE__, __LINE__, *patch);
			}
		}
		else if(free_head == *patch || (*patch)->free_prev)
		{
			assert(!(*patch)->befores);
			patch_free_remove(*patch);
		}
		account_npatches_undo((*patch)->type);
	}
	
	/* remove befores first, so patch_satisfy() won't complain */
	while((*patch)->befores)
		patch_dep_remove((*patch)->befores);
	if((*patch)->afters)
	{
		/* patch_satisfy will set it to NULL */
		patch_t * desc = *patch;
		patch_satisfy(&desc);
	}

	patch_unlink_overlap(*patch);
	patch_unlink_index_patches(*patch);
	patch_unlink_ready_patches(*patch);
	patch_unlink_all_patches(*patch);
	
	patch_weak_collect(*patch);
	
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_DESTROY, *patch);
	
	switch((*patch)->type)
	{
		case BYTE:
			/* patch_satisfy() does free 'data', but error cases may not */
			//if(!(*patch)->byte.satisfy_freed)
			{
				if(patch_is_rollbackable(*patch))
				{
					patch_free_byte_data(*patch);
					(*patch)->byte.data = NULL;
				}
				//else
				//	account_update(&act_nnrb, -1);
			}
			break;
		case EMPTY:
			if((*patch)->flags & PATCH_BIT_EMPTY)
			{
				assert((*patch)->empty.bit_patches);
				assert(hash_map_find_val((*patch)->empty.bit_patches, (*patch)->empty.hash_key) == *patch);
				hash_map_erase((*patch)->empty.bit_patches, (*patch)->empty.hash_key);
			}
			/* fall through */
		case BIT:
			break;
		default:
			printf("%s(): (%s:%d): unexpected patch of type %d!\n", __FUNCTION__, __FILE__, __LINE__, (*patch)->type);
	}
	
	if((*patch)->block)
		bdesc_release(&(*patch)->block);
	
#if COUNT_PATCHES && !COUNT_PATCHES_IS_TOTAL
	patch_counts[(*patch)->type]--;
	dump_counts();
#endif
#if 0 /* YOU_HAVE_TIME_TO_WASTE */
	memset(*patch, 0, sizeof(**patch));
#endif
	patch_free(*patch);
	*patch = NULL;
}

void patch_claim_empty(patch_t * patch)
{
	assert(patch->type == EMPTY && !patch->befores);
	assert(patch_before_level(patch) == BDLEVEL_NONE);
	if(patch->free_prev || free_head == patch)
		patch_free_remove(patch);
}

void patch_autorelease_empty(patch_t * patch)
{
	assert(patch->type == EMPTY && !patch->befores && !(patch->flags & PATCH_WRITTEN));
	assert(patch_before_level(patch) == BDLEVEL_NONE);
	while(patch->afters)
		patch_dep_remove(patch->afters);
	if(!patch->free_prev && free_head != patch)
		patch_free_push(patch);
}

void patch_set_empty_declare(patch_t * patch)
{
	assert(patch->type == EMPTY && !patch->afters && !(patch->flags & PATCH_WRITTEN));
	patch->flags |= PATCH_SET_EMPTY;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, patch, PATCH_SET_EMPTY);
	if(!patch->free_prev && free_head != patch)
		patch_free_push(patch);
}

void patch_reclaim_written(void)
{
	while(free_head)
	{
		patch_t * first = free_head;
		patch_free_remove(first);
		if(first->flags & PATCH_SET_EMPTY)
		{
			assert(first->type == EMPTY);
			assert(!first->afters);
			while(first->befores)
				patch_dep_remove(first->befores);
		}
		patch_destroy(&first);
	}
}

int patch_init(void)
{
	int r = fstitchd_register_shutdown_module(patchpools_free_all, NULL, SHUTDOWN_POSTMODULES);
	if (r < 0)
		return r;
	return account_init_all();
}
