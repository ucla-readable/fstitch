#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#include <lib/types.h>
#include <lib/hash_map.h>
#include <lib/assert.h>

#define CHDESC_BYTE_SUM 0

/* Set to allow non-rollbackable chdescs; these chdescs omit their data ptr
 * and mulitple NRBs on a given ddesc are merged into one */
/* values: 0 (disable), 1 (enable) */
#define CHDESC_NRB 1
/* BDESC_EXTERN_AFTER_COUNT speeds up data omittance detection,
 * but does not yet work with chdesc_noop_reassign() */
/* values: 0 (disable), 1 (enable) */
#define BDESC_EXTERN_AFTER_COUNT CHDESC_NRB

struct chdesc;
typedef struct chdesc chdesc_t;

struct chdepdesc;
typedef struct chdepdesc chdepdesc_t;

struct chrefdesc;
typedef struct chrefdesc chrefdesc_t;

#include <kfs/bd.h>
#include <kfs/bdesc.h>

#define CHDESC_MARKED    0x01 /* marker for graph traversal */
#define CHDESC_ROLLBACK  0x02 /* chdesc is rolled back */
#define CHDESC_WRITTEN   0x04 /* chdesc has been written to disk */
#define CHDESC_FREEING   0x08 /* chdesc is being freed */
#define CHDESC_DATA      0x10 /* user data change (not metadata) */
#define CHDESC_BIT_NOOP  0x20 /* bit_changes NOOP chdesc */
#define CHDESC_OVERLAP   0x40 /* overlaps another chdesc completely */
#define CHDESC_CREATING  0x80 /* chdesc is being created */
#define CHDESC_INFLIGHT 0x100 /* chdesc is being written to disk */

/* only effective in debugging mode */
#define CHDESC_DBWAIT  0x8000 /* wait for debug mark before this gets written (in debug mode) */

struct chdesc {
	BD_t * owner;
	bdesc_t * block;
	enum {BIT, BYTE, NOOP} type;
	union {
		struct {
			/* offset is in units of 32-bit words */
			uint16_t offset;
			uint32_t xor;
		} bit;
		struct {
			/* offset is in bytes */
			uint16_t offset, length;
			/* NULL data implies chdesc need not (and cannot) be rolled back */
			uint8_t * data;
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

	chrefdesc_t * weak_refs;

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

	/* entry in a temporary list */
	/* TODO: are these two and the ddesc_ready/free fields used concurrently ?
	 *       or, is tmp_pprev needed? */
	chdesc_t * tmp_next;
	chdesc_t ** tmp_pprev;

	uint32_t stamps;
	uint16_t flags;
};

struct chdepdesc {
	struct {
		chdepdesc_t ** ptr;
		chdesc_t * desc;
		chdepdesc_t * next;
	} before, after;
};

struct chrefdesc {
	chdesc_t ** desc;
	chrefdesc_t * next;
};

/* create new chdescs */
/* create a noop using befores array */
int chdesc_create_noop_array(bdesc_t * block, BD_t * owner, chdesc_t ** tail, size_t nbefores, chdesc_t * befores[]);
/* create a noop using the NULL-terminated befores var_arg */
int chdesc_create_noop_list(bdesc_t * block, BD_t * owner, chdesc_t ** tail, ...);
int chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor, chdesc_t ** head);
int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head);
int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head);
int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head);

/* like chdesc_create_byte(), but guarantees to only create a single chdesc */
int chdesc_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head);

static __inline bool chdesc_is_rollbackable(const chdesc_t * chdesc) __attribute__((always_inline));
static __inline bool chdesc_is_rollbackable(const chdesc_t * chdesc)
{
	switch(chdesc->type)
	{
		case BYTE:
			return chdesc->byte.data != NULL;
		case BIT:
			/* BIT chdescs that are to be nonrollbackable become BYTE chdescs */
		case NOOP:
			return 1;
		default:
			kpanic("Unexpected chdesc of type %d\n", chdesc->type);
	}
}

/* return the maximum before BD level */
/* TODO: determine whether inlining affects runtime */
static __inline uint16_t chdesc_before_level(const chdesc_t * chdesc) __attribute__((always_inline));
static __inline uint16_t chdesc_before_level(const chdesc_t * chdesc)
{
	int i;
	for(i = NBDLEVEL; i > 0; i--)
		if(chdesc->nbefores[i - 1])
			return i - 1;
	return BDLEVEL_NONE;
}

