/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/hash_map.h>
#include <lib/pool.h>

#include <fscore/bd.h>
#include <fscore/blockman.h>
#include <fscore/bdesc.h>
#include <fscore/debug.h>
#include <fscore/fstitchd.h>
#include <fscore/patch.h>

#ifdef __KERNEL__
# include <linux/page-flags.h>
# include <linux/mm.h>
#endif

/* Statically allocate two autopools. We probably won't ever need more than the
 * main top-level one and one nested pool, and if we do, we can allocate them
 * with malloc(). */
#define STATIC_AUTO_POOLS 2

struct auto_pool {
	bdesc_t * list;
	struct auto_pool * next;
};

static struct auto_pool * autorelease_stack = NULL;
static struct auto_pool static_pool[STATIC_AUTO_POOLS];
static unsigned int autorelease_depth = 0;

DECLARE_POOL(bdesc_mem, bdesc_t);

static void bdesc_pools_free_all(void * ignore)
{
	bdesc_mem_free_all();
}

#ifdef __KERNEL__
/* make 'page' the backing page for 'bdesc'. it might be more efficient
 * to tell linux to use the current backing page? */
void bdesc_link_page(bdesc_t * bdesc, page_t * page)
{
	assert(page && bdesc->page != page);
	assert(page_count(bdesc->page) == 1);
	assert(!PageHighMem(page));
	memcpy(lowmem_page_address(page), lowmem_page_address(bdesc->page), PAGE_SIZE);
	put_page(bdesc->page);
	bdesc->page = page;
	get_page(bdesc->page);
# if MALLOC_ACCOUNT
	extern unsigned long long malloc_total, malloc_blocks;
	malloc_total += PAGE_SIZE;
	malloc_blocks += PAGE_SIZE;
# endif
}
#endif

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint32_t blocksize, uint32_t count, page_t * page)
{
	bdesc_t * bdesc = bdesc_mem_alloc();
	uint16_t i;
	if(!bdesc)
		return NULL;
#ifdef __KERNEL__
	/* NOTE: wasteful for <PAGE_SIZE (eg FS setup and UFS) */
	assert(blocksize * count <= PAGE_SIZE);
	if(page)
	{
		bdesc->page = page;
		get_page(bdesc->page);
	}
	else
	{
		bdesc->page = alloc_page(GFP_KERNEL);
		if(!bdesc->page)
		{
			bdesc_mem_free(bdesc);
			return NULL;
		}
	}
# if MALLOC_ACCOUNT
	extern unsigned long long malloc_total, malloc_blocks;
	malloc_total += PAGE_SIZE;
	malloc_blocks += PAGE_SIZE;
# endif
#else
	bdesc->_data = malloc(blocksize * count);
	if(!bdesc->_data)
	{
		bdesc_mem_free(bdesc);
		return NULL;
	}
#endif
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_ALLOC, bdesc, bdesc, number, count);
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_BDESC_NUMBER, bdesc, number, count);
	bdesc->cache_number = (uint32_t) -1;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->synthetic = 0;
	bdesc->in_flight = 0;
	bdesc->flags = 0;
	bdesc->all_patches = NULL;
	bdesc->all_patches_tail = &bdesc->all_patches;
	for(i = 0; i < NBDLEVEL; i++)
	{
		bdesc->ready_patches[i].head = NULL;
		bdesc->ready_patches[i].tail = &bdesc->ready_patches[i].head;
	}
	for(i = 0; i < NBDINDEX; i++)
	{
		bdesc->index_patches[i].head = NULL;
		bdesc->index_patches[i].tail = &bdesc->index_patches[i].head;
	}
#if BDESC_EXTERN_AFTER_COUNT
	bdesc->extern_after_count = 0;
#endif
#if PATCH_NRB
	WEAK_INIT(bdesc->nrb);
