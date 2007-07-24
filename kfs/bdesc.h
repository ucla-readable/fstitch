#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

/* These flags are purely for debugging, and are set only when helpful. */
#define BDESC_FLAG_BITMAP 0x0001
#define BDESC_FLAG_DIRENT 0x0002
#define BDESC_FLAG_INDIR  0x0004

#ifndef NDEBUG
#define free_memset(data, length) memset((data), 0, (length))
#else
#define free_memset(data, length)
#endif

#ifndef CONSTANTS_ONLY

#include <lib/hash_map.h>
#include <kfs/debug.h>

struct bdesc;
typedef struct bdesc bdesc_t;

#include <kfs/bd.h>
#include <kfs/chdesc.h>

struct chdesc_dlist {
	chdesc_t * head;
	chdesc_t ** tail;
};
typedef struct chdesc_dlist chdesc_dlist_t;

/* reorder the queue to try and find a better flush order */
#define DIRTY_QUEUE_REORDERING 0

struct bdesc {
	uint8_t *data;
	uint32_t length;

	unsigned in_flight : 1;
	unsigned synthetic : 1;
	unsigned flags : 30;
	
	// CHANGE DESCRIPTOR INFORMATION
	chdesc_t * all_changes;
	chdesc_t ** all_changes_tail;

#if BDESC_EXTERN_AFTER_COUNT
	uint32_t extern_after_count;
#endif
	
	/* For each level (at most 1 BD per level), the level's ready chdescs.
	 * ready chdesc: chdesc with no befores at its level or higher. */
	chdesc_dlist_t ready_changes[NBDLEVEL];

	/* For each graph index, the chdescs owned by that BD. */
	chdesc_dlist_t index_changes[NBDINDEX];
	
#if CHDESC_NRB
	chweakref_t nrb;
#endif

#define OVERLAP1SHIFT	5
#define NOVERLAP1	32
	chdesc_t *overlap1[1 + NOVERLAP1];
	
	hash_map_t * bit_changes;
	
	// WB CACHE INFORMATION
	uint32_t cache_number;
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

	// DISK/BLOCKMAN INFORMATION
	uint32_t disk_number;
	struct {
		struct bdesc **pprev;
		struct bdesc *next;
	} disk_hash;

	// REFCOUNT INFORMATION
	uint32_t ref_count;
	uint32_t ar_count;
	bdesc_t * ar_next;

	bdesc_t *ddesc; /* hee hee */
};

int bdesc_init(void);

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint32_t blocksize, uint32_t count);

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
static inline void bdesc_release(bdesc_t **bdp) __attribute__((always_inline));
void __bdesc_release(bdesc_t *bdesc);

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc);

/* push an autorelease pool onto the stack */
int bdesc_autorelease_pool_push(void);

/* pop an autorelease pool off the stack */
void bdesc_autorelease_pool_pop(void);

/* get the number of autorelease pools on the stack */
unsigned int bdesc_autorelease_pool_depth(void);

static inline void bdesc_release(bdesc_t **bdp)
{
	assert((*bdp)->ref_count > (*bdp)->ar_count);
	(*bdp)->ref_count--;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RELEASE, *bdp, *bdp, (*bdp)->ref_count, (*bdp)->ar_count);
	if (!(*bdp)->ref_count)
		__bdesc_release(*bdp);
	*bdp = NULL;
}

#endif /* CONSTANTS_ONLY */

#endif /* __KUDOS_KFS_BDESC_H */
