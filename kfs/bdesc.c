#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/types.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/depman.h>

#define BDESC_DEBUG 0

#if BDESC_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(BD_t * bd, uint32_t number, uint16_t offset, uint16_t length)
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
	bdesc->bd = bd;
	bdesc->number = number;
	bdesc->refs = 0;
	bdesc->offset = offset;
	bdesc->length = length;
	bdesc->ddesc->refs = 0;
	bdesc->translated = 0;
	return bdesc;
}

/* prepare a bdesc's data to be modified, copying its data if it is currently shared with another bdesc */
int bdesc_touch(bdesc_t * bdesc)
{
	datadesc_t * data;
	Dprintf("<bdesc 0x%08x touch>\n", bdesc);
	/* invariant: bdesc->refs <= bdesc->data->refs */
	if(bdesc->refs == bdesc->ddesc->refs)
		return 0;
	Dprintf("<bdesc 0x%08x copy data>\n", bdesc);
	data = malloc(sizeof(*data));
	if(!data)
		return -E_NO_MEM;
	Dprintf("<bdesc 0x%08x alloc data 0x%08x>\n", bdesc, data);
	data->data = malloc(bdesc->length);
	if(!data->data)
	{
		free(data);
		return -E_NO_MEM;
	}
	memcpy(data->data, bdesc->ddesc->data, bdesc->length);
	data->refs = bdesc->refs;
	/* bdesc->data->refs > bdesc->refs, so it won't reach 0 */
	bdesc->ddesc->refs -= bdesc->refs;
	bdesc->ddesc = data;
	return 0;
}

/* prepare the bdesc to be permanently translated ("altered") by copying it if it has nonzero reference count */
/* this should be used for block device read operations when translations are performed (i.e. any nonterminal BD) */
int bdesc_alter(bdesc_t ** bdesc)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x alter>\n", *bdesc);
	if((*bdesc)->translated)
		Dprintf("%s(): (%s:%d): unexpected translated block!\n", __FUNCTION__, __FILE__, __LINE__);
	if(!(*bdesc)->refs)
		return 0;
	Dprintf("<bdesc 0x%08x copy>\n", *bdesc);
	copy = malloc(sizeof(*copy));
	if(!copy)
		return -E_NO_MEM;
	Dprintf("<bdesc 0x%08x alloc>\n", copy);
	*copy = **bdesc;
	copy->refs = 0;
	*bdesc = copy;
	return 0;
}

/* increase the reference count of a bdesc, copying it if it is currently translated (but sharing the data) */
int bdesc_retain(bdesc_t ** bdesc)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x retain>\n", *bdesc);
	if(!(*bdesc)->translated)
	{
		(*bdesc)->refs++;
		(*bdesc)->ddesc->refs++;
		return 0;
	}
	if(!(*bdesc)->refs)
	{
		Dprintf("<bdesc 0x%08x reset translation>\n", *bdesc);
		(*bdesc)->refs = 1;
		(*bdesc)->ddesc->refs++;
		(*bdesc)->translated = 0;
		/* notify the dependency manager of the translation,
		 * even though the pointer stays the same */
		depman_forward_chdesc(*bdesc, *bdesc);
		return 0;
	}
	Dprintf("<bdesc 0x%08x copy>\n", *bdesc);
	copy = malloc(sizeof(*copy));
	if(!copy)
		return -E_NO_MEM;
	Dprintf("<bdesc 0x%08x alloc>\n", copy);
	*copy = **bdesc;
	copy->refs = 1;
	copy->ddesc->refs++;
	copy->translated = 0;
	depman_forward_chdesc(*bdesc, copy);
	*bdesc = copy;
	return 0;
}

/* free a bdesc if it has zero reference count */
void bdesc_drop(bdesc_t ** bdesc)
{
	Dprintf("<bdesc 0x%08x drop>\n", *bdesc);
	if((*bdesc)->refs < 0)
	{
		Dprintf("<bdesc 0x%08x negative reference count!>\n", *bdesc);
		(*bdesc)->ddesc->refs -= (*bdesc)->refs;
		(*bdesc)->refs = 0;
	}
	if(!(*bdesc)->refs)
	{
		Dprintf("<bdesc 0x%08x free>\n", *bdesc);
		if(depman_get_deps(*bdesc))
			fprintf(STDERR_FILENO, "%s(): (%s:%d): orphaning change descriptors for block 0x%08x!\n", __FUNCTION__, __FILE__, __LINE__, *bdesc);
		if(!(*bdesc)->ddesc->refs)
		{
			Dprintf("<bdesc 0x%08x free data 0x%08x>\n", *bdesc, (*bdesc)->ddesc);
			free((*bdesc)->ddesc->data);
			free((*bdesc)->ddesc);
		}
		free(*bdesc);
	}
	/* dropped, so set pointer to NULL */
	*bdesc = NULL;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc)
{
	Dprintf("<bdesc 0x%08x release>\n", *bdesc);
	(*bdesc)->ddesc->refs--;
	(*bdesc)->refs--;
	bdesc_drop(bdesc);
}
