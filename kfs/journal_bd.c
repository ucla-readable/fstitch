#include <lib/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>

#include <kfs/bd.h>
#include <kfs/kfsd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/debug.h>
#include <kfs/revision.h>
#include <kfs/kernel_serve.h>
#include <kfs/journal_bd.h>

/* transaction period of 5 seconds */
#define TRANSACTION_PERIOD (5 * HZ)
/* transaction slot size of 64 x 4K */
#define TRANSACTION_SIZE (64 * 4096)

/* Theory of operation:
 * 
 * Basically, as chdescs pass through the journal_bd module, we copy their
 * blocks into a journal and add a before to each of the chdescs to keep
 * them from being written to disk. Then, when the transaction is over, we write
 * some bookkeeping stuff to the journal, hook it up to the waiting before
 * of all the data, and watch the cache do all our dirty work as it sorts out
 * the chdescs.
 * 
 * We break the journal area up into slots. Each slot begins with a commit
 * record followed by block number lists, then actual data blocks. The commit
 * record stores the number of blocks stored in this slot (up to the slot's
 * capacity, which depends on how large the journal is), as well as the slot
 * number of the "next" commit record in this "chain" of commit records. If a
 * single slot is not large enough for a transaction, only one of them will be
 * marked as an active commit record (the others will be "subcommit" records),
 * and each record will store the slot number of the next. The chain is
 * terminated by a record that points to itself.
 * 
 * At runtime, to keep track of which slots are busy (i.e. they have not been
 * completely written to disk), we weak retain the last chdesc in a transaction
 * in an array of chdescs whose indices correspond to slot numbers. Because we
 * can have "chained" slots, we have a special NOOP chdesc that represents the
 * whole transaction (since the commit record cancellation chdesc will not be
 * created until the end of the transaction, and we need to do the weak retains
 * as we claim slots for use during the transaction).
 * 
 * We keep track of which slot we are currently filling as we are creating a
 * transaction. If and when we fill it, we write a subcommit record, find a new
 * slot, and continue. In this way, when the whole transaction is done, we will
 * be able to do a relatively small amount of work to complete the picture. Note
 * that when subcommit records are written, we must weak retain "done" in their
 * slot so that we won't reuse those slots until after the entire transaction is
 * finished.
 * 
 * Here is the chdesc structure of a transaction:
 * 
 *   +-------------+------ NOOPs --------+---------------------+---------------------+
 *   |             |                     |                     |                     |
 *   |             |                     |                     |                     |
 *   v             |                     |                     |                     |
 * "keep_w" <--+   |                     |                     |                     |
 *            /    v                     v                     v                     v
 * jrdata <--+-- "wait" <-- commit <-- "hold" <-- fsdata <-- "data" <-- cancel <-- "done"
 *           |                 ^         ^                     |           ^
 * subcmt <--+                 |         |         "keep_d" <--+           |
 *           |                 |         |                     |           |
 * prev_cr <-+                 |         |      prev_cancel <--+           |
 *                             |         |                                 |
 *                             |         +--- Managed NOOP chdesc          |
 *                             |                                           |
 *                             +------ Created at end of transaction ------+
 * 
 * Purposes of various NOOP chdescs:
 * keep_w:
 *   keep "wait" from becoming satisfied as the jrdata (journal data) chdescs
 *   are written to disk and satisfied (all the other NOOPs depend on things
 *   that won't get satisfied until we send the whole transaction off into
 *   the cache)
 * wait:
 *   allow the commit record to easily be hooked up to everything written to
 *   the journal so far, since it will not be created until the end of the
 *   transaction
 * hold:
 *   prevent the actual filesystem changes from being written until we have
 *   hooked up all the necessary dependencies for the transaction
 * keep_d:
 *   keep "data" from becoming satisfied in the event that prev_cancel does
 * data:
 *   allow the cancellation to easily be hooked up to all the fsdata (filesystem
 *   data) chdescs that are part of the transaction, and to the previous one
 * done:
 *   provide a single chdesc that exists at the beginning of the transaction
 *   which represents the whole transaction, so we can weak retain it to claim
 *   slots in the journal
 * */

