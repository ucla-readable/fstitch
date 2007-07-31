#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#define CHDESC_MARKED          0x01 /* marker for graph traversal */
#define CHDESC_ROLLBACK        0x02 /* chdesc is rolled back */
#define CHDESC_WRITTEN         0x04 /* chdesc has been written to disk */
#define CHDESC_FREEING         0x08 /* chdesc is being freed */
#define CHDESC_DATA            0x10 /* user data change (not metadata) */
#define CHDESC_BIT_NOOP        0x20 /* bit_changes NOOP chdesc */
#define CHDESC_OVERLAP         0x40 /* overlaps another chdesc completely */
#define CHDESC_SAFE_AFTER      0x80 /* add depend: assume this is a safe after */
#define CHDESC_SET_NOOP       0x100 /* NOOP whose would-be afters get its befores instead */
#define CHDESC_INFLIGHT       0x200 /* chdesc is being written to disk */
#define CHDESC_NO_OPGROUP     0x400 /* chdesc is exempt from opgroup tops */
#define CHDESC_FULLOVERLAP    0x800 /* overlapped by current chdesc completely */

#define CHDESC_CYCLE_CHECK 0
#define CHDESC_BYTE_SUM 0

#ifndef CONSTANTS_ONLY

#include <lib/hash_map.h>
#include <kfs/bdesc.h>

#define CHDESC_LOCALDATA 4

struct chdesc {
	BD_t * owner;
	bdesc_t * block;
	
	enum {BIT, BYTE, NOOP} type;
	
	uint32_t flags;
	
	uint16_t offset;	/* measured in bytes */
	uint16_t length;	/* 4 for bit patches, 0 for noops */
	
	union {
		struct {
			uint32_t xor;
			/* or allows merging */
			uint32_t or;
		} bit;
		struct {
			/* NULL data implies chdesc need not (and cannot) be rolled back */
			uint8_t * data;
			uint8_t ldata[CHDESC_LOCALDATA];
			//uint16_t satisfy_freed;
#if CHDESC_BYTE_SUM
			uint16_t old_sum, new_sum;
#endif
		} byte;
		struct {
			/* used by bit_changes NOOPs */
			hash_map_t * bit_changes;
			void * hash_key;
		} noop;
	};
	chdepdesc_t * befores;
	chdepdesc_t ** befores_tail;

	chdepdesc_t * afters;
	chdepdesc_t ** afters_tail;

	chweakref_t * weak_refs;

	/* nbefores[i] is the number of direct dependencies at level i */
	uint32_t nbefores[NBDLEVEL];

	/* entry in the free list */
	/* TODO: can a chdesc be an entry in the free list and all_changes at
	 * the same time? If not, we can unionize these. */
	chdesc_t * free_prev;
	chdesc_t * free_next;

	/* entry in the datadesc_t.all_changes list */
	/* TODO: change all_changes to be not_ready_changes so that a chdesc
	 * is in only one of these lists; reduces these 4 fields to 2. */
	chdesc_t * ddesc_next;
	chdesc_t ** ddesc_pprev;

	/* entry in the datadesc_t.ready_changes list */
	chdesc_t * ddesc_ready_next;
	chdesc_t ** ddesc_ready_pprev;

	/* entry in the datadesc_t.index_changes list */
	chdesc_t * ddesc_index_next;
	chdesc_t ** ddesc_index_pprev;
	
	/* entry in a temporary list */
	/* TODO: are these two and the ddesc_ready/free fields used concurrently ?
	 *       or, is tmp_pprev needed? */
	chdesc_t * tmp_next;
	chdesc_t ** tmp_pprev;

	chdesc_t * overlap_next;
	chdesc_t ** overlap_pprev;
};

struct chdepdesc {
	struct {
		chdepdesc_t ** ptr;
		chdesc_t * desc;
		chdepdesc_t * next;
	} before, after;
};

/* chdesc pass sets allow easy prepending of new chdescs to argument lists */
struct chdesc_pass_set {
	struct chdesc_pass_set * next;
	ssize_t size;
	/* must be last */
	union {
		chdesc_t * array[0];
		/* when size is negative, use list rather than array */
		chdesc_t ** list;
	};
};

#define CHDESC_PASS_SET_TYPE(n) struct { chdesc_pass_set_t * next; ssize_t size; chdesc_t * array[n]; }
#define DEFINE_CHDESC_PASS_SET(name, n, base) CHDESC_PASS_SET_TYPE(n) name = {.next = base, .size = n}
#define PASS_CHDESC_SET(set) ((chdesc_pass_set_t *) (void *) &(set))

int chdesc_init(void);

