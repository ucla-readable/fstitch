#include <inc/lib.h>
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

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(BD_t * bd, uint32_t number, uint16_t offset, uint16_t length)
{
	bdesc_t * bdesc = malloc(sizeof(*bdesc));
	Dprintf("<bdesc 0x%08x alloc>\n", bdesc);
	if(!bdesc)
		return NULL;
	bdesc->data = malloc(length);
	if(!bdesc->data)
	{
		free(bdesc);
		return NULL;
	}
	bdesc->bd = bd;
	bdesc->number = number;
	bdesc->refs = 0;
	bdesc->offset = offset;
	bdesc->length = length;
	bdesc->translated = 0;
	return bdesc;
}

/* free a bdesc */
static void bdesc_free(bdesc_t * bdesc)
{
	Dprintf("<bdesc 0x%08x free>\n", bdesc);
	free(bdesc->data);
	free(bdesc);
}

/* free a bdesc if it has zero reference count */
void bdesc_drop(bdesc_t ** bdesc)
{
	Dprintf("<bdesc 0x%08x drop>\n", bdesc);
	if(!(*bdesc)->refs < 0)
	{
		Dprintf("<bdesc 0x%08x negative reference count!>\n", bdesc);
		(*bdesc)->refs = 0;
	}
	if(!(*bdesc)->refs)
	{
		bdesc_free(*bdesc);
		*bdesc = NULL;
	}
}

/* copy a bdesc, leaving the original unchanged and giving the new bdesc reference count 0 */
bdesc_t * bdesc_copy(bdesc_t * orig)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x copy>\n", orig);
	copy = bdesc_alloc(orig->bd, orig->number, orig->offset, orig->length);
	if(!copy)
		return NULL;
	memcpy(copy->data, orig->data, orig->length);
	return copy;
}

/* prepare a bdesc to be modified, copying it if it has a reference count higher than 1 */
int bdesc_touch(bdesc_t ** bdesc)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x touch>\n", *bdesc);
	if((*bdesc)->refs < 2)
		return 0;
	copy = bdesc_copy(*bdesc);
	if(!copy)
		return -1;
	/* refs is >= 2, so it can't reach 0 */
	(*bdesc)->refs--;
	copy->refs++;
	*bdesc = copy;
	return 0;
}

/* prepare the bdesc to be permanently translated ("altered") by copying it if it has nonzero reference count  */
int bdesc_alter(bdesc_t ** bdesc)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x alter>\n", *bdesc);
	if(!(*bdesc)->refs)
		return 0;
	copy = bdesc_copy(*bdesc);
	if(!copy)
		return -1;
	*bdesc = copy;
	return 0;
}

/* increase the reference count of a bdesc, copying it if it is currently translated */
int bdesc_retain(bdesc_t ** bdesc)
{
	bdesc_t * copy;
	Dprintf("<bdesc 0x%08x reference>\n", *bdesc);
	if(!(*bdesc)->translated)
	{
		(*bdesc)->refs++;
		return 0;
	}
	copy = bdesc_copy(*bdesc);
	if(!copy)
		return -1;
	copy->refs++;
	*bdesc = copy;
	return 0;
}

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc)
{
	Dprintf("<bdesc 0x%08x release>\n", *bdesc);
	if(!--(*bdesc)->refs)
	{
		bdesc_free(*bdesc);
		*bdesc = NULL;
	}
}
