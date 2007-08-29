#ifndef __FSTITCH_FSCORE_PATCH_H
#define __FSTITCH_FSCORE_PATCH_H

#define PATCH_MARKED          0x01 /* marker for graph traversal */
#define PATCH_ROLLBACK        0x02 /* patch is rolled back */
#define PATCH_WRITTEN         0x04 /* patch has been written to disk */
#define PATCH_FREEING         0x08 /* patch is being freed */
#define PATCH_DATA            0x10 /* user data change (not metadata) */
#define PATCH_BIT_EMPTY        0x20 /* bit_patches EMPTY patch */
#define PATCH_OVERLAP         0x40 /* overlaps another patch completely */
#define PATCH_SAFE_AFTER      0x80 /* add depend: assume this is a safe after */
#define PATCH_SET_EMPTY       0x100 /* EMPTY whose would-be afters get its befores instead */
#define PATCH_INFLIGHT       0x200 /* patch is being written to disk */
#define PATCH_NO_PATCHGROUP     0x400 /* patch is exempt from patchgroup tops */
#define PATCH_FULLOVERLAP    0x800 /* overlapped by current patch completely */

#define PATCH_CYCLE_CHECK 0
#define PATCH_BYTE_SUM 0

#ifndef CONSTANTS_ONLY

#include <lib/hash_map.h>
#include <fscore/bdesc.h>

#define PATCH_LOCALDATA 4

struct patch {
	BD_t * owner;
	bdesc_t * block;
	
	enum {BIT, BYTE, EMPTY} type;
	
	uint32_t flags;
	
	uint16_t offset;	/* measured in bytes */
	uint16_t length;	/* 4 for bit patches, 0 for emptys */
	
	union {
		struct {
			uint32_t xor;
			/* or allows merging */
			uint32_t or;
		} bit;
		struct {
			/* NULL data implies patch need not (and cannot) be rolled back */
			uint8_t * data;
			uint8_t ldata[PATCH_LOCALDATA];
			//uint16_t satisfy_freed;
#if PATCH_BYTE_SUM
			uint16_t old_sum, new_sum;
#endif
		} byte;
		struct {
			/* used by bit_patches EMPTYs */
			hash_map_t * bit_patches;
			void * hash_key;
		} empty;
	};
	patchdep_t * befores;
	patchdep_t ** befores_tail;

	patchdep_t * afters;
	patchdep_t ** afters_tail;

	patchweakref_t * weak_refs;

	/* nbefores[i] is the number of direct dependencies at level i */
	uint32_t nbefores[NBDLEVEL];

	/* entry in the free list */
	/* TODO: can a patch be an entry in the free list and all_patches at
	 * the same time? If not, we can unionize these. */
	patch_t * free_prev;
	patch_t * free_next;

	/* entry in the datadesc_t.all_patches list */
	/* TODO: change all_patches to be not_ready_patches so that a patch
	 * is in only one of these lists; reduces these 4 fields to 2. */
	patch_t * ddesc_next;
	patch_t ** ddesc_pprev;

	/* entry in the datadesc_t.ready_patches list */
	patch_t * ddesc_ready_next;
	patch_t ** ddesc_ready_pprev;

	/* entry in the datadesc_t.index_patches list */
	patch_t * ddesc_index_next;
	patch_t ** ddesc_index_pprev;
	
	/* entry in a temporary list */
	/* TODO: are these two and the ddesc_ready/free fields used concurrently ?
	 *       or, is tmp_pprev needed? */
	patch_t * tmp_next;
	patch_t ** tmp_pprev;

	patch_t * overlap_next;
	patch_t ** overlap_pprev;
};

struct patchdep {
	struct {
		patchdep_t ** ptr;
		patch_t * desc;
		patchdep_t * next;
	} before, after;
};

/* patch pass sets allow easy prepending of new patches to argument lists */
struct patch_pass_set {
	struct patch_pass_set * next;
	ssize_t size;
	/* must be last */
	union {
		patch_t * array[0];
		/* when size is negative, use list rather than array */
		patch_t ** list;
	};
};

#define PATCH_PASS_SET_TYPE(n) struct { patch_pass_set_t * next; ssize_t size; patch_t * array[n]; }
#define DEFINE_PATCH_PASS_SET(name, n, base) PATCH_PASS_SET_TYPE(n) name = {.next = base, .size = n}
#define PASS_PATCH_SET(set) ((patch_pass_set_t *) (void *) &(set))

int patch_init(void);

