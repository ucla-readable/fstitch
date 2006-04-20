#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#include <lib/types.h>
#include <lib/hash_map.h>

/* values: 0 (disable), 1 (enable), 2 (paranoid) */
#define CHDESC_BYTE_SUM 1
/* values: 0 (disable), 1 (enable) */
#define CHDESC_CYCLE_CHECK 1

struct chdesc;
typedef struct chdesc chdesc_t;

struct chmetadesc;
typedef struct chmetadesc chmetadesc_t;

struct chrefdesc;
typedef struct chrefdesc chrefdesc_t;

#include <kfs/bd.h>
#include <kfs/bdesc.h>

#define CHDESC_MARKED    0x01 /* marker for graph traversal */
#define CHDESC_INSET     0x02 /* indicator for set membership */
#define CHDESC_MOVED     0x04 /* flag for moving chdescs */
#define CHDESC_ROLLBACK  0x08 /* chdesc is rolled back */
#define CHDESC_READY     0x10 /* chdesc is ready to be written */
#define CHDESC_WRITTEN   0x20 /* chdesc has been written to disk */
#define CHDESC_FREEING   0x40 /* chdesc is being freed */
#define CHDESC_DATA      0x80 /* user data change (not metadata) */
#define CHDESC_BIT_NOOP 0x100 /* bit_changes NOOP chdesc */
#define CHDESC_OVERLAP  0x200 /* overlaps another chdesc completely */

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
	chmetadesc_t * dependencies;
	chmetadesc_t ** dependencies_tail;
	chmetadesc_t * dependents;
	chmetadesc_t ** dependents_tail;
	chrefdesc_t * weak_refs;
	chdesc_t * free_prev;
	chdesc_t * free_next;
	uint32_t stamps;
	uint16_t flags;
	uint32_t ready_epoch;
};

struct chmetadesc {
	struct {
		chmetadesc_t ** ptr;
		chdesc_t * desc;
		chmetadesc_t * next;
	} dependency, dependent;
};

struct chrefdesc {
	chdesc_t ** desc;
	chrefdesc_t * next;
};

/* create new chdescs */
chdesc_t * chdesc_create_noop(bdesc_t * block, BD_t * owner);
chdesc_t * chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor);
int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head);
int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head);
int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head);

/* like chdesc_create_byte(), but guarantees to only create a single chdesc */
int chdesc_create_byte_atomic(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head);

/* check whether two change descriptors overlap, even on different blocks */
int chdesc_overlap_check(chdesc_t * a, chdesc_t * b);

/* rewrite a byte change descriptor, if it is safe to do so */
int chdesc_rewrite_byte(chdesc_t * chdesc, uint16_t offset, uint16_t length, void * data);

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency);

/* remove a dependency from a change descriptor */
void chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency);

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

/* hidden functions for use in chdesc_util.c */
int __ensure_bdesc_has_changes(bdesc_t * block);
int __ensure_bdesc_has_overlaps(bdesc_t * block);
chdesc_t * __ensure_bdesc_has_bit_changes(bdesc_t * block, uint16_t offset);
chdesc_t * __chdesc_bit_changes(bdesc_t * block, uint16_t offset);
int __chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, bool slip_under);
int __chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency);
int __chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block);

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

/* also include utility functions */
#include <kfs/chdesc_util.h>

#endif /* __KUDOS_KFS_CHDESC_H */