/* create new chdescs */
/* create a noop using a pass set */
int chdesc_create_noop_set(BD_t * owner, chdesc_t ** tail, chdesc_pass_set_t * befores);
/* create a noop using befores array */
int chdesc_create_noop_array(BD_t * owner, chdesc_t ** tail, size_t nbefores, chdesc_t * befores[]);
/* create a noop using the NULL-terminated befores var_arg */
int chdesc_create_noop_list(BD_t * owner, chdesc_t ** tail, ...);
int chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor, chdesc_t ** head);
int chdesc_create_byte_basic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, chdesc_t ** tail, chdesc_pass_set_t * befores);
static inline int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head) __attribute__((always_inline));
static inline int chdesc_create_byte_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** tail, chdesc_pass_set_t * befores) __attribute__((always_inline));
static inline int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head) __attribute__((always_inline));
static inline int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head) __attribute__((always_inline));

/* like chdesc_create_byte(), but guarantees to only create a single chdesc */
int chdesc_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head);

static __inline bool chdesc_is_rollbackable(const chdesc_t * chdesc) __attribute__((always_inline));

/* return the maximum before BD level */
static __inline uint16_t chdesc_before_level(const chdesc_t * chdesc) __attribute__((always_inline));

/* return the BD level of chdesc_t * 'chdesc' */
static __inline uint16_t chdesc_level(const chdesc_t * chdesc) __attribute__((always_inline));

/* propagate a level change to chdesc->afters, from 'prev_level' to 'new_level' */
void chdesc_propagate_level_change(chdesc_t * chdesc, uint16_t prev_level, uint16_t new_level);

/* check whether two change descriptors overlap, even on different blocks */
static inline int chdesc_overlap_check(const chdesc_t * a, const chdesc_t * b);

/* add a dependency from 'after' on 'before' */
static inline int chdesc_add_depend(chdesc_t * after, chdesc_t * before);
int chdesc_add_depend_no_cycles(chdesc_t * after, chdesc_t * before);

/* remove a dependency from 'after' on 'before' */
void chdesc_remove_depend(chdesc_t * after, chdesc_t * before);
/* remove the given dependency */
void chdesc_dep_remove(chdepdesc_t * dep);

#include <kfs/revision.h>

#if CHDESC_BYTE_SUM && !REVISION_TAIL_INPLACE
#error CHDESC_BYTE_SUM is incompatible with !REVISION_TAIL_INPLACE
#endif

/* apply and roll back change descriptors */
int chdesc_apply(chdesc_t * chdesc);
#if REVISION_TAIL_INPLACE
int chdesc_rollback(chdesc_t * chdesc);
#else
int chdesc_rollback(chdesc_t * chdesc, uint8_t * buffer);
#endif

/* mark chdesc as inflight */
void chdesc_set_inflight(chdesc_t * chdesc);

/* satisfy a change descriptor, i.e. remove it from all others that depend on it and add it to the list of written chdescs */
int chdesc_satisfy(chdesc_t ** chdesc);

/* create and remove weak references to a chdesc */
#if !CHDESC_WEAKREF_CALLBACKS
/* these macros will adjust the function definitions and prototypes as well as callsites... */
#define chdesc_weak_retain(chdesc, weak, callback, data) chdesc_weak_retain(chdesc, weak)
#define chdesc_weak_release(weak, callback) chdesc_weak_release(weak)
#endif
void chdesc_weak_retain(chdesc_t * chdesc, chweakref_t * weak, chdesc_satisfy_callback_t callback, void * callback_data);
static inline void chdesc_weak_release(chweakref_t * weak, bool callback);
#define WEAK_INIT(weak) ((weak).chdesc = NULL)
#define WEAK(weak) ((weak).chdesc)

/* destroy a chdesc, actually freeing it - be careful calling this function */
void chdesc_destroy(chdesc_t ** chdesc);

/* remove a new NOOP chdesc from the free list without adding a dependency */
void chdesc_claim_noop(chdesc_t * chdesc);
/* add a NOOP chdesc with no dependencies back to the free list */
void chdesc_autorelease_noop(chdesc_t * chdesc);

/* mark a NOOP chdesc as a set NOOP: would-be afters get its befores instead */
void chdesc_set_noop_declare(chdesc_t * chdesc);

/* reclaim written chdescs, by chdesc_destroy() on them */
void chdesc_reclaim_written(void);

/* link chdesc into its ddesc's all_changes list */
static inline void chdesc_link_all_changes(chdesc_t * chdesc);
/* unlink chdesc from its ddesc's all_changes list */
static inline void chdesc_unlink_all_changes(chdesc_t * chdesc);

