#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/hash_map.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/journal_queue_bd.h>

/* "JnlQ" */
#define JOURNAL_QUEUE_MAGIC 0x4A6E6C51

struct queue_info {
	BD_t * bd;
	hash_map_t * bdesc_hash;
	uint16_t blocksize;
	enum {RELEASE, HOLD, PASSTHROUGH} state;
};

static int journal_queue_bd_get_config(void * object, int level, char * string, size_t length)
{
	/* no configuration of interest */
	snprintf(string, length, "");
	return 0;
}

static int journal_queue_bd_get_status(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct queue_info * info = (struct queue_info *) bd->instance;
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "state: %s, blocked: %d", (info->state == RELEASE) ? "RELEASE" : (info->state == HOLD) ? "HOLD" : "PASSTHROUGH", hash_map_size(info->bdesc_hash));
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "%s", (info->state == RELEASE) ? "RELEASE" : (info->state == HOLD) ? "HOLD" : "PASSTHROUGH");
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "state: %s", (info->state == RELEASE) ? "RELEASE" : (info->state == HOLD) ? "HOLD" : "PASSTHROUGH");
	}
	return 0;
}

static uint32_t journal_queue_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct queue_info *) object->instance)->bd, get_numblocks);
}

static uint16_t journal_queue_bd_get_blocksize(BD_t * object)
{
	return ((struct queue_info *) object->instance)->blocksize;
}

static uint16_t journal_queue_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct queue_info *) object->instance)->bd, get_atomicsize);
}

static bdesc_t * journal_queue_bd_read_block(BD_t * object, uint32_t number)
{
	struct queue_info * info = (struct queue_info *) object->instance;
	bdesc_t * block;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	block = hash_map_find_val(info->bdesc_hash, (void *) number);
	if(block)
		return block;
	
	/* not in the queue, need to read it */
	block = CALL(info->bd, read_block, number);
	
	if(!block)
		return NULL;
	
	/* FIXME bdesc_alter() can fail */
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&block);
	
	/* adjust the block descriptor to match the queue */
	block->bd = object;
	
	return block;
}

static int journal_queue_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct queue_info * info = (struct queue_info *) object->instance;
	int value = 0;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	switch(info->state)
	{
		bdesc_t * bdesc;
		uint32_t refs;
		case HOLD:
			bdesc = (bdesc_t *) hash_map_find_val(info->bdesc_hash, (void *) block->number);
			if(bdesc)
			{
				overwrite:
				if(bdesc != block)
				{
					value = bdesc_overwrite(bdesc, block);
					bdesc_drop(&block);
				}
				/* don't need to drop if bdesc == block, because we know its reference count is > 0 */
			}
			else
			{
				bdesc_retain(&block);
				if((value = hash_map_insert(info->bdesc_hash, (void *) block->number, block)) < 0)
					bdesc_release(&block);
				/* ... and return below */
			}
			break;
		case PASSTHROUGH:
			bdesc = (bdesc_t *) hash_map_find_val(info->bdesc_hash, (void *) block->number);
			/* write of existing blocked block? */
			if(bdesc)
				goto overwrite;
			/* fall through */
		case RELEASE:
			refs = block->refs;
			block->translated++;
			block->bd = info->bd;
			
			/* write it */
			value = CALL(block->bd, write_block, block);
			
			if(refs)
			{
				block->bd = object;
				block->translated--;
			}
	}
	
	/* note that value is assigned on every path above */
	return value;
}

static int journal_queue_bd_sync(BD_t * object, bdesc_t * block)
{
	struct queue_info * info = (struct queue_info *) object->instance;
	uint32_t refs;
	int value;
	
	/* can't sync in the HOLD state at all */
	if(info->state == HOLD)
		return -E_INVAL;
	
	if(!block)
	{
		/* can't sync the whole device except in RELEASE state */
		if(info->state != RELEASE)
			return -E_INVAL;
		return CALL(info->bd, sync, NULL);
	}
	
	/* save reference count */
	refs = block->refs;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	if(info->state == PASSTHROUGH)
	{
		/* can't sync a held block */
		if(hash_map_find_val(info->bdesc_hash, (void *) block->number))
			return -E_INVAL;
	}
	
	block->translated++;
	block->bd = info->bd;
	
	/* sync it */
	value = CALL(block->bd, sync, block);
	
	if(refs)
	{
		block->bd = object;
		block->translated--;
	}
	
	return value;
}

static int journal_queue_bd_destroy(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) bd->instance;
	int r;
	
	if(info->state == HOLD || info->state == PASSTHROUGH)
		if((r = journal_queue_release(bd)) < 0)
			return r;
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	hash_map_destroy(info->bdesc_hash);
	free(info);
	
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t * journal_queue_bd(BD_t * disk)
{
	struct queue_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	bd->instance = info;
	
	info->bdesc_hash = hash_map_create();
	if(!info->bdesc_hash)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = JOURNAL_QUEUE_MAGIC;
	OBJASSIGN(bd, journal_queue_bd, get_config);
	OBJASSIGN(bd, journal_queue_bd, get_status);
	ASSIGN(bd, journal_queue_bd, get_numblocks);
	ASSIGN(bd, journal_queue_bd, get_blocksize);
	ASSIGN(bd, journal_queue_bd, get_atomicsize);
	ASSIGN(bd, journal_queue_bd, read_block);
	ASSIGN(bd, journal_queue_bd, write_block);
	ASSIGN(bd, journal_queue_bd, sync);
	DESTRUCTOR(bd, journal_queue_bd, destroy);

	info->bd = disk;
	info->blocksize = CALL(disk, get_blocksize);
	info->state = RELEASE;
	
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
	
	return bd;
}

bool journal_queue_detect(BD_t * bd)
{
	return OBJMAGIC(bd) == JOURNAL_QUEUE_MAGIC;
}

int journal_queue_release(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) bd->instance;
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	if(info->state != RELEASE)
	{
		bdesc_t * bdesc;
		hash_map_it_t * it = hash_map_it_create();
		if(!it)
			return -E_NO_MEM;
		while((bdesc = (bdesc_t *) hash_map_val_next(info->bdesc_hash, it)))
		{
			int value;
			
			bdesc->translated++;
			bdesc->bd = info->bd;
			
			value = CALL(info->bd, write_block, bdesc);
			
			bdesc->bd = bd;
			bdesc->translated--;
			
			if(value < 0)
			{
				hash_map_it_destroy(it);
				return value;
			}
			
			/* note that resetting the value for a key already in the hash map does not break iteration */
			hash_map_insert(info->bdesc_hash, (void *) bdesc->number, NULL);
			bdesc_release(&bdesc);
		}
		hash_map_it_destroy(it);
		hash_map_clear(info->bdesc_hash);
		/* FIXME maybe resize the hash map to be small? */
	}
	
	info->state = RELEASE;
	return 0;
}

int journal_queue_hold(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) bd->instance;
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	info->state = HOLD;
	return 0;
}

int journal_queue_passthrough(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) bd->instance;
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	info->state = PASSTHROUGH;
	return 0;
}

const hash_map_t * journal_queue_blocklist(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) bd->instance;
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return NULL;
	
	return info->bdesc_hash;
}
