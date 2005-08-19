#include <inc/env.h>
#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/journal_bd.h>

struct journal_info {
	BD_t * bd;
	BD_t * journal;
	uint16_t blocksize, length;
	uint16_t level;
	bool recursion;
	chdesc_t * hold;
};

#define CREMPTY     0
#define CRSUBCOMMIT 1
#define CRCOMMIT    2

struct commit_record {
	uint32_t magic;
	uint16_t type, next;
	uint32_t nblocks;
};

static int journal_bd_get_config(void * object, int level, char * string, size_t length)
{
	/* no configuration of interest */
	snprintf(string, length, "");
	return 0;
}

static int journal_bd_get_status(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "verbose: BD 0x%08x, JBD 0x%08x, blocksize %d, length %d, level %d", info->bd, info->journal, info->blocksize, info->length, info->level);
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "brief: blocksize %d", info->blocksize);
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "normal: BD 0x%08x, JBD 0x%08x, blocksize %d", info->bd, info->journal, info->blocksize);
	}
	return 0;
}

static uint32_t journal_bd_get_numblocks(BD_t * object)
{
	return CALL(((struct journal_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint16_t journal_bd_get_blocksize(BD_t * object)
{
	return ((struct journal_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t journal_bd_get_atomicsize(BD_t * object)
{
	return CALL(((struct journal_info *) OBJLOCAL(object))->bd, get_atomicsize);
}

static bdesc_t * journal_bd_read_block(BD_t * object, uint32_t number)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	return CALL(info->bd, read_block, number);
}

static bdesc_t * journal_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	return CALL(info->bd, synthetic_read_block, number, synthetic);
}

static int journal_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return -E_INVAL;
	
	return CALL(info->bd, cancel_block, number);
}

static int journal_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;
	
	if(!info->hold)
	{
		int r;
		info->hold = chdesc_create_noop(NULL, object);
		if(!info->hold)
			return -E_NO_MEM;
		r = chdesc_weak_retain(info->hold, &info->hold);
		if(r < 0)
		{
			chdesc_destroy(&info->hold);
			return r;
		}
		/* claim it so it won't get satisfied since it has no dependencies */
		chdesc_claim_noop(info->hold);
	}
	
	if(block->ddesc->changes)
	{
		chmetadesc_t * meta;
		for(meta = block->ddesc->changes->dependencies; meta; meta = meta->next)
			if(meta->desc->owner == object)
			{
				int r = chdesc_add_depend(meta->desc, info->hold);
				if(r < 0)
					panic("Holy Mackerel!");
			}
	}
	
	chdesc_push_down(object, block, info->bd, block);
	
	return CALL(info->bd, write_block, block);
}

static int journal_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	return CALL(info->bd, sync, block, ch);
}

static uint16_t journal_bd_get_devlevel(BD_t * object)
{
	return ((struct journal_info *) OBJLOCAL(object))->level;
}

static void journal_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	printf("Journal test: satisfying control NOOP!\n");
	if(info->hold)
		chdesc_satisfy(&info->hold);
}

static int journal_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(((struct journal_info *) OBJLOCAL(bd))->bd, bd);
	modman_dec_bd(((struct journal_info *) OBJLOCAL(bd))->journal, bd);
	sched_unregister(journal_bd_callback, bd);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * journal_bd(BD_t * disk, BD_t * journal)
{
	uint16_t blocksize;
	struct journal_info * info;
	BD_t * bd;
	
	if(!disk || !journal)
		return NULL;
	
	/* make sure the journal device has the same blocksize as the disk */
	blocksize = CALL(disk, get_blocksize);
	if(blocksize != CALL(journal, get_blocksize))
		return NULL;
	
	/* make sure the atomic size of the journal device is big enough */
	if(sizeof(struct commit_record) > CALL(journal, get_atomicsize))
		return NULL;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, journal_bd, info);
	OBJMAGIC(bd) = JOURNAL_MAGIC;
	
	info->bd = disk;
	info->journal = journal;
	info->blocksize = blocksize;
	info->length = CALL(disk, get_numblocks);
	info->recursion = 0;
	info->hold = NULL;
	
	info->level = CALL(disk, get_devlevel);
	
	/* sixty second callback */
	if(sched_register(journal_bd_callback, bd, 60 * HZ) < 0)
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
	if(modman_inc_bd(journal, bd, NULL) < 0)
	{
		modman_dec_bd(disk, bd);
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
