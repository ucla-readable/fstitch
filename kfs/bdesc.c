#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/types.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>

#define BDESC_DEBUG 0

#if BDESC_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

static bdesc_t * autorelease_list = NULL;

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(uint32_t number, uint16_t length)
{
	bdesc_t * bdesc = malloc(sizeof(*bdesc));
	Dprintf("<bdesc 0x%08x alloc>\n", bdesc);
	if(!bdesc)
		return NULL;
	bdesc->ddesc = malloc(sizeof(*bdesc->ddesc));
	if(!bdesc->ddesc)
	{
		free(bdesc);
		return NULL;
	}
	Dprintf("<bdesc 0x%08x alloc data 0x%08x>\n", bdesc, bdesc->ddesc);
	bdesc->ddesc->data = malloc(length);
	if(!bdesc->ddesc->data)
	{
		free(bdesc->ddesc);
		free(bdesc);
		return NULL;
	}
	bdesc->number = number;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->ddesc->ref_count = 1;
	bdesc->ddesc->changes = NULL;
	bdesc->ddesc->length = length;
	return bdesc;
}

/* increase the reference count of a bdesc, copying it if it is currently translated (but sharing the data) */
bdesc_t * bdesc_retain(bdesc_t * bdesc)
{
	Dprintf("<bdesc 0x%08x retain>\n", bdesc);
	bdesc->ref_count++;
	bdesc->ddesc->ref_count++;
	return bdesc;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc)
{
	Dprintf("<bdesc 0x%08x release>\n", *bdesc);
	(*bdesc)->ddesc->ref_count--;
	(*bdesc)->ref_count--;
	if((*bdesc)->ref_count < 0)
	{
		Dprintf("<bdesc 0x%08x negative reference count!>\n", *bdesc);
		(*bdesc)->ddesc->ref_count -= (*bdesc)->ref_count;
		(*bdesc)->ref_count = 0;
	}
	if(!(*bdesc)->ref_count)
	{
		Dprintf("<bdesc 0x%08x free>\n", *bdesc);
		if(!(*bdesc)->ddesc->ref_count)
		{
			Dprintf("<bdesc 0x%08x free data 0x%08x>\n", *bdesc, (*bdesc)->ddesc);
			if((*bdesc)->ddesc->changes)
				fprintf(STDERR_FILENO, "%s(): (%s:%d): orphaning change descriptors for block 0x%08x!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
			free((*bdesc)->ddesc->data);
			free((*bdesc)->ddesc);
		}
		free(*bdesc);
	}
	/* released, so set pointer to NULL */
	*bdesc = NULL;
}

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc)
{
	Dprintf("<bdesc 0x%08x autorelease>\n", bdesc);
	if(!bdesc->ar_count++)
	{
		bdesc->ar_next = autorelease_list;
		autorelease_list = bdesc;
	}
	return bdesc;
}

/* run the scheduled bdesc autoreleases */
void bdesc_run_autorelease(void)
{
	Dprintf("<bdesc run_autorelease>\n");
	while(autorelease_list)
	{
		bdesc_t * head = autorelease_list;
		int i = head->ar_count;
		autorelease_list = head->ar_next;
		head->ar_count = 0;
		while(i-- > 0)
		{
			bdesc_t * release = head;
			bdesc_release(&release);
		}
	}
}

/* make a new bdesc that shares a ddesc with another bdesc */
bdesc_t * bdesc_clone(uint32_t number, bdesc_t * original)
{
	bdesc_t * bdesc = malloc(sizeof(*bdesc));
	Dprintf("<bdesc 0x%08x clone>\n", bdesc);
	if(!bdesc)
		return NULL;
	bdesc->ddesc = original->ddesc;
	bdesc->number = number;
	bdesc->ref_count = 1;
	bdesc->ar_count = 0;
	bdesc->ar_next = NULL;
	bdesc->ddesc->ref_count++;
	return bdesc;
}

/* a function for caches and cache-like modules to use for bdesc overwriting */
/* this may no longer be needed, but it is left commented just in case */
#if 0
int bdesc_overwrite(bdesc_t * cached, bdesc_t * written)
{
	const chdesc_t * root;
	
	if(cached == written)
		return 0;
	
	root = depman_get_deps(written);
	
	if(root)
	{
		chmetadesc_t * scan;
		for(scan = root->dependencies; scan; scan = scan->next)
			chdesc_rollback(scan->desc);
	}
	
	if(memcmp(cached->ddesc->data, written->ddesc->data, cached->ddesc->length))
	{
		chdesc_t * head = NULL;
		chdesc_t * tail = NULL;
		/* FIXME check for errors */
		chdesc_create_full(cached, written->ddesc->data, &head, &tail);
		depman_add_chdesc(head);
	}
	
	if(root)
	{
		chmetadesc_t * scan;
		for(scan = root->dependencies; scan; scan = scan->next)
			chdesc_apply(scan->desc);
	}
	
	/* share the written bdesc's ddesc */
	cached->ddesc->refs -= cached->refs;
	if(cached->ddesc->refs <= 0)
	{
		free(cached->ddesc->data);
		free(cached->ddesc);
	}
	cached->ddesc = written->ddesc;
	cached->ddesc->refs += cached->refs;
	
	return depman_forward_chdesc(written, cached);
}
#endif

int bdesc_blockno_compare(const void * a, const void * b)
{
	return (*(bdesc_t **) a)->number - (*(bdesc_t **) b)->number;
}
