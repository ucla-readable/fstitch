#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>

#include <kfs/bdesc.h>
#include <kfs/blockman.h>

#define BLOCKMAN_DEBUG 0

#if BLOCKMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

blockman_t * blockman_create(void)
{
	return hash_map_create();
}

void blockman_destroy(blockman_t ** blockman)
{
	hash_map_t * hash = *blockman;
	hash_map_it_t it;
	datadesc_t * ddesc;
	
	hash_map_it_init(&it);
	while((ddesc = hash_map_val_next(hash, &it)))
	{
		fprintf(STDERR_FILENO, "%s(): (%s:%d): orphaning data descriptor 0x%08x (manager 0x%08x)!\n", __FUNCTION__, __FILE__, __LINE__, ddesc, *blockman);
		ddesc->manager = NULL;
	}
	hash_map_destroy(hash);
	*blockman = NULL;
}

int blockman_add(blockman_t * blockman, uint32_t number, datadesc_t * ddesc)
{
	int r;
	Dprintf("<blockman 0x%08x add %u: ddesc 0x%08x>\n", blockman, number, ddesc);
	
	if(ddesc->manager)
		return -E_INVAL;
	
	r = hash_map_insert(blockman, (void *) number, ddesc);
	if(r < 0)
		return r;
	
	ddesc->manager = blockman;
	ddesc->managed_number = number;
	
	return 0;
}

int blockman_remove(datadesc_t * ddesc)
{
	Dprintf("<blockman 0x%08x remove %u: ddesc 0x%08x>\n", blockman, ddesc->managed_number, ddesc);
	if(ddesc->manager)
		hash_map_erase(ddesc->manager, (void *) ddesc->managed_number);
	return 0;
}

datadesc_t * blockman_lookup(blockman_t * blockman, uint32_t number)
{
	Dprintf("<blockman 0x%08x lookup %u>\n", blockman, number);
	return (datadesc_t *) hash_map_find_val(blockman, (void *) number);
}

int blockman_managed_add(blockman_t * blockman, bdesc_t * bdesc)
{
	return blockman_add(blockman, bdesc->number, bdesc->ddesc);
}

bdesc_t * blockman_managed_lookup(blockman_t * blockman, uint32_t number)
{
	bdesc_t * bdesc;
	datadesc_t * ddesc = blockman_lookup(blockman, number);
	if(!ddesc)
		return NULL;
	bdesc = bdesc_alloc_wrap(ddesc, number);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	return bdesc;
}