/* link chdesc into its ddesc's ready_changes list */
static inline void chdesc_link_ready_changes(chdesc_t * chdesc);
/* unlink chdesc from its ddesc's ready_changes list */
static inline void chdesc_unlink_ready_changes(chdesc_t * chdesc);
/* ensure chdesc is properly linked into/unlinked from its ddesc's ready_changes list */
static __inline void chdesc_update_ready_changes(chdesc_t * chdesc) __attribute__((always_inline));

/* link chdesc into its ddesc's index_changes list */
static inline void chdesc_link_index_changes(chdesc_t * chdesc);
/* unlink chdesc from its ddesc's index_changes list */
static inline void chdesc_unlink_index_changes(chdesc_t * chdesc);

void chdesc_tmpize_all_changes(chdesc_t * chdesc);
void chdesc_untmpize_all_changes(chdesc_t * chdesc);


/* Implementations of inline functions */

static __inline bool chdesc_is_rollbackable(const chdesc_t * chdesc)
{
	return chdesc->type != BYTE || chdesc->byte.data;
}

static __inline uint16_t chdesc_before_level(const chdesc_t * chdesc)
{
	int i;
	for(i = NBDLEVEL; i > 0; i--)
		if(chdesc->nbefores[i - 1])
			return i - 1;
	return BDLEVEL_NONE;
}

/* FIXME: INFLIGHT's l+1 can be incorrect when the module above l has multiple
 * paths to stable storage. */
static __inline uint16_t chdesc_level(const chdesc_t * chdesc)
{
	const chdesc_t * __chdesc = (chdesc);
	assert(!__chdesc->block || __chdesc->owner);
	/* in-flight chdescs have +1 to their level to prevent other chdescs from following */
	return __chdesc->owner ? __chdesc->owner->level + ((chdesc->flags & CHDESC_INFLIGHT) ? 1 : 0): chdesc_before_level(__chdesc);
}

/* return whether chdesc is ready to go down one level */
/* FIXME: Can be incorrect when the below module's level differs by >1
 * (eg the current module has multiple paths to stable storage). */
static __inline bool chdesc_is_ready(const chdesc_t * chdesc) __attribute__((always_inline));
static __inline bool chdesc_is_ready(const chdesc_t * chdesc)
{
	/* empty noops are not on blocks and so cannot be on a ready list */
	if(!chdesc->owner)
		return 0;
	uint16_t before_level = chdesc_before_level(chdesc);
	return before_level < chdesc->owner->level || before_level == BDLEVEL_NONE;
}

static __inline int chdesc_create_byte_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	assert(&block->data[offset] != data);
	int r = chdesc_create_byte_basic(block, owner, offset, length, tail, befores);
	if (r >= 0) {
		if (data)
			memcpy(&block->data[offset], data, length);
		else
			memset(&block->data[offset], 0, length);
	}
	return r;
}

static __inline int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return chdesc_create_byte_set(block, owner, offset, length, data, head, PASS_CHDESC_SET(set));
}

static __inline int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return chdesc_create_byte_set(block, owner, 0, block->length, NULL, head, PASS_CHDESC_SET(set));
}

static __inline int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return chdesc_create_byte_set(block, owner, 0, block->length, data, head, PASS_CHDESC_SET(set));
}

static inline void chdesc_link_all_changes(chdesc_t * chdesc)
{
	assert(!chdesc->ddesc_next && !chdesc->ddesc_pprev);
	if(chdesc->block)
	{
		bdesc_t * bdesc = chdesc->block;
		chdesc->ddesc_pprev = &bdesc->all_changes;
		chdesc->ddesc_next = bdesc->all_changes;
		bdesc->all_changes = chdesc;
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = &chdesc->ddesc_next;
		else
			bdesc->all_changes_tail = &chdesc->ddesc_next;
	}
}

static inline void chdesc_unlink_all_changes(chdesc_t * chdesc)
{
	if(chdesc->ddesc_pprev)
	{
		bdesc_t * bdesc = chdesc->block;
		// remove from old ddesc changes list
		if(chdesc->ddesc_next)
			chdesc->ddesc_next->ddesc_pprev = chdesc->ddesc_pprev;
		else
			bdesc->all_changes_tail = chdesc->ddesc_pprev;
		*chdesc->ddesc_pprev = chdesc->ddesc_next;
		chdesc->ddesc_next = NULL;
		chdesc->ddesc_pprev = NULL;
	}
	else
		assert(!chdesc->ddesc_next && !chdesc->ddesc_pprev);
}