struct journal_info {
	BD_t * bd;
	BD_t * journal;
	uint16_t blocksize, cr_count;
	uint32_t length;
	uint32_t trans_total_blocks;
	uint32_t trans_data_blocks;
	uint32_t stamp;
	/* state information below */
	chdesc_t * keep_w;
	chdesc_t * wait;
	chdesc_t * hold;
	chdesc_t * keep_d;
	chdesc_t * data;
	chdesc_t * done;
	uint16_t trans_slot, prev_slot;
	uint32_t trans_seq;
	/* If we are reusing a transaction slot, jdata_head stores a weak reference
	 * to the previous "done" chdesc. Notice that we cannot reuse a transaction
	 * slot during the same transaction as the last time it was used. */
	chdesc_t * jdata_head;
	chdesc_t * prev_cr;
	chdesc_t * prev_cancel;
	struct {
		chdesc_t * cr;
		uint32_t seq;
	} * cr_retain;
	/* map from FS block number -> journal block number (note 0 is invalid) */
	hash_map_t * block_map;
	uint16_t trans_slot_count;
	uint8_t recursion;
};

#define CREMPTY     0
#define CRSUBCOMMIT 1
#define CRCOMMIT    2

struct commit_record {
	uint32_t magic;
	uint16_t type, next;
	uint32_t nblocks;
	uint32_t seq;
};

static unsigned int nholds = 0;

/* number of block numbers that can be stored in a block */
static uint16_t numbers_per_block(uint16_t blocksize)
{
	return blocksize / sizeof(uint32_t);
}

/* number of blocks that must be used for block numbers in a transaction */
static uint32_t trans_number_block_count(uint16_t blocksize)
{
	const uint16_t npb = numbers_per_block(blocksize);
	const uint32_t bpt = (TRANSACTION_SIZE + blocksize - 1) / blocksize;
	return (bpt - 1 + npb) / (npb + 1);
}

static int journal_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "blocksize: %d, length: %d, level: %d", info->blocksize, info->length, bd->level);
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "blocksize: %d", info->blocksize);
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "blocksize: %d, length: %d", info->blocksize, info->length);
	}
	return 0;
}

static int journal_bd_get_status(void * object, int level, char * string, size_t length)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL((BD_t *) object);
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "held: %d, slot: %d", hash_map_size(info->block_map), info->trans_slot);
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "held: %d", hash_map_size(info->block_map));
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "held: %d", hash_map_size(info->block_map));
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

static bdesc_t * journal_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	/* make sure it's a valid block */
	if(!count || number + count > info->length)
		return NULL;
	
	return CALL(info->bd, read_block, number, count);
}

static bdesc_t * journal_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	/* make sure it's a valid block */
	if(!count || number + count > info->length)
		return NULL;
	
	return CALL(info->bd, synthetic_read_block, number, count);
}

static void journal_bd_unlock_callback(void * data, int count);

static int journal_bd_grab_slot(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	uint16_t scan = info->trans_slot;
	int r;
	
	/* we must stay below the total size of the journal */
	assert(info->trans_slot_count != info->cr_count);
	
	do {
		if(!info->cr_retain[scan].cr && info->cr_retain[scan].seq != info->trans_seq)
		{
			if(info->jdata_head)
				chdesc_weak_release(&info->jdata_head);
			r = chdesc_weak_retain(info->done, &info->cr_retain[scan].cr);
			if(r < 0)
				return r;
			info->cr_retain[scan].seq = info->trans_seq;
			info->prev_slot = info->trans_slot;
			info->trans_slot = scan;
			/* if the transaction reaches half the
			 * slots, make sure it finishes soon */
			if(++info->trans_slot_count >= info->cr_count / 2)
				kfsd_unlock_callback(journal_bd_unlock_callback, object);
			return 0;
		}
		if(++scan == info->cr_count)
			scan = 0;
	} while(scan != info->trans_slot);
	
	/* we could not find an available slot, so start stacking */
	do {
		if(info->cr_retain[scan].seq != info->trans_seq)
		{
			r = chdesc_weak_retain(info->cr_retain[scan].cr, &info->jdata_head);
			if(r < 0)
				return r;
			r = chdesc_weak_retain(info->done, &info->cr_retain[scan].cr);
			if(r < 0)
				return r;
			info->cr_retain[scan].seq = info->trans_seq;
			info->prev_slot = info->trans_slot;
			info->trans_slot = scan;
			/* if the transaction reaches half the
			 * slots, make sure it finishes soon */
			if(++info->trans_slot_count >= info->cr_count / 2)
				kfsd_unlock_callback(journal_bd_unlock_callback, object);
			return 0;
		}
		if(++scan == info->cr_count)
			scan = 0;
	} while(scan != info->trans_slot);
	
	/* this should probably never happen */
	kpanic("all transaction slots used by the current transaction (%d)", info->trans_seq);
}

