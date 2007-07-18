#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

/* These flags are purely for debugging, and are set only when helpful. */
#define BDESC_FLAG_BITMAP 0x0001
#define BDESC_FLAG_DIRENT 0x0002
#define BDESC_FLAG_INDIR  0x0004

#ifndef CONSTANTS_ONLY

#include <lib/hash_map.h>

struct bdesc;
typedef struct bdesc bdesc_t;

struct datadesc;
typedef struct datadesc datadesc_t;

#include <kfs/bd.h>
#include <kfs/chdesc.h>
#include <kfs/blockman.h>

struct chdesc_dlist {
	chdesc_t * head;
	chdesc_t ** tail;
};
typedef struct chdesc_dlist chdesc_dlist_t;

struct datadesc {
	uint8_t * data;
	uint32_t ref_count:30, in_flight:1, synthetic:1;

	chdesc_t * all_changes;
	chdesc_t ** all_changes_tail;

#if BDESC_EXTERN_AFTER_COUNT
	uint32_t extern_after_count;
#endif
	
	/* For each level (at most one BD per level), the level's ready chdescs.
	 * ready chdesc: chdesc with no befores at its level or higher. */
	chdesc_dlist_t ready_changes[NBDLEVEL];
	
	/* For each graph index, the chdescs owned by that BD. */
	chdesc_dlist_t index_changes[NBDINDEX];
	
#if CHDESC_NRB
	chdesc_t * nrb;
#endif

#define OVERLAP1SHIFT	5
#define NOVERLAP1	32
	chdesc_t *overlap1[1 + NOVERLAP1];
	
	hash_map_t * bit_changes;
	blockman_t * manager;
	uint32_t managed_number;
	uint16_t length, flags;
};

/* reorder the queue to try and find a better flush order */
#define DIRTY_QUEUE_REORDERING 0

struct bdesc {
	uint32_t number;
	uint32_t ref_count;
	uint32_t ar_count;
	bdesc_t * ar_next;
	datadesc_t * ddesc;
	uint16_t count;

	/* information for the write-back cache */
#if DIRTY_QUEUE_REORDERING
	uint32_t pass;
	/* if we've put a block after this one already during this pass through
	 * the dirty blocks, put further blocks after that one instead */
	uint32_t block_after_number;
	uint32_t block_after_pass;
#endif
	struct {
		struct bdesc **pprev;
		struct bdesc *next;
	} block_hash;
	struct {
		struct bdesc *prev;
		struct bdesc *next;
	} lru_all, lru_dirty;
};

int bdesc_init(void);

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint16_t length, uint16_t count);

/* wrap a ddesc in a new bdesc */
bdesc_t * bdesc_alloc_wrap(datadesc_t * ddesc, uint32_t number, uint16_t count);

/* make a new bdesc that shares a ddesc with another bdesc */
bdesc_t * bdesc_alloc_clone(bdesc_t * original, uint32_t number);

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc);

/* push an autorelease pool onto the stack */
int bdesc_autorelease_pool_push(void);

/* pop an autorelease pool off the stack */
void bdesc_autorelease_pool_pop(void);

/* get the number of autorelease pools on the stack */
unsigned int bdesc_autorelease_pool_depth(void);

/* scan the autorelease pool stack and return the total ar_count of a ddesc */
int bdesc_autorelease_poolstack_scan(datadesc_t * ddesc);

#endif /* CONSTANTS_ONLY */

#endif /* __KUDOS_KFS_BDESC_H */
