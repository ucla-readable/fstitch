#include <lib/platform.h>
#include <lib/hash_map.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/revision.h>
#include <kfs/crashsim_bd.h>

#ifdef __KERNEL__
#include <linux/random.h>
#else
#include <stdlib.h>
#include <limits.h>
#define random32() (rand() * (INT_MAX / RAND_MAX))
#endif

struct crashsim_info {
	BD_t my_bd;
	
	BD_t * bd;
	uint32_t threshold;
	
	bool crashed;
	uint32_t absorbed, total;
	hash_map_t * blocks;
};

static bdesc_t * crashsim_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct crashsim_info * info = (struct crashsim_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	if(info->crashed)
	{
		bdesc_t * copy = (bdesc_t *) hash_map_find_val(info->blocks, (void *) number);
		if(copy)
		{
			assert(copy->length == object->blocksize * count);
			bdesc_ensure_linked_page(copy, page);
			return copy;
		}
	}
	
	return CALL(info->bd, read_block, number, count, page);
}

static bdesc_t * crashsim_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct crashsim_info * info = (struct crashsim_info *) object;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	if(info->crashed)
	{
		bdesc_t * copy = (bdesc_t *) hash_map_find_val(info->blocks, (void *) number);
		if(copy)
		{
			assert(copy->length == object->blocksize * count);
			bdesc_ensure_linked_page(copy, page);
			return copy;
		}
	}
	
	return CALL(info->bd, synthetic_read_block, number, count, page);
}

static int crashsim_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct crashsim_info * info = (struct crashsim_info *) object;
	int value;
	
	/* make sure it's a valid block */
	assert(block->length && number + block->length / object->blocksize <= object->numblocks);

	info->total++;
	if(!info->crashed)
	{
		uint32_t rval;
		if((rval = random32()) < info->threshold)
		{
			printf("Crash simulator simulating block device crash! (%u < %u)\n", rval, info->threshold);
			info->crashed = 1;
		}
	}
	
	if(info->crashed)
	{
		if(!hash_map_find_val(info->blocks, (void *) number))
		{
			value = hash_map_insert(info->blocks, (void *) number, block);
			if(value < 0)
				return value;
			bdesc_retain(block);
		}
		
#if REVISION_TAIL_INPLACE
		value = revision_tail_prepare(block, object);
		if(value < 0)
			return value;
#else
		static uint8_t buffer[4096];
		if(block->length > sizeof(buffer))
		{
			printf("%s(): block size larger than buffer! (%d, %d)\n", __FUNCTION__, block->length, sizeof(buffer));
			return -EFAULT;
		}
		
		value = revision_tail_prepare(block, object, buffer);
		if(value < 0)
			return value;
#endif
		value = revision_tail_acknowledge(block, object);
		if(value < 0)
		{
			kpanic("revision_tail_acknowledge gave error: %d\n", value);
			return value;
		}
		
		info->absorbed++;
		return 0;
	}
	
	/* this should never fail */
	value = chdesc_push_down(block, object, info->bd);
	if(value < 0)
		return value;
	
	/* write it */
	return CALL(info->bd, write_block, block, number);
}

static int crashsim_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** crashsim_bd_get_write_head(BD_t * object)
{
	struct crashsim_info * info = (struct crashsim_info *) object;
	return CALL(info->bd, get_write_head);
}

static int32_t crashsim_bd_get_block_space(BD_t * object)
{
	struct crashsim_info * info = (struct crashsim_info *) object;
	return CALL(info->bd, get_block_space);
}

static int crashsim_bd_destroy(BD_t * bd)
{
	struct crashsim_info * info = (struct crashsim_info *) bd;
	int r = modman_rem_bd(bd);
	hash_map_it2_t it;
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	it = hash_map_it2_create(info->blocks);
	while(hash_map_it2_next(&it))
	{
		bdesc_t * block = (bdesc_t *) it.val;
		bdesc_release(&block);
	}
	hash_map_destroy(info->blocks);
	
	printf("Crash simulator absorbed %u/%u block writes\n", info->absorbed, info->total);
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

BD_t * crashsim_bd(BD_t * disk, uint32_t threshold)
{
	struct crashsim_info * info;
	BD_t * bd;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	info->blocks = hash_map_create();
	if(!info->blocks)
	{
		free(info);
		return NULL;
	}
	
	BD_INIT(bd, crashsim_bd);
	
	info->bd = disk;
	info->threshold = threshold;
	info->crashed = 0;
	info->absorbed = 0;
	info->total = 0;
	bd->blocksize = disk->blocksize;
	bd->numblocks = disk->numblocks;
	bd->atomicsize = disk->atomicsize;
	bd->level = disk->level;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	if(modman_inc_bd(disk, bd, NULL) < 0)
	{
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}
	
	printf("Crash simulator block device initialized (threshold %u)\n", threshold);
	return bd;
}