static uint32_t journal_bd_lookup_block(BD_t * object, bdesc_t * block)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	uint32_t number = (uint32_t) hash_map_find_val(info->block_map, (void *) block->number);
	
	if(!number)
	{
		chdesc_t * head = info->jdata_head;
		bdesc_t * number_block;
		size_t blocks = hash_map_size(info->block_map);
		size_t last = blocks % info->trans_data_blocks;
		uint32_t data;
		int r;
		
		if(blocks && !last)
		{
			/* we need to allocate a new transaction slot */
			struct commit_record commit;
			bdesc_t * record = CALL(info->journal, read_block, info->trans_slot * info->trans_total_blocks, 1);
			if(!record)
				return INVALID_BLOCK;
			
			/* first write the subcommit record */
			commit.magic = JOURNAL_MAGIC;
			commit.type = CRSUBCOMMIT;
			commit.next = info->prev_slot;
			commit.nblocks = info->trans_data_blocks;
			commit.seq = info->trans_seq;
			r = chdesc_create_byte_atomic(record, info->journal, 0, sizeof(commit), &commit, &head);
			assert(r >= 0);
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "subcommit");
			r = chdesc_add_depend(info->wait, head);
			assert(r >= 0);
			head = NULL;
			info->recursion = 1;
			r = CALL(info->journal, write_block, record);
			info->recursion = 0;
			assert(r >= 0);
			
			/* then grab a new slot */
			r = journal_bd_grab_slot(object);
			assert(r >= 0);
		}
		
		/* get next journal block, write block number to journal block number map */
		number = info->trans_slot * info->trans_total_blocks + 1;
		number_block = CALL(info->journal, read_block, number + last / numbers_per_block(info->blocksize), 1);
		assert(number_block);
		
		data = block->number;
		r = chdesc_create_byte(number_block, info->journal, last * sizeof(uint32_t), sizeof(uint32_t), &data, &head);
		assert(r >= 0);
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "journal number");
		r = chdesc_add_depend(info->wait, head);
		assert(r >= 0);
		info->recursion = 1;
		r = CALL(info->journal, write_block, number_block);
		info->recursion = 0;
		assert(r >= 0);
		
		/* add the journal block number to the map */
		number += trans_number_block_count(info->blocksize) + last;
		r = hash_map_insert(info->block_map, (void *) block->number, (void *) number);
		assert(r >= 0);
	}
	
	return number;
}