#define DEFINE_LINK_CHANGES(name, index) \
static inline void chdesc_link_##name##_changes(chdesc_t * chdesc) \
{ \
	assert(!chdesc->ddesc_##name##_next && !chdesc->ddesc_##name##_pprev); \
	if(chdesc->block) \
	{ \
		bdesc_t * bdesc = chdesc->block; \
		chdesc_dlist_t * rcl = &bdesc->name##_changes[chdesc->owner->index]; \
		chdesc->ddesc_##name##_pprev = &rcl->head; \
		chdesc->ddesc_##name##_next = rcl->head; \
		rcl->head = chdesc; \
		if(chdesc->ddesc_##name##_next) \
			chdesc->ddesc_##name##_next->ddesc_##name##_pprev = &chdesc->ddesc_##name##_next; \
		else \
			rcl->tail = &chdesc->ddesc_##name##_next; \
	} \
}

#define DEFINE_UNLINK_CHANGES(name, index) \
static inline void chdesc_unlink_##name##_changes(chdesc_t * chdesc) \
{ \
	if(chdesc->ddesc_##name##_pprev) \
	{ \
		bdesc_t * bdesc = chdesc->block; \
		chdesc_dlist_t * rcl = &bdesc->name##_changes[chdesc->owner->index]; \
		/* remove from old ddesc changes list */ \
		if(chdesc->ddesc_##name##_next) \
			chdesc->ddesc_##name##_next->ddesc_##name##_pprev = chdesc->ddesc_##name##_pprev; \
		else \
			rcl->tail = chdesc->ddesc_##name##_pprev; \
		*chdesc->ddesc_##name##_pprev = chdesc->ddesc_##name##_next; \
		chdesc->ddesc_##name##_next = NULL; \
		chdesc->ddesc_##name##_pprev = NULL; \
	} \
	else \
		assert(!chdesc->ddesc_##name##_next && !chdesc->ddesc_##name##_pprev); \
}

/* confuse ctags */
DEFINE_LINK_CHANGES(ready, level);
DEFINE_UNLINK_CHANGES(ready, level);
DEFINE_LINK_CHANGES(index, graph_index);
DEFINE_UNLINK_CHANGES(index, graph_index);

static __inline void chdesc_update_ready_changes(chdesc_t * chdesc)
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

#if CHDESC_BYTE_SUM
/* stupid little checksum, just to try and make sure we get the same data */
static __inline uint16_t chdesc_byte_sum(uint8_t * data, size_t length)
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

static inline void chdesc_weak_release(chweakref_t * weak, bool callback)
{
	if(weak->chdesc)
	{
#if CHDESC_WEAKREF_CALLBACKS || KFS_DEBUG
		chdesc_t * old = weak->chdesc;
#endif
		weak->chdesc = NULL;
		*weak->pprev = weak->next;
		if(weak->next)
			weak->next->pprev = weak->pprev;
#if CHDESC_WEAKREF_CALLBACKS
		/* notice that we do not touch weak again after the
		 * callback, so it can free it if it wants */
		if(callback && weak->callback)
			weak->callback(weak, old, weak->callback_data);
#endif
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_WEAK_FORGET, old, weak);
	}
}

/* add a dependency between change descriptors */
static inline int chdesc_add_depend(chdesc_t * after, chdesc_t * before)
{
	/* compensate for Heisenberg's uncertainty principle */
	assert(after && before);
	
	/* avoid creating a dependency loop */
#if CHDESC_CYCLE_CHECK
	if(after == before || chdesc_has_before(before, after))
	{
		printf("%s(): (%s:%d): Avoided recursive dependency! (debug = %d)\n", __FUNCTION__, __FILE__, __LINE__, KFS_DEBUG_COUNT());
		assert(0);
		return -EINVAL;
	}
	/* chdesc_has_before() marks the DAG rooted at "before" so we must unmark it */
	chdesc_unmark_graph(before);
#endif
	
	return chdesc_add_depend_no_cycles(after, before);
}

/* CRUCIAL NOTE: does *not* check whether the chdescs are on the same ddesc */
/* returns 0 for no overlap, 1 for overlap, and 2 for a overlaps b completely */
static inline int chdesc_overlap_check(const chdesc_t * a, const chdesc_t * b)
{
	// Given that noops have offset and length 0, don't need to check
	// for them explicitly!
	assert(a->type != NOOP || (a->offset == 0 && a->length == 0));
	assert(b->type != NOOP || (b->offset == 0 && b->length == 0));
	
	if(a->offset >= b->offset + b->length
	   || b->offset >= a->offset + a->length)
		return 0;
	
	/* two bit chdescs overlap if they modify the same bits */
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
#include <kfs/chdesc_util.h>

#endif /* CONSTANTS_ONLY */

#endif /* __KUDOS_KFS_CHDESC_H */
