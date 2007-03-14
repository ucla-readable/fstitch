#include <lib/stdlib.h>
#include <lib/hash_map.h>
#include <lib/error.h>

#include <kfs/chdesc.h>
#include <kfs/block_alloc.h>

typedef struct {
	/* clear must be the first element! */
	chdesc_t * clear;
	uint32_t block;
} alloc_record_t;

static int block_alloc_satisfy_callback(chdesc_t ** location, void * data)
{
	/* count on clear being the first element */
	alloc_record_t * record = (alloc_record_t *) location;
	block_alloc_head_t * alloc = (block_alloc_head_t *) data;
	hash_map_erase(alloc->map, (void *) record->block);
	free(record);
	/* we have freed the pointer, so return nonzero */
	return 1;
}

int block_alloc_set_freed(block_alloc_head_t * alloc, uint32_t block, chdesc_t * clear)
{
	int r;
	alloc_record_t * record = malloc(sizeof(*record));
	if(!record)
		return -E_NO_MEM;
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
	assert(record->clear);
	if(!*head)
		*head = record->clear;
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
	return alloc->map ? 0 : -E_NO_MEM;
}

void block_alloc_head_destroy(block_alloc_head_t * alloc)
{
	alloc_record_t * record;
	hash_map_it_t it;
	hash_map_it_init(&it, alloc->map);
	while((record = (alloc_record_t *) hash_map_val_next(&it)))
	{
		block_alloc_notify_alloc(alloc, record->block);
		/* behavior is undefined if we modify the hash map while using
		 * the iterator, but well-defined if we create a new iterator */
		hash_map_it_init(&it, alloc->map);
	}
	assert(!hash_map_size(alloc->map));
	hash_map_destroy(alloc->map);
	alloc->map = NULL;
}