static int journal_bd_start_transaction(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	int r = -E_NO_MEM;
	
	/* do we have a journal yet? */
	if(!info->journal)
		return -E_INVAL;
	if(info->keep_w)
		return 0;

#define CREATE_NOOP(name, owner) \
	do { \
		r = chdesc_create_noop_list(NULL, owner, &info->name, NULL); \
		if(r < 0) \
			goto fail_##name; \
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->name, #name); \
		chdesc_claim_noop(info->name); \
	} while(0)
	
	/* this order is important due to the error recovery code */
	CREATE_NOOP(keep_w, NULL);
	/* make the new commit record (via wait) depend on the previous via info->prev_cr */
	assert(info->keep_w); /* keep_w must be non-NULL for chdesc_create_noop_list */
	r = chdesc_create_noop_list(NULL, NULL, &info->wait, info->keep_w, info->prev_cr, NULL);
	if(r < 0)
		goto fail_wait;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->wait, "wait");
	CREATE_NOOP(hold, object); /* this one is managed */
	CREATE_NOOP(keep_d, NULL);
	/* make the new complete record (via data) depend on the previous via info->prev_cancel */
	assert(info->keep_d); /* keep_d must be non-NULL for chdesc_create_noop_list */
	r = chdesc_create_noop_list(NULL, NULL, &info->data, info->keep_d, info->prev_cancel, NULL);
	if(r < 0)
		goto fail_data;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->data, "data");
	CREATE_NOOP(done, NULL);

	info->trans_slot_count = 0;
	r = journal_bd_grab_slot(object);
	if(r < 0)
		goto fail_postdone;
	
	/* terminate the chain */
	info->prev_slot = info->trans_slot;
	
	return 0;
	
fail_postdone:
	chdesc_destroy(&info->done);
fail_done:
	chdesc_destroy(&info->data);
fail_data:
	chdesc_destroy(&info->keep_d);
fail_keep_d:
	chdesc_destroy(&info->hold);
fail_hold:
	chdesc_destroy(&info->wait);
fail_wait:
	chdesc_destroy(&info->keep_w);
fail_keep_w:
	return r;
}

static int journal_bd_stop_transaction(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	struct commit_record commit;
	bdesc_t * block;
	chdesc_t * head;
	int r;
	
	if(nholds)
		return -E_BUSY;
	
	block = CALL(info->journal, read_block, info->trans_slot * info->trans_total_blocks, 1);
	if(!block)
	{
		printf("Can't get the commit record block!\n");
		return -E_UNSPECIFIED;
	}
	
	commit.magic = JOURNAL_MAGIC;
	commit.type = CRCOMMIT;
	commit.next = info->prev_slot;
	commit.nblocks = hash_map_size(info->block_map) % info->trans_data_blocks;
	commit.seq = info->trans_seq++;
	/* skip 0 */
	if(!info->trans_seq)
		info->trans_seq = 1;
	
	/* create commit record, make it depend on wait */
	head = info->wait;
	r = chdesc_create_byte(block, info->journal, 0, sizeof(commit), &commit, &head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "commit");
	/* ...and make hold depend on it */
	info->hold->flags |= CHDESC_SAFE_AFTER;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, info->hold, CHDESC_SAFE_AFTER);
	r = chdesc_add_depend(info->hold, head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	info->hold->flags &= ~CHDESC_SAFE_AFTER;
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_CLEAR_FLAGS, info->hold, CHDESC_SAFE_AFTER);
	/* set the new previous commit record */
	r = chdesc_weak_retain(head, &info->prev_cr);
	if(r < 0)
		kpanic("Holy Mackerel!");
	
	/* create cancellation, make it depend on data */
	commit.type = CREMPTY;
	head = info->data;
	r = chdesc_create_byte(block, info->journal, 0, sizeof(commit), &commit, &head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "complete");
	/* ...and make done depend on it */
	r = chdesc_add_depend(info->done, head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	/* set the new previous cancellation record */
	r = chdesc_weak_retain(head, &info->prev_cancel);
	if(r < 0)
		kpanic("Holy Mackerel!");
	
	/* unmanage the hold NOOP */
	KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_OWNER, info->hold, NULL);
	info->hold->owner = NULL;
	/* satisfy the keep NOOPs */
	chdesc_satisfy(&info->keep_w);
	chdesc_satisfy(&info->keep_d);
	
	/* ...and finally write the commit and cancellation records */
	info->recursion = 1;
	r = CALL(info->journal, write_block, block);
	info->recursion = 0;
	if(r < 0)
		kpanic("Holy Mackerel!");
	
	hash_map_clear(info->block_map);
	
	info->keep_w = NULL;
	info->wait = NULL;
	info->hold = NULL;
	info->keep_d = NULL;
	info->data = NULL;
	info->done = NULL;
	
	/* increment the transaction slot so we use them all fairly */
	if(++info->trans_slot == info->cr_count)
		info->trans_slot = 0;
	
	return 0;
}

