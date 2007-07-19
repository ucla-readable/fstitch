#include <lib/platform.h>
#include <lib/hash_map.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>

#define BLOCKMAN_DEBUG 0
#define DISABLE_ORPHAN_WARNING 0

#if BLOCKMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* the length parameter is for calculating how many blocks each bdesc_t represents */
blockman_t * blockman_create(uint16_t length, BD_t * owner, destroy_notify_t destroy_notify)
{
	blockman_t * man;
	if(destroy_notify && !owner)
		return NULL;
	man = malloc(sizeof(*man));
	if(!man)
		return NULL;
	man->map = hash_map_create();
	if(!man->map)
	{
		free(man);
		return NULL;
	}
	man->length = length;
	man->owner = owner;
	man->destroy_notify = destroy_notify;
	return man;
}

void blockman_destroy(blockman_t ** blockman)
{
	hash_map_t * hash = (*blockman)->map;
	hash_map_it_t it;
	datadesc_t * ddesc;
	
	hash_map_it_init(&it, hash);
	while((ddesc = hash_map_val_next(&it)))
	{
#if !DISABLE_ORPHAN_WARNING
		if(bdesc_autorelease_poolstack_scan(ddesc) < ddesc->ref_count)
			fprintf(stderr, "%s(): (%s:%d): orphaning data descriptor %p (manager %p, #%d, count %d)!\n", __FUNCTION__, __FILE__, __LINE__, ddesc, *blockman, ddesc->managed_number, ddesc->ref_count - bdesc_autorelease_poolstack_scan(ddesc));
#endif
		ddesc->manager = NULL;
	}
	hash_map_destroy(hash);
	free(*blockman);
	*blockman = NULL;
}

int blockman_add(blockman_t * blockman, uint32_t number, datadesc_t * ddesc)
{
	int r;
	Dprintf("<blockman 0x%08x add %u: ddesc 0x%08x>\n", blockman, number, ddesc);
	
	if(ddesc->manager)
		return -EINVAL;
	
	r = hash_map_insert(blockman->map, (void *) number, ddesc);
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
	{
		hash_map_erase(ddesc->manager->map, (void *) ddesc->managed_number);
		if(ddesc->manager->destroy_notify)
			ddesc->manager->destroy_notify(ddesc->manager->owner, ddesc->managed_number, ddesc->length);
		ddesc->manager = NULL;
	}
	return 0;
}

datadesc_t * blockman_lookup(blockman_t * blockman, uint32_t number)
{
	Dprintf("<blockman 0x%08x lookup %u>\n", blockman, number);
	return (datadesc_t *) hash_map_find_val(blockman->map, (void *) number);
}

int blockman_managed_add(blockman_t * blockman, bdesc_t * bdesc)
{
	return blockman_add(blockman, bdesc->b_number, bdesc->ddesc);
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
