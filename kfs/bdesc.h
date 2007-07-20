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

struct bdesc;
typedef struct bdesc bdesc_t;

#include <kfs/bd.h>
#include <kfs/chdesc.h>
#include <kfs/blockman.h>

struct chdesc_dlist {
	chdesc_t * head;
	chdesc_t ** tail;
};
typedef struct chdesc_dlist chdesc_dlist_t;

struct bdesc {
	uint32_t ref_count;

	uint8_t *data;
	uint32_t length;

	unsigned in_flight : 1;
	unsigned synthetic : 1;
	unsigned flags : 30;

	// CHANGE DESCRIPTOR INFORMATION
	chdesc_t * all_changes;
	chdesc_t ** all_changes_tail;

	/* For each level (at most 1 BD per level), the level's ready chdescs.
	 * ready chdesc: chdesc with no befores at its level or higher. */
	chdesc_dlist_t ready_changes[NBDLEVEL];

	// The number of changes that are not in flight. (sort of)
	int32_t nactive;
	
	/* For each level, the chdescs owned by BDs at that level. */
	chdesc_dlist_t level_changes[NBDLEVEL];

#if BDESC_EXTERN_AFTER_COUNT
	uint32_t extern_after_count;
#endif
	
#if CHDESC_NRB
	chdesc_t * nrb;
#endif

#define OVERLAP1SHIFT	5
#define NOVERLAP1	32
	chdesc_t *overlap1[1 + NOVERLAP1];
	
	hash_map_t * bit_changes;
	
	// WB CACHE INFORMATION
	uint32_t cache_number;
	struct {
		struct bdesc **pprev;
		struct bdesc *next;
	} cache_hash;
	struct {
		struct bdesc *prev;
		struct bdesc *next;
	} lru_all, lru_dirty;

	// AUTORELEASE INFORMATION
	uint32_t ar_count;
	bdesc_t * ar_next;

	// BLOCK MANAGER INFORMATION
	blockman_t * manager;
	uint32_t managed_number;
};

int bdesc_init(void);

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(uint32_t number, uint32_t nbytes);

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t *bdesc);

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

#ifndef NDEBUG
static inline void bdesc_check_level(bdesc_t *b) {
	int i, nactive = 0;
	if (b) {
		assert(b->nactive >= 0);
		for (i = 1; i < NBDLEVEL; i++)
			if (b->level_changes[i].head)
				nactive++;
		assert((b->nactive == 0) == (nactive == 0));
	}
}
#else
#define bdesc_check_level(b) /* nada */
#endif

#endif /* CONSTANTS_ONLY */

#endif /* __KUDOS_KFS_BDESC_H */