/* We will register this callback to be called as soon as kfsd_global_lock is
 * unlocked if the cache below us ever reports it is running out of room. We
 * will also register it if the size of the current transaction exceeds half the
 * size of the journal. */
static void journal_bd_unlock_callback(void * data, int count)
{
	BD_t * object = (BD_t *) data;
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	if(info->keep_w)
		journal_bd_stop_transaction(object);
}

static int journal_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	bdesc_t * journal_block;
	chdesc_t * chdesc, * chdesc_next;
	chdesc_t * head;
	uint32_t number;
	int r;
	
	/* FIXME: make this module support counts other than 1 */
	assert(block->count == 1);
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->length)
		return -E_INVAL;
	
	if(info->recursion)
	{
		chdesc_push_down(object, block, info->bd, block);
		return CALL(info->bd, write_block, block);
	}
	
	/* why write a block with no changes? */
	if(!block->ddesc->all_changes)
		return 0;
	
	/* we should have gotten a get_write_head call,
	 * which would start a transaction for us */
	assert(info->keep_w);
	
	/* add our stamp to all chdescs passing through */
	for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc_next)
	{
		chdesc_next = chdesc->ddesc_next; /* in case changes */
		if(chdesc->owner == object)
		{
			int r = chdesc_add_depend(info->data, chdesc);
			if(r < 0)
				kpanic("Holy Mackerel!");
			chdesc_stamp(chdesc, info->stamp);
		}
	}
	
	number = journal_bd_lookup_block(object, block);
	assert(number != INVALID_BLOCK);
	journal_block = CALL(info->journal, read_block, number, 1);
	assert(journal_block);
	
	/* rewind the data to the state that is (now) below us... */
	r = revision_tail_prepare_stamp(block, info->stamp);
	assert(r >= 0);
	/* ...and copy it to the journal */
	head = info->jdata_head;
	r = chdesc_rewrite_block(journal_block, info->journal, block->ddesc->data, &head);
	assert(r >= 0);
	r = revision_tail_revert_stamp(block, info->stamp);
	assert(r >= 0);
	if(head)
	{
		r = chdesc_add_depend(info->wait, head);
		assert(r >= 0);
	}
	
	info->recursion = 1;
	r = CALL(info->journal, write_block, journal_block);
	info->recursion = 0;
	assert(r >= 0);
	
	chdesc_push_down(object, block, info->bd, block);
	
	r = CALL(info->bd, write_block, block);
	if(CALL(info->bd, get_block_space) <= 0)
		kfsd_unlock_callback(journal_bd_unlock_callback, object);
	return r;
}

static int journal_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	if(info->keep_w)
	{
		if(journal_bd_stop_transaction(object) < 0)
			return FLUSH_NONE;
		return FLUSH_DONE;
	}
	return FLUSH_EMPTY;
}

static chdesc_t * journal_bd_get_write_head(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	assert(!CALL(info->bd, get_write_head));
	if(info->recursion)
		return NULL;
	if(!info->keep_w)
	{
		int r = journal_bd_start_transaction(object);
		assert(r >= 0);
	}
	return info->hold;
}

static int32_t journal_bd_get_block_space(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	return CALL(info->bd, get_block_space);
}

static void journal_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	struct journal_info * info = (struct journal_info *) OBJLOCAL(object);
	if(info->keep_w)
	{
		int r;
		r = journal_bd_stop_transaction(object);
		if(r < 0 && r != -E_BUSY)
			kpanic("Holy Mackerel!");
	}
}

