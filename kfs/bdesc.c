#include <lib/platform.h>
#include <lib/hash_map.h>
#include <lib/pool.h>

#include <kfs/bd.h>
#include <kfs/blockman.h>
#include <kfs/bdesc.h>
#include <kfs/debug.h>
#include <kfs/kfsd.h>

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

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint32_t nbytes)
{
	bdesc_t * bdesc = bdesc_mem_alloc();
	uint16_t i;
	if(!bdesc)
		return NULL;
	bdesc->data = malloc(nbytes);
	if(!bdesc->data)
	{
		bdesc_mem_free(bdesc);
		return NULL;
	}
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_ALLOC, bdesc, bdesc, number, nbytes / 4096); /* XXXXXXX */
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_BDESC_NUMBER, bdesc, number, nbytes / 4096); /* XXXXXXX */
	bdesc->cache_number = (uint32_t) -1;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->synthetic = 0;
	bdesc->in_flight = 0;
	bdesc->need_new_changes = 0;
	bdesc->flags = 0;
	bdesc->all_changes = NULL;
	bdesc->all_changes_tail = &bdesc->all_changes;
	for(i = 0; i < NBDLEVEL; i++)
	{
		bdesc->ready_changes[i].head = NULL;
		bdesc->ready_changes[i].tail = &bdesc->ready_changes[i].head;
	}
	bdesc->nactive = 0;
#if HAVE_LEVEL_CHANGES
	for(i = 0; i < NBDLEVEL; i++)
	{
		bdesc->level_changes[i].head = NULL;
		bdesc->level_changes[i].tail = &bdesc->level_changes[i].head;
	}
#endif
	bdesc->new_changes = NULL;
#if BDESC_EXTERN_AFTER_COUNT
	bdesc->extern_after_count = 0;
#endif
#if CHDESC_NRB
	bdesc->nrb = NULL;
#endif
	for (i = 0; i < NOVERLAP1 + 1; i++)
		bdesc->overlap1[i] = NULL;
	bdesc->bit_changes = NULL;
	bdesc->disk_hash.pprev = NULL;
	bdesc->length = nbytes;
	return bdesc;
}

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc)
{
	bdesc->ref_count++;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RETAIN, bdesc, bdesc, bdesc->ref_count, bdesc->ar_count, bdesc->ref_count);
	return bdesc;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc)
{
	(*bdesc)->ref_count--;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RELEASE, *bdesc, *bdesc, (*bdesc)->ref_count, (*bdesc)->ar_count, (*bdesc)->ref_count);
	assert((*bdesc)->ref_count >= (*bdesc)->ar_count);
	if(!(*bdesc)->ref_count)
	{
		KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_DESTROY, *bdesc, *bdesc);
		uint16_t i;
		KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_FREE_DDESC, *bdesc, *bdesc);
		assert(!(*bdesc)->all_changes);
		assert(!(*bdesc)->overlap1[0]);
		/* XXX don't bother checking other overlap1[] */
#if BDESC_EXTERN_AFTER_COUNT
		assert(!(*bdesc)->extern_after_count);
#endif
#if CHDESC_NRB
		assert(!(*bdesc)->nrb);
#endif
#if 0
		if((*bdesc)->all_changes || (*bdesc)->overlap1[0]) /* XXX don't bother checking other overlap1[] */
			fprintf(stderr, "%s(): (%s:%d): orphaning change descriptors for block %p!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
#if BDESC_EXTERN_AFTER_COUNT
		if((*bdesc)->extern_after_count)
			fprintf(stderr, "%s(): (%s:%d): block still has %u external afters\n", __FUNCTION__, __FILE__, __LINE__, (*bdesc)->extern_after_count);
#endif
#if CHDESC_NRB
		if((*bdesc)->nrb)
			fprintf(stderr, "%s(): (%s:%d): block still has a NRB\n", __FUNCTION__, __FILE__, __LINE__);
#endif
#endif
		for(i = 0; i < NBDLEVEL; i++)
			assert(!(*bdesc)->ready_changes[i].head);
		if((*bdesc)->bit_changes) {
			assert(hash_map_empty((*bdesc)->bit_changes));
			hash_map_destroy((*bdesc)->bit_changes);
		}
		blockman_remove(*bdesc);
		free((*bdesc)->data);
		free_memset(*bdesc, sizeof(**bdesc));
		bdesc_mem_free(*bdesc);
	}
	/* released, so set pointer to NULL */
	*bdesc = NULL;
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
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AUTORELEASE, bdesc, bdesc, bdesc->ref_count, bdesc->ar_count, bdesc->ddesc->ref_count);
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
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AR_POOL_PUSH, bdesc_autorelease_pool_depth());
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
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AR_POOL_POP, bdesc_autorelease_pool_depth() - 1);
	while(pool->list)
	{
		bdesc_t * head = pool->list;
		int i = head->ar_count;
		pool->list = head->ar_next;
		head->ar_count = 0;
		KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AR_RESET, head, head, head->ref_count, head->ar_count, head->ddesc->ref_count);
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
	return kfsd_register_shutdown_module(bdesc_pools_free_all, NULL, SHUTDOWN_POSTMODULES);
}