#endif
	for (i = 0; i < NOVERLAP1 + 1; i++)
		bdesc->overlap1[i] = NULL;
	bdesc->bit_patches = NULL;
	bdesc->disk_hash.pprev = NULL;
	bdesc->length = blocksize * count;
	bdesc->ddesc = bdesc; /* ha ha */
	return bdesc;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void __bdesc_release(bdesc_t *bdesc)
{
	assert(bdesc && bdesc->ref_count == 0 && bdesc->ar_count == 0);
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_DESTROY, bdesc, bdesc);
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_FREE_DDESC, bdesc, bdesc);
	assert(!bdesc->all_patches);
	assert(!bdesc->overlap1[0]);
	/* XXX don't bother checking other overlap1[] */
#if BDESC_EXTERN_AFTER_COUNT
	assert(!bdesc->extern_after_count);
#endif
#if PATCH_NRB
	assert(!WEAK(bdesc->nrb));
#endif
	int i;
	for(i = 0; i < NBDLEVEL; i++)
		assert(!bdesc->ready_patches[i].head);
	if(bdesc->bit_patches) {
		assert(hash_map_empty(bdesc->bit_patches));
		hash_map_destroy(bdesc->bit_patches);
	}
	blockman_remove(bdesc);
#ifdef __KERNEL__
	put_page(bdesc->page);
#else
	free(bdesc->_data);
#endif
	free_memset(bdesc, sizeof(*bdesc));
	bdesc_mem_free(bdesc);
}

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc)
{
	if(bdesc->ar_count == bdesc->ref_count)
	{
		fprintf(stderr, "%s(): (%s:%d): bdesc %p autorelease count would exceed reference count!\n", __FUNCTION__, __FILE__, __LINE__, bdesc);
		return bdesc;
	}
	if(!bdesc->ar_count++)
	{
		if(!autorelease_stack)
			kpanic("no current autorelease pool!");
		bdesc->ar_next = autorelease_stack->list;
		autorelease_stack->list = bdesc;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_AUTORELEASE, bdesc, bdesc, bdesc->ref_count, bdesc->ar_count);
	return bdesc;
}

/* push an autorelease pool onto the stack */
int bdesc_autorelease_pool_push(void)
{
	struct auto_pool * pool;
	if(autorelease_depth < STATIC_AUTO_POOLS)
		pool = &static_pool[autorelease_depth];
	else
		pool = malloc(sizeof(*pool));
	if(!pool)
		return -ENOMEM;
	pool->list = NULL;
	pool->next = autorelease_stack;
	autorelease_stack = pool;
	autorelease_depth++;
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_AR_POOL_PUSH, bdesc_autorelease_pool_depth());
	assert(autorelease_depth > 0);
	return 0;
}

/* pop an autorelease pool off the stack */
void bdesc_autorelease_pool_pop(void)
{
	struct auto_pool * pool = autorelease_stack;
	if(!pool)
	{
		fprintf(stderr, "%s(): (%s:%d): autorelease pool stack empty!\n", __FUNCTION__, __FILE__, __LINE__);
		return;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_AR_POOL_POP, bdesc_autorelease_pool_depth() - 1);
	while(pool->list)
	{
		bdesc_t * head = pool->list;
		int i = head->ar_count;
		pool->list = head->ar_next;
		head->ar_count = 0;
		FSTITCH_DEBUG_SEND(FDB_MODULE_BDESC, FDB_BDESC_AR_RESET, head, head, head->ref_count, head->ar_count);
		while(i-- > 0)
		{
			bdesc_t * release = head;
			bdesc_release(&release);
		}
	}
	autorelease_stack = pool->next;
	if(autorelease_depth-- > STATIC_AUTO_POOLS)
		free(pool);
}

unsigned int bdesc_autorelease_pool_depth(void)
{
	return autorelease_depth;
}

int bdesc_init(void)
{
	return fstitchd_register_shutdown_module(bdesc_pools_free_all, NULL, SHUTDOWN_POSTMODULES);
}