static int journal_bd_destroy(BD_t * bd)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	int r;
	
	if(info->keep_w)
	{
		r = journal_bd_stop_transaction(bd);
		if(r < 0)
			return r;
	}
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_bd(info->bd, bd);
	
	if(info->journal)
	{
		r = journal_bd_set_journal(bd, NULL);
		/* should not fail; we just stopped the transaction */
		assert(r >= 0);
	}
	
	r = sched_unregister(journal_bd_callback, bd);
	/* should not fail */
	assert(r >= 0);
	
	/* might not exist if we are destroying because of failed creation */
	if(info->block_map)
		hash_map_destroy(info->block_map);
	
	chdesc_release_stamp(info->stamp);
	
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

static int replay_single_transaction(BD_t * bd, uint32_t transaction_start, uint16_t expected_type)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	chdesc_t * head = NULL;
	int r = -E_NO_MEM;
	
	const uint32_t bnpb = numbers_per_block(info->blocksize);
	const uint32_t transaction_number = transaction_start / info->trans_total_blocks;
	
	uint32_t block, bnb, db;
	struct commit_record * cr;
	bdesc_t * commit_block = CALL(info->journal, read_block, transaction_start, 1);
	
	if(!commit_block)
		return -E_UNSPECIFIED;
	
	cr = (struct commit_record *) commit_block->ddesc->data;
	if(cr->magic != JOURNAL_MAGIC || cr->type != expected_type)
		return 0;
	
	/* make sure our block doesn't go anywhere for a while */
	bdesc_autorelease(bdesc_retain(commit_block));
	
	if(expected_type == CRCOMMIT)
	{
		/* create the three NOOPs we will need for this chain */
		r = chdesc_create_noop_list(NULL, NULL, &info->keep_d, NULL);
		if(r < 0)
			goto error_1;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->keep_d, "keep_d");
		chdesc_claim_noop(info->keep_d);
		/* make the new complete record (via data) depend on the previous via info->prev_cancel */
		r = chdesc_create_noop_list(NULL, NULL, &info->data, info->keep_d, info->prev_cancel, NULL);
		if(r < 0)
			goto error_2;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->data, "data");
		r = chdesc_create_noop_list(NULL, NULL, &info->done, NULL);
		if(r < 0)
		{
			chdesc_destroy(&info->data);
		error_2:
			chdesc_destroy(&info->keep_d);
		error_1:
			return r;
		}
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, info->done, "done");
		chdesc_claim_noop(info->done);
	}
	
	/* check for chained transaction */
	block = cr->next * info->trans_total_blocks;
	if(block != transaction_start)
	{
		/* expect a CRSUBCOMMIT as the next element */
		r = replay_single_transaction(bd, block, CRSUBCOMMIT);
		if(r < 0)
			return r;
	}
	
	printf("%s(): recovering journal transaction %d (%u data blocks)\n", __FUNCTION__, transaction_number, cr->nblocks);
	
	/* bnb is "block number block" number */
	bnb = transaction_start + 1;
	/* db is "data block" number */
	db = bnb + trans_number_block_count(info->blocksize);
	for(block = 0; block < cr->nblocks; block += bnpb)
	{
		uint32_t index, max = MIN(bnpb, cr->nblocks - block);
		uint32_t * numbers;
		bdesc_t * number_block = CALL(info->journal, read_block, bnb, 1);
		if(!number_block)
			return -E_UNSPECIFIED;
		bdesc_retain(number_block);
		
		numbers = (uint32_t *) number_block->ddesc->data;
		for(index = 0; index != max; index++)
		{
			bdesc_t * output;
			bdesc_t * data_block = CALL(info->journal, read_block, db++, 1);
			r = -E_UNSPECIFIED;
			if(!data_block)
				goto data_error;
			bdesc_retain(data_block);
			
			output = CALL(info->bd, synthetic_read_block, numbers[index], 1);
			if(!output)
				goto output_error;
			
			head = NULL;
			r = chdesc_create_full(output, info->bd, data_block->ddesc->data, &head);
			if(r < 0)
				goto output_error;
			r = chdesc_add_depend(info->data, head);
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
			return r;
		}
		
		bdesc_release(&number_block);
	}
	
	r = chdesc_weak_retain(info->done, &info->cr_retain[transaction_start / info->trans_total_blocks].cr);
	if(r < 0)
		kpanic("Holy Mackerel!");
	info->cr_retain[transaction_start / info->trans_total_blocks].seq = cr->seq;
	
	/* only CRCOMMIT records need to be cancelled */
	if(cr->type == CRCOMMIT)
	{
		typeof(cr->type) empty = CREMPTY;
		head = info->data;
		r = chdesc_create_byte_atomic(commit_block, info->journal, (uint16_t) &((struct commit_record *) NULL)->type, sizeof(cr->type), &empty, &head);
		if(r < 0)
			kpanic("Holy Mackerel!");
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "complete");
		r = chdesc_add_depend(info->done, head);
		if(r < 0)
			kpanic("Holy Mackerel!");
		/* set the new previous cancellation record */
		r = chdesc_weak_retain(head, &info->prev_cancel);
		if(r < 0)
			kpanic("Holy Mackerel!");
		/* clean up the transaction state */
		chdesc_satisfy(&info->keep_d);
		info->data = NULL;
		info->done = NULL;
		/* and write it to disk */
		info->recursion = 1;
		r = CALL(info->journal, write_block, commit_block);
		info->recursion = 0;
		if(r < 0)
			kpanic("Holy Mackerel!");
	}
	
	return 0;
}