/* create new patches */
/* create a empty using a pass set */
int patch_create_empty_set(BD_t * owner, patch_t ** tail, patch_pass_set_t * befores);
/* create a empty using befores array */
int patch_create_empty_array(BD_t * owner, patch_t ** tail, size_t nbefores, patch_t * befores[]);
/* create a empty using the NULL-terminated befores var_arg */
int patch_create_empty_list(BD_t * owner, patch_t ** tail, ...);
int patch_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor, patch_t ** head);
int patch_create_byte_basic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, patch_t ** tail, patch_pass_set_t * befores);
static inline int patch_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** head) __attribute__((always_inline));
static inline int patch_create_byte_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** tail, patch_pass_set_t * befores) __attribute__((always_inline));
static inline int patch_create_init(bdesc_t * block, BD_t * owner, patch_t ** head) __attribute__((always_inline));
static inline int patch_create_full(bdesc_t * block, BD_t * owner, void * data, patch_t ** head) __attribute__((always_inline));

/* like patch_create_byte(), but guarantees to only create a single patch */
int patch_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** head);

static __inline bool patch_is_rollbackable(const patch_t * patch) __attribute__((always_inline));

/* return the maximum before BD level */
static __inline uint16_t patch_before_level(const patch_t * patch) __attribute__((always_inline));

/* return the BD level of patch_t * 'patch' */
static __inline uint16_t patch_level(const patch_t * patch) __attribute__((always_inline));

/* propagate a level change to patch->afters, from 'prev_level' to 'new_level' */
void patch_propagate_level_change(patch_t * patch, uint16_t prev_level, uint16_t new_level);

/* check whether two patches overlap, even on different blocks */
static inline int patch_overlap_check(const patch_t * a, const patch_t * b);

/* add a dependency from 'after' on 'before' */
static inline int patch_add_depend(patch_t * after, patch_t * before);
int patch_add_depend_no_cycles(patch_t * after, patch_t * before);

/* remove a dependency from 'after' on 'before' */
void patch_remove_depend(patch_t * after, patch_t * before);
/* remove the given dependency */
void patch_dep_remove(patchdep_t * dep);

#include <fscore/revision.h>

#if PATCH_BYTE_SUM && !REVISION_TAIL_INPLACE
#error PATCH_BYTE_SUM is incompatible with !REVISION_TAIL_INPLACE
#endif

/* apply and roll back patches */
int patch_apply(patch_t * patch);
#if REVISION_TAIL_INPLACE
int patch_rollback(patch_t * patch);
#else
int patch_rollback(patch_t * patch, uint8_t * buffer);
#endif

/* mark patch as inflight */
void patch_set_inflight(patch_t * patch);

/* satisfy a patch, i.e. remove it from all others that depend on it and add it to the list of written patches */
int patch_satisfy(patch_t ** patch);

/* create and remove weak references to a patch */
#if !PATCH_WEAKREF_CALLBACKS
/* these macros will adjust the function definitions and prototypes as well as callsites... */
#define patch_weak_retain(patch, weak, callback, data) patch_weak_retain(patch, weak)
#define patch_weak_release(weak, callback) patch_weak_release(weak)
#endif
void patch_weak_retain(patch_t * patch, patchweakref_t * weak, patch_satisfy_callback_t callback, void * callback_data);
static inline void patch_weak_release(patchweakref_t * weak, bool callback);
#define WEAK_INIT(weak) ((weak).patch = NULL)
#define WEAK(weak) ((weak).patch)

/* destroy a patch, actually freeing it - be careful calling this function */
void patch_destroy(patch_t ** patch);

/* remove a new EMPTY patch from the free list without adding a dependency */
void patch_claim_empty(patch_t * patch);
/* add a EMPTY patch with no dependencies back to the free list */
void patch_autorelease_empty(patch_t * patch);

/* mark a EMPTY patch as a set EMPTY: would-be afters get its befores instead */
void patch_set_empty_declare(patch_t * patch);

/* reclaim written patches, by patch_destroy() on them */
void patch_reclaim_written(void);

/* link patch into its ddesc's all_patches list */
static inline void patch_link_all_patches(patch_t * patch);
/* unlink patch from its ddesc's all_patches list */
static inline void patch_unlink_all_patches(patch_t * patch);

/* link patch into its ddesc's ready_patches list */
static inline void patch_link_ready_patches(patch_t * patch);
/* unlink patch from its ddesc's ready_patches list */
static inline void patch_unlink_ready_patches(patch_t * patch);
/* ensure patch is properly linked into/unlinked from its ddesc's ready_patches list */
static __inline void patch_update_ready_patches(patch_t * patch) __attribute__((always_inline));

