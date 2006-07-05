#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/hash_map.h>
#include <lib/panic.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/blockman.h>
#include <kfs/bdesc.h>
#include <kfs/debug.h>

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

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint16_t length, uint16_t count)
{
	bdesc_t * bdesc = malloc(sizeof(*bdesc));
	if(!bdesc)
		return NULL;
	bdesc->ddesc = malloc(sizeof(*bdesc->ddesc));
	if(!bdesc->ddesc)
	{
		free(bdesc);
		return NULL;
	}
	length *= count;
	bdesc->ddesc->data = malloc(length);
	if(!bdesc->ddesc->data)
	{
		free(bdesc->ddesc);
		free(bdesc);
		return NULL;
	}
	bdesc->ddesc->bit_changes = hash_map_create();
	if(!bdesc->ddesc->bit_changes)
	{
		free(bdesc->ddesc->data);
		free(bdesc->ddesc);
		free(bdesc);
		return NULL;
	}
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_ALLOC, bdesc, bdesc->ddesc, number, count);
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_BDESC_NUMBER, bdesc, number, count);
	bdesc->number = number;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->count = count;
	bdesc->ddesc->ref_count = 1;
	bdesc->ddesc->all_changes = NULL;
	bdesc->ddesc->all_changes_tail = &bdesc->ddesc->all_changes;
	bdesc->ddesc->overlaps = NULL;
	bdesc->ddesc->manager = NULL;
	bdesc->ddesc->managed_number = 0;
	bdesc->ddesc->length = length;
	return bdesc;
}

/* wrap a ddesc in a new bdesc */
bdesc_t * bdesc_alloc_wrap(datadesc_t * ddesc, uint32_t number, uint16_t count)
{
	bdesc_t * bdesc = malloc(sizeof(*bdesc));
	if(!bdesc)
		return NULL;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_ALLOC_WRAP, bdesc, ddesc, number, count);
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_BDESC_NUMBER, bdesc, number, count);
	bdesc->ddesc = ddesc;
	bdesc->number = number;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->count = count;
	bdesc->ddesc->ref_count++;
	return bdesc;
}

/* make a new bdesc that shares a ddesc with another bdesc */
bdesc_t * bdesc_alloc_clone(bdesc_t * original, uint32_t number)
{
	return bdesc_alloc_wrap(original->ddesc, number, original->count);
}

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc)
{
	bdesc->ref_count++;
	bdesc->ddesc->ref_count++;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RETAIN, bdesc, bdesc->ddesc, bdesc->ref_count, bdesc->ar_count, bdesc->ddesc->ref_count);
	return bdesc;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc)
{
	(*bdesc)->ddesc->ref_count--;
	(*bdesc)->ref_count--;
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_RELEASE, *bdesc, (*bdesc)->ddesc, (*bdesc)->ref_count, (*bdesc)->ar_count, (*bdesc)->ddesc->ref_count);
	if((*bdesc)->ref_count - (*bdesc)->ar_count < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): block %p had negative reference count!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
		(*bdesc)->ddesc->ref_count -= (*bdesc)->ref_count - (*bdesc)->ar_count;
		(*bdesc)->ref_count = (*bdesc)->ar_count;
	}
	if(!(*bdesc)->ref_count)
	{
		KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_DESTROY, *bdesc, (*bdesc)->ddesc);
		if(!(*bdesc)->ddesc->ref_count)
		{
			KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_FREE_DDESC, *bdesc, (*bdesc)->ddesc);
			if((*bdesc)->ddesc->all_changes || (*bdesc)->ddesc->overlaps)
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): orphaning change descriptors for block %p!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
			if(!hash_map_empty((*bdesc)->ddesc->bit_changes))
				kdprintf(STDERR_FILENO, "%s(): (%s:%d): orphaning bit change descriptors for block %p!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
			hash_map_destroy((*bdesc)->ddesc->bit_changes);
			if((*bdesc)->ddesc->manager)
				blockman_remove((*bdesc)->ddesc);
			free((*bdesc)->ddesc->data);
			memset((*bdesc)->ddesc, 0, sizeof(*(*bdesc)->ddesc));
			free((*bdesc)->ddesc);
		}
		memset(*bdesc, 0, sizeof(**bdesc));
		free(*bdesc);
	}
	/* released, so set pointer to NULL */
	*bdesc = NULL;
}

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc)
{
	if(bdesc->ar_count == bdesc->ref_count)
	{
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): bdesc %p autorelease count would exceed reference count!\n", __FUNCTION__, __FILE__, __LINE__, bdesc);
		return bdesc;
	}
	if(!bdesc->ar_count++)
	{
		if(!autorelease_stack)
			panic("%s() called with no current autorelease pool!", __FUNCTION__);
		bdesc->ar_next = autorelease_stack->list;
		autorelease_stack->list = bdesc;
	}
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AUTORELEASE, bdesc, bdesc->ddesc, bdesc->ref_count, bdesc->ar_count, bdesc->ddesc->ref_count);
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
		return -E_NO_MEM;
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
		kdprintf(STDERR_FILENO, "%s(): (%s:%d): autorelease pool stack empty!\n", __FUNCTION__, __FILE__, __LINE__);
		return;
	}
	KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AR_POOL_POP, bdesc_autorelease_pool_depth() - 1);
	while(pool->list)
	{
		bdesc_t * head = pool->list;
		int i = head->ar_count;
		pool->list = head->ar_next;
		head->ar_count = 0;
		KFS_DEBUG_SEND(KDB_MODULE_BDESC, KDB_BDESC_AR_RESET, head, head->ddesc, head->ref_count, head->ar_count, head->ddesc->ref_count);
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

int bdesc_autorelease_poolstack_scan(datadesc_t * ddesc)
{
	int ar_count = 0;
	struct auto_pool * pool;
	for(pool = autorelease_stack; pool; pool = pool->next)
	{
		bdesc_t * scan;
		for(scan = pool->list; scan; scan = scan->ar_next)
			if(scan->ddesc == ddesc)
				ar_count += scan->ar_count;
	}
	return ar_count;
}