/* these macros are for the circular sequence number space */
#define GT32(a, b) (((int32_t) ((a) - (b))) > 0)
#define GE32(a, b) (((int32_t) ((a) - (b))) >= 0)
#define LT32(a, b) (((int32_t) ((a) - (b))) < 0)
#define LE32(a, b) (((int32_t) ((a) - (b))) <= 0)

static int replay_journal(BD_t * bd)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	uint32_t transaction;
	uint32_t min_trans = 0;
	uint32_t min_idx = 0;
	uint16_t recover_count = 0;
	
	for(transaction = 0; transaction < info->cr_count; transaction++)
	{
		struct commit_record * cr;
		bdesc_t * commit_block = CALL(info->journal, read_block, transaction * info->trans_total_blocks, 1);
		
		if(!commit_block)
			return -E_UNSPECIFIED;
		
		cr = (struct commit_record *) commit_block->ddesc->data;
		if(cr->magic != JOURNAL_MAGIC || cr->type != CRCOMMIT)
			continue;
		
		recover_count++;
		info->cr_retain[transaction].seq = cr->seq;
		if(!min_trans || LT32(cr->seq, min_trans))
		{
			min_trans = cr->seq;
			min_idx = transaction;
		}
	}
	
	transaction = min_idx;
	while(recover_count)
	{
		int r = replay_single_transaction(bd, transaction * info->trans_total_blocks, CRCOMMIT);
		if(r < 0)
		{
			if(info->keep_w)
			{
				chdesc_satisfy(&info->keep_w);
				info->data = NULL;
				if(!info->done->befores)
					chdesc_satisfy(&info->done);
				else
					info->done = NULL;
			}
			return r;
		}
		if(--recover_count)
		{
			uint32_t scan = transaction + 1;
			uint32_t next_seq = info->cr_retain[transaction].seq + 1;
			if(scan == info->cr_count)
				scan = 0;
			/* skip 0 */
			if(!next_seq)
				next_seq = 1;
			if(info->cr_retain[scan].seq != next_seq)
			{
				printf("%s(): found non-linear transaction!\n", __FUNCTION__);
				min_trans = 0;
				/* find lowest remaining sequence number */
				while(scan != transaction)
				{
					if(info->cr_retain[scan].seq)
						if(!min_trans || LT32(info->cr_retain[scan].seq, min_trans))
						{
							min_trans = info->cr_retain[scan].seq;
							min_idx = scan;
						}
					if(++scan == info->cr_count)
						scan = 0;
				}
				assert(min_trans);
			}
			transaction = min_idx;
		}
		else
		{
			info->trans_seq = info->cr_retain[transaction].seq + 1;
			if(!info->trans_seq)
				info->trans_seq = 1;
		}
	}
	
	return 0;
}