/* link patch into its ddesc's index_patches list */
static inline void patch_link_index_patches(patch_t * patch);
/* unlink patch from its ddesc's index_patches list */
static inline void patch_unlink_index_patches(patch_t * patch);

void patch_tmpize_all_patches(patch_t * patch);
void patch_untmpize_all_patches(patch_t * patch);


/* Implementations of inline functions */

static __inline bool patch_is_rollbackable(const patch_t * patch)
{
	return patch->type != BYTE || patch->byte.data;
}

static __inline uint16_t patch_before_level(const patch_t * patch)
{
	int i;
	for(i = NBDLEVEL; i > 0; i--)
		if(patch->nbefores[i - 1])
			return i - 1;
	return BDLEVEL_NONE;
}

/* FIXME: INFLIGHT's l+1 can be incorrect when the module above l has multiple
 * paths to stable storage. */
static __inline uint16_t patch_level(const patch_t * patch)
{
	const patch_t * __patch = (patch);
	assert(!__patch->block || __patch->owner);
	/* in-flight patches have +1 to their level to prevent other patches from following */
	return __patch->owner ? __patch->owner->level + ((patch->flags & PATCH_INFLIGHT) ? 1 : 0): patch_before_level(__patch);
}

/* return whether patch is ready to go down one level */
/* FIXME: Can be incorrect when the below module's level differs by >1
 * (eg the current module has multiple paths to stable storage). */
static __inline bool patch_is_ready(const patch_t * patch) __attribute__((always_inline));
static __inline bool patch_is_ready(const patch_t * patch)
{
	/* empty emptys are not on blocks and so cannot be on a ready list */
	if(!patch->owner)
		return 0;
	uint16_t before_level = patch_before_level(patch);
	return before_level < patch->owner->level || before_level == BDLEVEL_NONE;
}

static __inline int patch_create_byte_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** tail, patch_pass_set_t * befores)
{
	assert(&bdesc_data(block)[offset] != data);
	int r = patch_create_byte_basic(block, owner, offset, length, tail, befores);
	if (r >= 0) {
		if (data)
			memcpy(&bdesc_data(block)[offset], data, length);
		else
			memset(&bdesc_data(block)[offset], 0, length);
	}
	return r;
}

static __inline int patch_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, patch_t ** head)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return patch_create_byte_set(block, owner, offset, length, data, head, PASS_PATCH_SET(set));
}

static __inline int patch_create_init(bdesc_t * block, BD_t * owner, patch_t ** head)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return patch_create_byte_set(block, owner, 0, block->length, NULL, head, PASS_PATCH_SET(set));
}

static __inline int patch_create_full(bdesc_t * block, BD_t * owner, void * data, patch_t ** head)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return patch_create_byte_set(block, owner, 0, block->length, data, head, PASS_PATCH_SET(set));
}

static inline void patch_link_all_patches(patch_t * patch)
{
	assert(!patch->ddesc_next && !patch->ddesc_pprev);
	if(patch->block)
	{
		bdesc_t * bdesc = patch->block;
		patch->ddesc_pprev = &bdesc->all_patches;
		patch->ddesc_next = bdesc->all_patches;
		bdesc->all_patches = patch;
		if(patch->ddesc_next)
			patch->ddesc_next->ddesc_pprev = &patch->ddesc_next;
		else
			bdesc->all_patches_tail = &patch->ddesc_next;
	}
}

static inline void patch_unlink_all_patches(patch_t * patch)
{
	if(patch->ddesc_pprev)
	{
		bdesc_t * bdesc = patch->block;
		// remove from old ddesc changes list
		if(patch->ddesc_next)
			patch->ddesc_next->ddesc_pprev = patch->ddesc_pprev;
		else
			bdesc->all_patches_tail = patch->ddesc_pprev;
		*patch->ddesc_pprev = patch->ddesc_next;
		patch->ddesc_next = NULL;
		patch->ddesc_pprev = NULL;
	}
	else
		assert(!patch->ddesc_next && !patch->ddesc_pprev);
}

