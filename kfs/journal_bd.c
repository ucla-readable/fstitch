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
	uint16_t level, cr_count;
	bool recursion;
	chdesc_t * hold;
	chdesc_t ** cr_commit;
	uint32_t stamp;
};

#define TRANSACTION_PERIOD (5 * HZ)
#define TRANSACTION_SIZE (64 * 4096)

#define CREMPTY     0
#define CRSUBCOMMIT 1
#define CRCOMMIT    2

struct commit_record {
	uint32_t magic;
	uint16_t type, next;
	uint32_t nblocks;
};

/* number of block numbers that can be stored in a block */
static uint16_t numbers_per_block(uint16_t blocksize)
{
	return blocksize / sizeof(uint32_t);
}

/* number of blocks that must be used for block numbers in a transaction */
static uint32_t trans_number_block_count(uint16_t blocksize)
{
	const uint16_t npb = numbers_per_block(blocksize);
	const uint32_t bpt = TRANSACTION_SIZE / blocksize;
	return (bpt - 1 + npb) / (npb + 1);
}

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
				chdesc_stamp(meta->desc, info->stamp);
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
	//printf("Journal test: satisfying control NOOP!\n");
	if(info->hold)
		chdesc_satisfy(&info->hold);
}

static int journal_bd_destroy(BD_t * bd)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	int i, r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	modman_dec_bd(info->journal, bd);
	sched_unregister(journal_bd_callback, bd);
	chdesc_release_stamp(info->stamp);
	for(i = 0; i != info->cr_count; i++)
		if(info->cr_commit[i])
			chdesc_weak_release(&info->cr_commit[i]);
	free(info->cr_commit);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

static int replay_single_transaction(BD_t * bd, uint32_t transaction_start, uint16_t expected_type)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	chdesc_t * head = NULL;
	chdesc_t * tail = NULL;
	chdesc_t * keep = NULL;
	chdesc_t * all = NULL;
	int r;
	
	const uint32_t bnpb = numbers_per_block(info->blocksize);
	const uint32_t transaction_blocks = TRANSACTION_SIZE / info->blocksize;
	const uint32_t transaction_number = transaction_start / transaction_blocks;
	
	uint32_t block, bnb, db;
	struct commit_record * cr;
	bdesc_t * commit_block = CALL(info->journal, read_block, transaction_start);
	
	if(!commit_block)
		return -E_UNSPECIFIED;
	bdesc_retain(commit_block);
	
	cr = (struct commit_record *) commit_block->ddesc->data;
	if(cr->magic != JOURNAL_MAGIC || cr->type != expected_type)
	{
		bdesc_release(&commit_block);
		return 0;
	}
	
	/* check for chained transaction */
	block = cr->next * transaction_blocks;
	if(block != transaction_start)
	{
		/* expect a CRSUBCOMMIT as the next element */
		r = replay_single_transaction(bd, block, CRSUBCOMMIT);
		if(r < 0)
		{
			bdesc_release(&commit_block);
			return r;
		}
	}
	
	printf("%s(): recovering journal transaction %d (%u data blocks)\n", __FUNCTION__, transaction_number, cr->nblocks);
	
	keep = chdesc_create_noop(NULL, bd);
	if(!keep)
		panic("Holy Mackerel!");
	chdesc_claim_noop(keep);
	all = chdesc_create_noop(NULL, NULL);
	if(!all)
		panic("Holy Mackerel!");
	r = chdesc_add_depend(all, keep);
	if(r < 0)
		panic("Holy Mackerel!");
	
	/* bnb is "block number block" number */
	bnb = transaction_start + 1;
	/* db is "data block" number */
	db = bnb + trans_number_block_count(info->blocksize);
	for(block = 0; block < cr->nblocks; block += bnpb)
	{
		uint32_t index, max = MIN(bnpb, cr->nblocks - block);
		uint32_t * numbers;
		bdesc_t * number_block = CALL(info->journal, read_block, bnb);
		if(!number_block)
		{
			bdesc_release(&commit_block);
			return -E_UNSPECIFIED;
		}
		bdesc_retain(number_block);
		
		numbers = (uint32_t *) number_block->ddesc->data;
		for(index = 0; index != max; index++)
		{
			bdesc_t * output;
			bdesc_t * data_block = CALL(info->journal, read_block, db++);
			r = -E_UNSPECIFIED;
			if(!data_block)
				goto data_error;
			bdesc_retain(data_block);
			
			/* FIXME synthetic */
			output = CALL(info->bd, read_block, numbers[index]);
			if(!output)
				goto output_error;
			
			head = NULL;
			r = chdesc_create_full(output, info->bd, data_block->ddesc->data, &head, &tail);
			if(r < 0)
				goto output_error;
			r = chdesc_add_depend(all, head);
			if(r < 0)
				goto chdesc_error;
			r = CALL(info->bd, write_block, output);
			if(r < 0)
				goto chdesc_error;
			bdesc_release(&data_block);
			continue;
			
		chdesc_error:
			/* FIXME clean up chdescs */
			assert(0);
		output_error:
			bdesc_release(&data_block);
		data_error:
			bdesc_release(&number_block);
			bdesc_release(&commit_block);
			return r;
		}
		
		bdesc_release(&number_block);
	}
	
	/* only CRCOMMIT records need to be cancelled */
	if(cr->type == CRCOMMIT)
	{
		typeof(cr->type) empty = CREMPTY;
		head = all;
		chdesc_create_byte(commit_block, info->journal, (uint16_t) &((struct commit_record *) NULL)->type, sizeof(cr->type), &empty, &head, &tail);
		assert(head == tail);
		r = chdesc_weak_retain(head, &info->cr_commit[transaction_start / transaction_blocks]);
		if(r < 0)
			panic("Holy Mackerel!");
		chdesc_satisfy(&keep);
		r = CALL(info->journal, write_block, commit_block);
		if(r < 0)
			panic("Holy Mackerel!");
	}
	else
		chdesc_satisfy(&keep);
	
	bdesc_release(&commit_block);
	
	return 0;
}

static int replay_journal(BD_t * bd)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	uint32_t transaction, transaction_blocks = TRANSACTION_SIZE / info->blocksize;
	
	for(transaction = 0; transaction < info->cr_count; transaction++)
	{
		/* FIXME this may need attention when we finally fix the transaction ordering bug */
		int r = replay_single_transaction(bd, transaction * transaction_blocks, CRCOMMIT);
		if(r < 0)
			return r;
	}
	
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
	
	info->stamp = chdesc_register_stamp(bd);
	if(!info->stamp)
	{
		free(info);
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
	
	info->cr_count = CALL(journal, get_numblocks) / (TRANSACTION_SIZE / blocksize);
	if(!info->cr_count)
		panic("Holy Mackerel!");
	
	info->cr_commit = calloc(info->cr_count, sizeof(*info->cr_commit));
	if(!info->cr_commit)
		panic("Holy Mackerel!");
	
	replay_journal(bd);
	
	/* set up transaction callback */
	if(sched_register(journal_bd_callback, bd, TRANSACTION_PERIOD) < 0)
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