BD_t * journal_bd(BD_t * disk)
{
	struct journal_info * info;
	BD_t * bd;
	
	if(!disk->level)
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
	info->journal = NULL;
	info->blocksize = CALL(disk, get_blocksize);
	info->length = CALL(disk, get_numblocks);
	bd->level = disk->level;
	info->trans_total_blocks = (TRANSACTION_SIZE + info->blocksize - 1) / info->blocksize;
	info->trans_data_blocks = info->trans_total_blocks - 1 - trans_number_block_count(info->blocksize);
	info->keep_w = NULL;
	info->wait = NULL;
	info->hold = NULL;
	info->keep_d = NULL;
	info->data = NULL;
	info->done = NULL;
	info->trans_slot = 0;
	info->prev_slot = 0;
	/* start the transaction sequence numbering 65536 from overflow */
	info->trans_seq = -65536;
	info->jdata_head = NULL;
	info->prev_cr = NULL;
	info->prev_cancel = NULL;
	
	info->block_map = hash_map_create();
	if(!info->block_map)
	{
		DESTROY(bd);
		return NULL;
	}
	
	info->cr_count = 0;
	info->cr_retain = NULL;
	info->recursion = 0;
	
	/* set up transaction callback */
	if(sched_register(journal_bd_callback, bd, TRANSACTION_PERIOD) < 0)
	{
		DESTROY(bd);
		return NULL;
	}
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		sched_unregister(journal_bd_callback, bd);
		DESTROY(bd);
		return NULL;
	}
	if(modman_inc_bd(disk, bd, "data") < 0)
	{
		modman_rem_bd(bd);
		sched_unregister(journal_bd_callback, bd);
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}

int journal_bd_set_journal(BD_t * bd, BD_t * journal)
{
	struct journal_info * info = (struct journal_info *) OBJLOCAL(bd);
	uint16_t level;
	
	if(OBJMAGIC(bd) != JOURNAL_MAGIC)
		return -E_INVAL;
	
	/* allow disabling the journal */
	if(!journal)
	{
		if(info->journal)
		{
			int i;
			if(info->keep_w)
			{
				int r;
				r = journal_bd_stop_transaction(bd);
				if(r < 0)
					return r;
			}
			modman_dec_bd(info->journal, bd);
			info->journal = NULL;
			chdesc_weak_release(&info->jdata_head);
			chdesc_weak_release(&info->prev_cr);
			chdesc_weak_release(&info->prev_cancel);
			for(i = 0; i != info->cr_count; i++)
				if(info->cr_retain[i].cr)
					chdesc_weak_release(&info->cr_retain[i].cr);
			sfree(info->cr_retain, info->cr_count * sizeof(*info->cr_retain));
			info->cr_retain = NULL;
			info->cr_count = 0;
		}
		
		return 0;
	}
	
	/* make sure there is no current journal */
	if(info->journal)
		return -E_INVAL;
	
	/* make sure the journal device has the same blocksize as the disk */
	if(info->blocksize != CALL(journal, get_blocksize))
		return -E_INVAL;
	
	/* make sure the atomic size of the journal device is big enough */
	if(sizeof(struct commit_record) > CALL(journal, get_atomicsize))
		return -E_INVAL;
	
	level = journal->level;
	if(!level || level > bd->level)
		return -E_INVAL;
	
	if(modman_inc_bd(journal, bd, "journal") < 0)
		return -E_INVAL;
	
	info->journal = journal;
	
	info->cr_count = CALL(journal, get_numblocks) / info->trans_total_blocks;
	if(!info->cr_count)
		kpanic("Holy Mackerel!");
	
	info->cr_retain = scalloc(info->cr_count, sizeof(*info->cr_retain));
	if(!info->cr_retain)
		kpanic("Holy Mackerel!");
	
	replay_journal(bd);
	
	return 0;
}

void journal_bd_add_hold(void)
{
	nholds++;
}

void journal_bd_remove_hold(void)
{
	assert(nholds > 0);
	if(!nholds)
		printf("%s: nholds already 0\n", __FUNCTION__);
	else
		nholds--;
}
