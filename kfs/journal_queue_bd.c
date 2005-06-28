#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/hash_map.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/blockman.h>
#include <kfs/journal_queue_bd.h>

#define RELEASE_PROGRESS_ENABLED
#define RELEASE_PROGRESS_COLOR 9

/* "JnlQ" */
#define JOURNAL_QUEUE_MAGIC 0x4A6E6C51

struct queue_info {
	BD_t * bd;
	hash_map_t * bdesc_hash;
	uint16_t blocksize, level;
	enum {RELEASE, HOLD, PASSTHROUGH} state;
	blockman_t * blockman;
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
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
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
	return CALL(((struct queue_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t journal_queue_bd_get_blocksize(BD_t * object)
{
	return ((struct queue_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t journal_queue_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct queue_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static bdesc_t * journal_queue_bd_read_block(BD_t * object, uint32_t number)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(object);
	bdesc_t * block;
	bdesc_t * orig;
	
	block = blockman_managed_lookup(info->blockman, number);
	if(block)
		return block;
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	/* is this necessary anymore? probably the blockman
	 * lookup will always work if this would work... */
	block = hash_map_find_val(info->bdesc_hash, (void *) number);
	if(block)
		return block;
	
	/* not in the queue, need to read it */
	block = bdesc_alloc(number, info->blocksize);
	if(!block)
		return NULL;
	bdesc_autorelease(block);
	
	orig = CALL(info->bd, read_block, number);
	if(!orig)
		return NULL;
	
	memcpy(block->ddesc->data, orig->ddesc->data, info->blocksize);
	
	if(blockman_managed_add(info->blockman, block) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

/* we are a barrier, so just synthesize it if it's not already in this zone */
static bdesc_t * journal_queue_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(number >= CALL(info->bd, get_numblocks))
		return NULL;
	
	bdesc = bdesc_alloc(number, info->blocksize);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int journal_queue_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int journal_queue_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(object);
	int value = 0;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	switch(info->state)
	{
		case HOLD:
			if(!hash_map_find_val(info->bdesc_hash, (void *) block->number))
			{
				bdesc_retain(block);
				if((value = hash_map_insert(info->bdesc_hash, (void *) block->number, block)) < 0)
					bdesc_release(&block);
				/* ... and return below */
			}
			break;
		case PASSTHROUGH:
			/* write of existing blocked block? */
			if(hash_map_find_val(info->bdesc_hash, (void *) block->number))
				break;
			/* fall through */
		case RELEASE:
			value = barrier_simple_forward(info->bd, block->number, object, block);
	}
	
	/* note that value is assigned on every path above */
	return value;
}

static int journal_queue_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(object);
	
	/* can't sync in the HOLD state at all */
	if(info->state == HOLD)
		return -E_BUSY;
	
	if(block == SYNC_FULL_DEVICE)
	{
		/* can't sync the whole device except in RELEASE state */
		if(info->state != RELEASE)
			return -E_BUSY;
		return CALL(info->bd, sync, SYNC_FULL_DEVICE, NULL);
	}
	
	/* make sure it's a valid block */
	if(block >= CALL(info->bd, get_numblocks))
		return -E_INVAL;
	
	if(info->state == PASSTHROUGH)
	{
		/* can't sync a held block */
		if(hash_map_find_val(info->bdesc_hash, (void *) block))
			return -E_BUSY;
	}
	
	return CALL(info->bd, sync, block, ch);
}

static uint16_t journal_queue_bd_get_devlevel(BD_t * object)
{
	return ((struct queue_info *) OBJLOCAL(object))->level;
}

static int journal_queue_bd_destroy(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
	int r;
	
	if(info->state == HOLD || info->state == PASSTHROUGH)
		if((r = journal_queue_release(bd)) < 0)
			return r;
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	blockman_destroy(&info->blockman);
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
	
	info->bdesc_hash = hash_map_create();
	if(!info->bdesc_hash)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create();
	if(!info->blockman)
	{
		hash_map_destroy(info->bdesc_hash);
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, journal_queue_bd, info);
	OBJMAGIC(bd) = JOURNAL_QUEUE_MAGIC;
	
	info->bd = disk;
	info->blocksize = CALL(disk, get_blocksize);
	info->state = RELEASE;
	
	/* we might delay blocks, so our level goes up */
	info->level = CALL(disk, get_devlevel) + 1;
	
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
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	if(info->state != RELEASE)
	{
		bdesc_t * bdesc;
		hash_map_it_t it;
#ifdef RELEASE_PROGRESS_ENABLED
		const size_t bdesc_hash_size = hash_map_size(info->bdesc_hash);
		size_t disp_period, disp_prev = 0, nbdescs_released = 0;
		int disp_ncols, r;
#endif

		hash_map_it_init(&it);

#ifdef RELEASE_PROGRESS_ENABLED
		disp_ncols = textbar_init(-1);
		assert(disp_ncols > 0);
		disp_period = (bdesc_hash_size + disp_ncols - 1) / disp_ncols;
#endif

		while((bdesc = (bdesc_t *) hash_map_val_next(info->bdesc_hash, &it)))
		{
			int value;
			
			value = barrier_simple_forward(info->bd, bdesc->number, bd, bdesc);
			
			if(value < 0)
				return value;
			
			/* note that resetting the value for a key already in the hash map does not break iteration */
			hash_map_insert(info->bdesc_hash, (void *) bdesc->number, NULL);
			bdesc_release(&bdesc);

#ifdef RELEASE_PROGRESS_ENABLED
			if(++nbdescs_released >= disp_prev + disp_period)
			{
				r = textbar_set_progress(nbdescs_released * disp_ncols / bdesc_hash_size, RELEASE_PROGRESS_COLOR);
				assert(r >= 0);
				disp_prev = nbdescs_released;
			}
#endif
		}
		hash_map_clear(info->bdesc_hash);
		/* FIXME maybe resize the hash map to be small? */

#ifdef RELEASE_PROGRESS_ENABLED
		r = textbar_close();
		assert(r >= 0);
#endif	
	}
	
	info->state = RELEASE;
	return 0;
}

int journal_queue_hold(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	info->state = HOLD;
	return 0;
}

int journal_queue_passthrough(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return -E_INVAL;
	
	info->state = PASSTHROUGH;
	return 0;
}

const hash_map_t * journal_queue_blocklist(BD_t * bd)
{
	struct queue_info * info = (struct queue_info *) OBJLOCAL(bd);
	
	if(OBJMAGIC(bd) != JOURNAL_QUEUE_MAGIC)
		return NULL;
	
	return info->bdesc_hash;
}
