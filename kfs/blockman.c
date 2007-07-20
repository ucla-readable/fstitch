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

void blockman_destroy(blockman_t **blockman)
{
	hash_map_t * hash = (*blockman)->map;
	hash_map_it_t it;
	bdesc_t * bdesc;
	
	hash_map_it_init(&it, hash);
	while((bdesc = hash_map_val_next(&it)))
	{
		bdesc->ddesc->manager = NULL;
	}
	hash_map_destroy(hash);
	free(*blockman);
	*blockman = NULL;
}

int blockman_add(blockman_t * blockman, bdesc_t *bdesc, uint32_t number)
{
	int r;
	Dprintf("<blockman 0x%08x add %u: ddesc 0x%08x>\n", blockman, number, bdesc->ddesc);
	
	if(bdesc->ddesc->manager)
		return -EINVAL;
	
	r = hash_map_insert(blockman->map, (void *) number, bdesc);
	if(r < 0)
		return r;
	
	bdesc->ddesc->manager = blockman;
	bdesc->ddesc->managed_number = number;
	
	return 0;
}

int blockman_remove(bdesc_t *bdesc)
{
	Dprintf("<blockman 0x%08x remove %u: ddesc 0x%08x>\n", blockman, bdesc->ddesc->managed_number, bdesc->ddesc);
	blockman_t *blockman = bdesc->ddesc->manager;
	assert(blockman);
	hash_map_erase(blockman->map, (void *) bdesc->ddesc->managed_number);
	if(blockman->destroy_notify)
		blockman->destroy_notify(blockman->owner, bdesc->ddesc->managed_number, bdesc->ddesc->length);
	bdesc->ddesc->manager = NULL;
	return 0;
}

bdesc_t * blockman_lookup(blockman_t * blockman, uint32_t number)
{
	Dprintf("<blockman 0x%08x lookup %u>\n", blockman, number);
	return (bdesc_t *) hash_map_find_val(blockman->map, (void *) number);
}
