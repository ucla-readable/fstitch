#ifndef __FSTITCH_FSCORE_BDESC_H
#define __FSTITCH_FSCORE_BDESC_H

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

/* Set to allow non-rollbackable patches; these patches omit their data ptr
 * and mulitple NRBs on a given ddesc are merged into one */
/* values: 0 (disable), 1 (enable) */
#define PATCH_NRB 1
/* BDESC_EXTERN_AFTER_COUNT speeds up data omittance detection */
/* values: 0 (disable), 1 (enable) */
#define BDESC_EXTERN_AFTER_COUNT PATCH_NRB
/* Set to ensure that, for a block with a NRB, all RBs on the block depend
 * on the NRB, thereby ensuring the ready list contains only ready patches */
/* values: 0 (do not ensure), 1 (do ensure) */
#define PATCH_RB_NRB_READY (PATCH_NRB && 1)

#include <lib/hash_map.h>
#include <fscore/debug.h>
#include <fscore/bd.h>

/* reorder the queue to try and find a better flush order */
#define DIRTY_QUEUE_REORDERING 0

struct bdesc {
#ifdef __KERNEL__
	page_t * page;
#else
	uint8_t * _data;
#endif
	uint32_t length;

	unsigned in_flight : 1;
	unsigned synthetic : 1;
	unsigned flags : 30;
	
	// PATCH INFORMATION
	patch_t * all_patches;
	patch_t ** all_patches_tail;

#if BDESC_EXTERN_AFTER_COUNT
	uint32_t extern_after_count;
#endif
	
	/* For each level (at most 1 BD per level), the level's ready patches.
	 * ready patch: patch with no befores at its level or higher. */
	patch_dlist_t ready_patches[NBDLEVEL];

	/* For each graph index, the patches owned by that BD. */
	patch_dlist_t index_patches[NBDINDEX];
	
#if PATCH_NRB
	patchweakref_t nrb;
#endif

#define OVERLAP1SHIFT	5
#define NOVERLAP1	32
	patch_t *overlap1[1 + NOVERLAP1];
	
	hash_map_t * bit_patches;
	
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
bdesc_t * bdesc_alloc(uint32_t number, uint32_t blocksize, uint32_t count, page_t * page);

/* return the address of the bdesc's data content.
 * return value valid only until the bdesc's page changes. */
static inline uint8_t * bdesc_data(bdesc_t * bdesc);

/* ensure that the bdesc's backing page is 'page' */
static inline void bdesc_ensure_linked_page(bdesc_t * bdesc, page_t * page);

/* increase the reference count of a bdesc */
static inline bdesc_t * bdesc_retain(bdesc_t * bdesc);

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

#ifdef __KERNEL__
# include <linux/page-flags.h>
# include <linux/mm.h>
#endif
static inline uint8_t * bdesc_data(bdesc_t * bdesc)
{
#ifdef __KERNEL__
	assert(!PageHighMem(bdesc->page));
	return lowmem_page_address(bdesc->page);
#else
	return bdesc->_data;
#endif
}

static inline void bdesc_ensure_linked_page(bdesc_t * bdesc, page_t * page)
{
#ifdef __KERNEL__
	extern void bdesc_link_page(bdesc_t * bdesc, page_t * page);
	if(!page || bdesc->page == page)
		return;
	bdesc_link_page(bdesc, page);
#endif
}

/* increase the reference count of a bdesc */
static inline bdesc_t * bdesc_retain(bdesc_t * bdesc)
{
	bdesc->ref_count++;
	FSTITCH_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RETAIN, bdesc, bdesc, bdesc->ref_count, bdesc->ar_count);
	return bdesc;
}

static inline void bdesc_release(bdesc_t **bdp)
{
	assert((*bdp)->ref_count > (*bdp)->ar_count);
	(*bdp)->ref_count--;
	FSTITCH_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RELEASE, *bdp, *bdp, (*bdp)->ref_count, (*bdp)->ar_count);
	if (!(*bdp)->ref_count)
		__bdesc_release(*bdp);
	*bdp = NULL;
}

#endif /* CONSTANTS_ONLY */

#endif /* __FSTITCH_FSCORE_BDESC_H */
