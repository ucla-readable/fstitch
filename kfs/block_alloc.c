#include <lib/platform.h>
#include <lib/hash_map.h>

#include <kfs/chdesc.h>
#include <kfs/block_alloc.h>

/* This library uses chdesc callbacks. But, we don't use this library yet, so
 * just disable it unless the callbacks are enabled. */
#if CHDESC_WEAKREF_CALLBACKS

typedef struct {
	/* clear must be the first element! */
	chweakref_t clear;
	uint32_t block;
} alloc_record_t;

static void block_alloc_satisfy_callback(chweakref_t * weak, chdesc_t * old, void * data)
{
	/* count on clear being the first element */
	alloc_record_t * record = (alloc_record_t *) weak;
	block_alloc_head_t * alloc = (block_alloc_head_t *) data;
	hash_map_erase(alloc->map, (void *) record->block);
	free(record);
}

int block_alloc_set_freed(block_alloc_head_t * alloc, uint32_t block, chdesc_t * clear)
{
	int r;
	alloc_record_t * record = malloc(sizeof(*record));
	if(!record)
		return -ENOMEM;
	record->block = block;
	r = chdesc_weak_retain(clear, &record->clear, block_alloc_satisfy_callback, alloc);
	if(r < 0)
	{
		free(record);
		return r;
	}
	r = hash_map_insert(alloc->map, (void *) block, record);
	if(r < 0)
	{
		chdesc_weak_release(&record->clear, 0);
		free(record);
		return r;
	}
	return 0;
}

int block_alloc_get_freed(block_alloc_head_t * alloc, uint32_t block, chdesc_t ** head)
{
	alloc_record_t * record = (alloc_record_t *) hash_map_find_val(alloc->map, (void *) block);
	if(!record)
		/* the block is not in the map, so nothing needed */
		return 0;
	assert(WEAK(record->clear));
	if(!*head)
		*head = WEAK(record->clear);
	else
	{
		chdesc_t * noop;
		int r = chdesc_create_noop_list(NULL, &noop, record->clear, *head, NULL);
		if(r < 0)
			return r;
		*head = noop;
	}
	return 0;
}

int block_alloc_notify_alloc(block_alloc_head_t * alloc, uint32_t block)
{
	alloc_record_t * record = (alloc_record_t *) hash_map_find_val(alloc->map, (void *) block);
	if(!record)
		return 0;
	hash_map_erase(alloc->map, (void *) block);
	chdesc_weak_release(&record->clear, 0);
	free(record);
	return 0;
}

int block_alloc_head_init(block_alloc_head_t * alloc)
{
	alloc->map = hash_map_create();
	return alloc->map ? 0 : -ENOMEM;
}

void block_alloc_head_destroy(block_alloc_head_t * alloc)
{
	hash_map_it2_t it = hash_map_it2_create(alloc->map);
	while(hash_map_it2_next(&it))
		block_alloc_notify_alloc(alloc, ((alloc_record_t *) it.val)->block);
	assert(!hash_map_size(alloc->map));
	hash_map_destroy(alloc->map);
	alloc->map = NULL;
}

#endif /* CHDESC_WEAKREF_CALLBACKS */