#define DEFINE_LINK_CHANGES(name, index) \
static inline void patch_link_##name##_patches(patch_t * patch) \
{ \
	assert(!patch->ddesc_##name##_next && !patch->ddesc_##name##_pprev); \
	if(patch->block) \
	{ \
		bdesc_t * bdesc = patch->block; \
		patch_dlist_t * rcl = &bdesc->name##_patches[patch->owner->index]; \
		patch->ddesc_##name##_pprev = &rcl->head; \
		patch->ddesc_##name##_next = rcl->head; \
		rcl->head = patch; \
		if(patch->ddesc_##name##_next) \
			patch->ddesc_##name##_next->ddesc_##name##_pprev = &patch->ddesc_##name##_next; \
		else \
			rcl->tail = &patch->ddesc_##name##_next; \
	} \
}

#define DEFINE_UNLINK_CHANGES(name, index) \
static inline void patch_unlink_##name##_patches(patch_t * patch) \
{ \
	if(patch->ddesc_##name##_pprev) \
	{ \
		bdesc_t * bdesc = patch->block; \
		patch_dlist_t * rcl = &bdesc->name##_patches[patch->owner->index]; \
		/* remove from old ddesc changes list */ \
		if(patch->ddesc_##name##_next) \
			patch->ddesc_##name##_next->ddesc_##name##_pprev = patch->ddesc_##name##_pprev; \
		else \
			rcl->tail = patch->ddesc_##name##_pprev; \
		*patch->ddesc_##name##_pprev = patch->ddesc_##name##_next; \
		patch->ddesc_##name##_next = NULL; \
		patch->ddesc_##name##_pprev = NULL; \
	} \
	else \
		assert(!patch->ddesc_##name##_next && !patch->ddesc_##name##_pprev); \
}

/* confuse ctags */
DEFINE_LINK_CHANGES(ready, level);
DEFINE_UNLINK_CHANGES(ready, level);
DEFINE_LINK_CHANGES(index, graph_index);
DEFINE_UNLINK_CHANGES(index, graph_index);

static __inline void patch_update_ready_patches(patch_t * patch)
{
	bool is_ready = patch_is_ready(patch);
	bool is_in_ready_list = patch->ddesc_ready_pprev != NULL;
	if(is_in_ready_list)
	{
		if(!is_ready)
			patch_unlink_ready_patches(patch);
	}
	else
	{
		if(is_ready)
			patch_link_ready_patches(patch);
	}
}

#if PATCH_BYTE_SUM
/* stupid little checksum, just to try and make sure we get the same data */
static __inline uint16_t patch_byte_sum(uint8_t * data, size_t length)
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

static inline void patch_weak_release(patchweakref_t * weak, bool callback)
{
	if(weak->patch)
	{
#if PATCH_WEAKREF_CALLBACKS || FSTITCH_DEBUG
		patch_t * old = weak->patch;
#endif
		weak->patch = NULL;
		*weak->pprev = weak->next;
		if(weak->next)
			weak->next->pprev = weak->pprev;
#if PATCH_WEAKREF_CALLBACKS
		/* notice that we do not touch weak again after the
		 * callback, so it can free it if it wants */
		if(callback && weak->callback)
			weak->callback(weak, old, weak->callback_data);
#endif
		FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_WEAK_FORGET, old, weak);
	}
}

/* add a dependency between patches */
static inline int patch_add_depend(patch_t * after, patch_t * before)
{
	/* compensate for Heisenberg's uncertainty principle */
	assert(after && before);
	
	/* avoid creating a dependency loop */
#if PATCH_CYCLE_CHECK
	if(after == before || patch_has_before(before, after))
	{
		printf("%s(): (%s:%d): Avoided recursive dependency! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, FSTITCH_DEBUG_COUNT());
		assert(0);
		return -EINVAL;
	}
	/* patch_has_before() marks the DAG rooted at "before" so we must unmark it */
	patch_unmark_graph(before);
#endif
	
	return patch_add_depend_no_cycles(after, before);
}

/* CRUCIAL NOTE: does *not* check whether the patches are on the same ddesc */
/* returns 0 for no overlap, 1 for overlap, and 2 for a overlaps b completely */
static inline int patch_overlap_check(const patch_t * a, const patch_t * b)
{
	// Given that emptys have offset and length 0, don't need to check
	// for them explicitly!
	assert(a->type != EMPTY || (a->offset == 0 && a->length == 0));
	assert(b->type != EMPTY || (b->offset == 0 && b->length == 0));
	
	if(a->offset >= b->offset + b->length
	   || b->offset >= a->offset + a->length)
		return 0;
	
	/* two bit patches overlap if they modify the same bits */
	if(a->type == BIT && b->type == BIT)
	{
		assert(a->offset == b->offset);
		uint32_t shared = a->bit.or & b->bit.or;
		if(!shared)
			return 0;
		/* check for complete overlap */
		return (shared == b->bit.or) ? 2 : 1;
	}

	if (a->offset <= b->offset && a->offset + a->length >= b->offset + b->length)
		return 2;
	else
		return 1;
}

/* also include utility functions */
#include <fscore/patch_util.h>

#endif /* CONSTANTS_ONLY */

#endif /* __FSTITCH_FSCORE_PATCH_H */