/* return the BD level of chdesc_t * 'chdesc' */
/* define as a macro instead of inline function because of include orderings */
/* TODO: determine whether inlining affects runtime */
#define chdesc_level(chdesc) \
({ \
	const chdesc_t * __chdesc = (chdesc); \
	assert(!__chdesc->block || __chdesc->owner); \
	/* in-flight chdescs have +1 to their level to prevent other chdescs from following */ \
	__chdesc->owner ? __chdesc->owner->level + ((chdesc->flags & CHDESC_INFLIGHT) ? 1 : 0): chdesc_before_level(__chdesc); \
})

/* propagate a level change to chdesc->afters, from 'prev_level' to 'new_level' */
void chdesc_propagate_level_change(chdesc_t * chdesc, uint16_t prev_level, uint16_t new_level);


/* check whether two change descriptors overlap, even on different blocks */
int chdesc_overlap_check(chdesc_t * a, chdesc_t * b);

/* rewrite a byte change descriptor, if it is safe to do so */
int chdesc_rewrite_byte(chdesc_t * chdesc, uint16_t offset, uint16_t length, void * data);

/* add a dependency from 'after' on 'before' */
int chdesc_add_depend(chdesc_t * after, chdesc_t * before);

/* remove a dependency from 'after' on 'before' */
void chdesc_remove_depend(chdesc_t * after, chdesc_t * before);

/* apply and roll back change descriptors */
int chdesc_apply(chdesc_t * chdesc);
int chdesc_rollback(chdesc_t * chdesc);

/* satisfy a change descriptor, i.e. remove it from all others that depend on it and add it to the list of written chdescs */
int chdesc_satisfy(chdesc_t ** chdesc);

/* create and remove weak references to a chdesc */
int chdesc_weak_retain(chdesc_t * chdesc, chdesc_t ** location);
void chdesc_weak_forget(chdesc_t ** location);
void chdesc_weak_release(chdesc_t ** location);

/* destroy a chdesc, actually freeing it - be careful calling this function */
void chdesc_destroy(chdesc_t ** chdesc);

/* remove a new NOOP chdesc from the free list without adding a dependency */
void chdesc_claim_noop(chdesc_t * chdesc);
/* add a NOOP chdesc with no dependencies back to the free list */
void chdesc_autorelease_noop(chdesc_t * chdesc);

/* reclaim written chdescs, by chdesc_destroy() on them */
void chdesc_reclaim_written(void);

/* link chdesc into its ddesc's all_changes list */
void chdesc_link_all_changes(chdesc_t * chdesc);
/* unlink chdesc from its ddesc's all_changes list */
void chdesc_unlink_all_changes(chdesc_t * chdesc);

/* link chdesc into its ddesc's ready_changes list */
void chdesc_link_ready_changes(chdesc_t * chdesc);
/* unlink chdesc from its ddesc's ready_changes list */
void chdesc_unlink_ready_changes(chdesc_t * chdesc);
/* ensure chdesc is properly linked into/unlinked from its ddesc's ready_changes list */
void chdesc_update_ready_changes(chdesc_t * chdesc);

uint32_t chdesc_register_stamp(BD_t * bd);
void chdesc_release_stamp(uint32_t stamp);

static __inline void chdesc_stamp(chdesc_t * chdesc, uint32_t stamp) __attribute__((always_inline));
static __inline void chdesc_stamp(chdesc_t * chdesc, uint32_t stamp)
{
	chdesc->stamps |= stamp;
}

static __inline int chdesc_has_stamp(chdesc_t * chdesc, uint32_t stamp) __attribute__((always_inline));
static __inline int chdesc_has_stamp(chdesc_t * chdesc, uint32_t stamp)
{
	return chdesc->stamps & stamp;
}

void chdesc_tmpize_all_changes(chdesc_t * chdesc);
void chdesc_untmpize_all_changes(chdesc_t * chdesc);

/* also include utility functions */
#include <kfs/chdesc_util.h>

#endif /* __KUDOS_KFS_CHDESC_H */
