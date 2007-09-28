/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/hash_map.h>

#include <fscore/bd.h>
#include <fscore/fstitchd.h>
#include <fscore/bdesc.h>
#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/debug.h>
#include <fscore/revision.h>

#include <modules/journal_bd.h>

#ifdef __KERNEL__
#include <fscore/kernel_serve.h>
#elif defined(UNIXUSER)
#include <fscore/fuse_serve.h>
#endif

#define DEBUG_JOURNAL 0
#if DEBUG_JOURNAL
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* transaction period of 5 seconds */
#define TRANSACTION_PERIOD (5 * HZ)
/* transaction slot size of 512 x 4K */
#define TRANSACTION_SIZE (512 * 4096)

/* In principle we can stack journal slots with later transactions, but this
 * really hurts performance because of the effect it has on patch
 * optimizations and rollback. The simple and surprisingly effective fix is to
 * cause the device to flush when this happens, to avoid ever needing to stack
 * transactions. This could be made asynchronous later if necessary. */
#define AVOID_STACKING_JOURNAL 1

/* Theory of operation:
 * 
 * Basically, as patches pass through the journal_bd module, we copy their
 * blocks into a journal and add a before to each of the patches to keep
 * them from being written to disk. Then, when the transaction is over, we write
 * some bookkeeping stuff to the journal, hook it up to the waiting before
 * of all the data, and watch the cache do all our dirty work as it sorts out
 * the patches.
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
 * completely written to disk), we weak retain the last patch in a transaction
 * in an array of patches whose indices correspond to slot numbers. Because we
 * can have "chained" slots, we have a special EMPTY patch that represents the
 * whole transaction (since the commit record cancellation patch will not be
 * created until the end of the transaction, and we need to do the weak retains
 * as we claim slots for use during the transaction).
 * 
 * We keep track of which slot we are currently filling as we are creating a
 * transaction. If and when we fill it, we write a subcommit record, find a new
 * slot, and continue. In this way, when the whole transaction is done, we will
 * be able to do a relatively small amount of work to complete the picture. Note
 * that when subcommit records are written, we must weak retain "done" in their
 * slot so that we can make any reuse of those slots depend on the previous
 * transaction having completed by creating a dependency to it.
 * 
 * Here is the patch structure of a transaction:
 * 
 *   +-------------+------ EMPTYs ---------+--------------------+---------------------+
 *   |             |                      |                    |                     |
 *   |             |                      |                    |                     |
 *   v             |      "keep_h" <---   |                    |                     |
 * "keep_w" <--+   |                   \  |                    |                     |
 *            /    v                    \ v                    v                     v
 * jrdata <--+-- "wait" <-- commit <-- "hold" <-- fsdata <-- "data" <-- cancel <-- "done"
 *           |                 ^         ^ |                   |           ^
 * subcmt <--+                 |         | |*      "keep_d" <--+           |
 *           |                 |         | |                   |           |
 * prev_cr <-+                 |         | +--> prev_cancel <--+           |
 *                             |         |                                 |
 *                             |         +--- Managed EMPTY patch          |
 *                             |                                           |
 *                             +------ Created at end of transaction ------+
 * 
 * Purposes of various EMPTY patches:
 * keep_w:
 *   keep "wait" from becoming satisfied as the jrdata (journal data) patches
 *   are written to disk and satisfied (all the other EMPTYs depend on things
 *   that won't get satisfied until we send the whole transaction off into
 *   the cache)
 * wait:
 *   allow the commit record to easily be hooked up to everything written to
 *   the journal so far, since it will not be created until the end of the
 *   transaction
 * hold:
 *   prevent the actual filesystem changes from being written until we have
 *   hooked up all the necessary dependencies for the transaction
 * keep_h:
 *   keep "hold" from becoming satisfied in the event that prev_cancel does
 *   * the "hold" -> "prev_cancel" dependency is temporary; is is present only
 *     until the end of the transaction to prevent merging with previous ones
 * keep_d:
 *   keep "data" from becoming satisfied in the event that prev_cancel does
 * data:
 *   allow the cancellation to easily be hooked up to all the fsdata (filesystem
 *   data) patches that are part of the transaction, and to the previous one
 * done:
 *   provide a single patch that exists at the beginning of the transaction
 *   which represents the whole transaction, so we can weak retain it to claim
 *   slots in the journal
 * */

struct journal_info {
	BD_t my_bd;
	
	BD_t * bd;
	BD_t * journal;
	patch_t * write_head;
	uint16_t cr_count;
	uint32_t trans_total_blocks;
	uint32_t trans_data_blocks;
	/* state information below */
	patch_t * keep_w;
	patch_t * wait;
	patch_t * keep_h;
	patch_t * hold;
	patch_t * keep_d;
	patch_t * data;
	patch_t * done;
	uint16_t trans_slot, prev_slot;
	uint32_t trans_seq;
	/* If we are reusing a transaction slot, jdata_head stores a weak reference
	 * to the previous "done" patch. Notice that we cannot reuse a transaction
	 * slot during the same transaction as the last time it was used. */
	patchweakref_t jdata_head;
	patchweakref_t prev_cr;
	patchweakref_t prev_cancel;
	struct {
		patchweakref_t cr;
		uint32_t seq;
	} * cr_retain;
	/* map from FS block number -> journal block number (note 0 is invalid) */
	hash_map_t * block_map;
	uint16_t trans_slot_count;
	uint8_t recursion, only_metadata;
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
#define numbers_per_block(blocksize) ((blocksize) / sizeof(uint32_t))

/* number of blocks that must be used for block numbers in a transaction */
static uint32_t trans_number_block_count(uint16_t blocksize)
{
	const uint16_t npb = numbers_per_block(blocksize);
	const uint32_t bpt = (TRANSACTION_SIZE + blocksize - 1) / blocksize;
	return (bpt - 1 + npb) / (npb + 1);
}

static bdesc_t * journal_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct journal_info * info = (struct journal_info *) object;
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, read_block, number, count, page);
}

static bdesc_t * journal_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct journal_info * info = (struct journal_info *) object;
	
	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
	
	return CALL(info->bd, synthetic_read_block, number, count, page);
}

static void journal_bd_unlock_callback(void * data, int count);

static int journal_bd_grab_slot(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) object;
	uint16_t scan = info->trans_slot;
	
	/* we must stay below the total size of the journal */
	assert(info->trans_slot_count != info->cr_count);
	
#if AVOID_STACKING_JOURNAL
	for(;;)
	{
#endif
		do {
			if(!WEAK(info->cr_retain[scan].cr) && info->cr_retain[scan].seq != info->trans_seq)
			{
				if(WEAK(info->jdata_head))
					patch_weak_release(&info->jdata_head, 0);
				patch_weak_retain(info->done, &info->cr_retain[scan].cr, NULL, NULL);
				Dprintf("%s(): using unused transaction slot %d (sequence %u)\n", __FUNCTION__, scan, info->trans_seq);
				info->cr_retain[scan].seq = info->trans_seq;
				info->prev_slot = info->trans_slot;
				info->trans_slot = scan;
				/* if the transaction reaches half the
				 * slots, make sure it finishes soon */
				if(++info->trans_slot_count >= info->cr_count / 2)
					fstitchd_unlock_callback(journal_bd_unlock_callback, object);
				return 0;
			}
			if(++scan == info->cr_count)
				scan = 0;
		} while(scan != info->trans_slot);
#if AVOID_STACKING_JOURNAL
		CALL(info->journal, flush, FLUSH_DEVICE, NULL);
		CALL(info->bd, flush, FLUSH_DEVICE, NULL);
#ifdef __KERNEL__
		if(revision_tail_flights_exist())
			revision_tail_wait_for_landing_requests();
#endif
		CALL(info->journal, flush, FLUSH_DEVICE, NULL);
	}
#else
	/* we could not find an available slot, so start stacking */
	do {
		if(info->cr_retain[scan].seq != info->trans_seq)
		{
			patch_weak_retain(WEAK(info->cr_retain[scan].cr), &info->jdata_head, NULL, NULL);
			patch_weak_retain(info->done, &info->cr_retain[scan].cr, NULL, NULL);
			Dprintf("%s(): reusing currently used transaction slot %d (sequence %u, old %u)\n", __FUNCTION__, scan, info->trans_seq, info->cr_retain[scan].seq);
			info->cr_retain[scan].seq = info->trans_seq;
			info->prev_slot = info->trans_slot;
			info->trans_slot = scan;
			/* if the transaction reaches half the
			 * slots, make sure it finishes soon */
			if(++info->trans_slot_count >= info->cr_count / 2)
				fstitchd_unlock_callback(journal_bd_unlock_callback, object);
			return 0;
		}
		if(++scan == info->cr_count)
			scan = 0;
	} while(scan != info->trans_slot);
	
	/* this should probably never happen */
	kpanic("all transaction slots used by the current transaction (%u)", info->trans_seq);
#endif
}

static uint32_t journal_bd_lookup_block(BD_t * object, bdesc_t * block, uint32_t block_number, bool * fresh)
{
	struct journal_info * info = (struct journal_info *) object;
	uint32_t number = (uint32_t) hash_map_find_val(info->block_map, (void *) block_number);
	
	if(!number)
	{
		patch_t * head = WEAK(info->jdata_head);
		bdesc_t * number_block;
		uint32_t number_block_number;
		size_t blocks = hash_map_size(info->block_map);
		size_t last = blocks % info->trans_data_blocks;
		uint16_t npb = numbers_per_block(object->blocksize);
		int r;
		
		if(fresh)
			*fresh = 1;
		
		if(blocks && !last)
		{
			/* we need to allocate a new transaction slot */
			struct commit_record commit;
			uint32_t record_number = info->trans_slot * info->trans_total_blocks;
			bdesc_t * record = CALL(info->journal, synthetic_read_block, record_number, 1, NULL);
			if(!record)
				return INVALID_BLOCK;
			Dprintf("%s(): writing subcommit record for slot %d (sequence %u) to journal block %u\n", __FUNCTION__, info->trans_slot, info->trans_seq, record_number);
			
			/* first write the subcommit record */
			commit.magic = JOURNAL_MAGIC;
			commit.type = CRSUBCOMMIT;
			commit.next = info->prev_slot;
			commit.nblocks = info->trans_data_blocks;
			commit.seq = info->trans_seq;
			r = patch_create_byte(record, info->journal, 0, sizeof(commit), &commit, &head);
			assert(r >= 0);
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "subcommit");
			r = patch_add_depend(info->wait, head);
			assert(r >= 0);
			head = WEAK(info->jdata_head);
			info->recursion = 1;
			info->write_head = NULL;
			r = CALL(info->journal, write_block, record, record_number);
			info->write_head = info->hold;
			info->recursion = 0;
			assert(r >= 0);
			
			/* then grab a new slot */
			r = journal_bd_grab_slot(object);
			assert(r >= 0);
		}
		
		/* get next journal block, write block number to journal block number map */
		number = info->trans_slot * info->trans_total_blocks + 1;
		number_block_number = number + last / npb;
		if(last % npb)
			number_block = CALL(info->journal, read_block, number_block_number, 1, NULL);
		else
			number_block = CALL(info->journal, synthetic_read_block, number_block_number, 1, NULL);
		assert(number_block);
		
		r = patch_create_byte(number_block, info->journal, (last % npb) * sizeof(uint32_t), sizeof(uint32_t), &block_number, &head);
		assert(r >= 0);
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "journal number");
		r = patch_add_depend(info->wait, head);
		assert(r >= 0);
		info->recursion = 1;
		info->write_head = NULL;
		r = CALL(info->journal, write_block, number_block, number_block_number);
		info->write_head = info->hold;
		info->recursion = 0;
		assert(r >= 0);
		
		/* add the journal block number to the map */
		number += trans_number_block_count(object->blocksize) + last;
		Dprintf("%s(): map FS block %u to journal block %u in number block %u\n", __FUNCTION__, block_number, number, number_block_number);
		r = hash_map_insert(info->block_map, (void *) block_number, (void *) number);
		assert(r >= 0);
	}
	else if(fresh)
		*fresh = 0;
	
	return number;
}

static int journal_bd_start_transaction(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) object;
	int r = -ENOMEM;
	
	/* do we have a journal yet? */
	if(!info->journal)
		return -EINVAL;
	if(info->keep_w)
		return 0;

#define CREATE_EMPTY(name) \
	do { \
		r = patch_create_empty_list(NULL, &info->name, NULL); \
		if(r < 0) \
			goto fail_##name; \
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->name, #name); \
		patch_claim_empty(info->name); \
	} while(0)
	
	/* this order is important due to the error recovery code */
	CREATE_EMPTY(keep_w);
	/* make the new commit record (via wait) depend on the previous via info->prev_cr */
	assert(info->keep_w); /* keep_w must be non-NULL for patch_create_empty_list */
	r = patch_create_empty_list(NULL, &info->wait, info->keep_w, WEAK(info->prev_cr), NULL);
	if(r < 0)
		goto fail_wait;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->wait, "wait");
	CREATE_EMPTY(keep_h);
	assert(info->keep_h);
	/* this one is managed, and temporarily depends on prev_cancel */
	r = patch_create_empty_list(object, &info->hold, info->keep_h, WEAK(info->prev_cancel), NULL);
	if(r < 0)
		goto fail_hold;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->hold, "hold");
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, info->hold, PATCH_NO_PATCHGROUP);
	info->hold->flags |= PATCH_NO_PATCHGROUP;
	CREATE_EMPTY(keep_d);
	/* make the new complete record (via data) depend on the previous via info->prev_cancel */
	assert(info->keep_d); /* keep_d must be non-NULL for patch_create_empty_list */
	r = patch_create_empty_list(NULL, &info->data, info->keep_d, WEAK(info->prev_cancel), NULL);
	if(r < 0)
		goto fail_data;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->data, "data");
	CREATE_EMPTY(done);

	Dprintf("%s(): starting new transaction (sequence %u, wait %p, hold %p, data %p, done %p)\n", __FUNCTION__, info->trans_seq, info->wait, info->hold, info->data, info->done);
	info->trans_slot_count = 0;
	r = journal_bd_grab_slot(object);
	if(r < 0)
		goto fail_postdone;
	
	/* terminate the chain */
	info->prev_slot = info->trans_slot;
	
	/* set the write head */
	info->write_head = info->hold;
	
	return 0;
	
fail_postdone:
	patch_destroy(&info->done);
fail_done:
	patch_destroy(&info->data);
fail_data:
	patch_destroy(&info->keep_d);
fail_keep_d:
	patch_destroy(&info->hold);
fail_hold:
	patch_destroy(&info->keep_h);
fail_keep_h:
	patch_destroy(&info->wait);
fail_wait:
	patch_destroy(&info->keep_w);
fail_keep_w:
	return r;
}

static int journal_bd_stop_transaction(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) object;
	struct commit_record commit;
	uint32_t block_number;
	bdesc_t * block;
	patch_t * head;
	int r;
	
	if(nholds)
		return -EBUSY;

	block_number = info->trans_slot * info->trans_total_blocks;
	block = CALL(info->journal, read_block, block_number, 1, NULL);
	if(!block)
	{
		printf("Can't get the commit record block!\n");
		return -1;
	}
	
	Dprintf("%s(): ending transaction (sequence %u, debug = %d)\n", __FUNCTION__, info->trans_seq, FSTITCH_DEBUG_COUNT());
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
	r = patch_create_byte(block, info->journal, 0, sizeof(commit), &commit, &head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "commit");
	/* ...and make hold depend on it */
	info->hold->flags |= PATCH_SAFE_AFTER;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, info->hold, PATCH_SAFE_AFTER);
	r = patch_add_depend(info->hold, head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	info->hold->flags &= ~PATCH_SAFE_AFTER;
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CLEAR_FLAGS, info->hold, PATCH_SAFE_AFTER);
	/* set the new previous commit record */
	patch_weak_retain(head, &info->prev_cr, NULL, NULL);
	
	/* we no longer need hold -> prev_cancel */
	if(WEAK(info->prev_cancel))
		patch_remove_depend(info->hold, WEAK(info->prev_cancel));
	
	/* create cancellation, make it depend on data */
	commit.type = CREMPTY;
	head = info->data;
	r = patch_create_byte(block, info->journal, 0, sizeof(commit), &commit, &head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "complete");
	/* ...and make done depend on it */
	r = patch_add_depend(info->done, head);
	if(r < 0)
		kpanic("Holy Mackerel!");
	/* set the new previous cancellation record */
	patch_weak_retain(head, &info->prev_cancel, NULL, NULL);
	
	/* unmanage the hold EMPTY */
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_OWNER, info->hold, NULL);
	info->hold->owner = NULL;
	/* satisfy the keep EMPTYs */
	patch_satisfy(&info->keep_w);
	patch_satisfy(&info->keep_h);
	patch_satisfy(&info->keep_d);
	
	/* ...and finally write the commit and cancellation records */
	info->recursion = 1;
	info->write_head = NULL;
	r = CALL(info->journal, write_block, block, block_number);
	info->write_head = info->hold;
	info->recursion = 0;
	if(r < 0)
		kpanic("Holy Mackerel!");
	
	hash_map_clear(info->block_map);
	
	info->write_head = NULL;
	info->keep_w = NULL;
	info->wait = NULL;
	info->keep_h = NULL;
	info->hold = NULL;
	info->keep_d = NULL;
	info->data = NULL;
	info->done = NULL;
	
	Dprintf("%s(): transaction ended (sequence %u, debug = %d)\n", __FUNCTION__, info->trans_seq, FSTITCH_DEBUG_COUNT());
	
	/* increment the transaction slot so we use them all fairly */
	if(++info->trans_slot == info->cr_count)
		info->trans_slot = 0;
	
	return 0;
}

/* We will register this callback to be called as soon as fstitchd_global_lock is
 * unlocked if the cache below us ever reports it is running out of room. We
 * will also register it if the size of the current transaction exceeds half the
 * size of the journal. */
static void journal_bd_unlock_callback(void * data, int count)
{
	BD_t * object = (BD_t *) data;
	struct journal_info * info = (struct journal_info *) object;
	if(info->keep_w && hash_map_size(info->block_map))
	{
		/* FIXME: check return values here */
		journal_bd_stop_transaction(object);
		journal_bd_start_transaction(object);
	}
}

static int journal_bd_write_block(BD_t * object, bdesc_t * block, uint32_t block_number)
{
	struct journal_info * info = (struct journal_info *) object;
	bdesc_t * journal_block;
	patch_t * patch;
	patch_t * patch_index_next;
	uint32_t number;
	int r, metadata = !info->only_metadata;
	const int engaged = patchgroup_engaged();
	
	/* FIXME: make this module support counts other than 1 */
	assert(block->length == object->blocksize);
	
	/* make sure it's a valid block */
	assert(block->length && block_number + block->length / object->blocksize <= object->numblocks);
	
	if(info->recursion)
	{
		/* only used to write the journal itself: many fewer patches there! */
		patch_push_down(block, object, info->bd);
		return CALL(info->bd, write_block, block, block_number);
	}
	
	/* why write a block with no new changes? */
	if(!block->index_patches[object->graph_index].head)
		return 0;
	
	/* there is supposed to always be a transaction going on */
	assert(info->keep_w);
	
	if(info->only_metadata)
	{
		number = (uint32_t) hash_map_find_val(info->block_map, (void *) block_number);
		/* if we already have the block in the journal, it must have metadata */
		if(number)
			metadata = 1;
		/* if there is an patchgroup engaged, everything we do should be
		 * put in the transaction to guarantee proper ordering of data
		 * with respect to both metadata and other data */
		else if(engaged)
			metadata = 1;
		else
			/* otherwise, scan for metadata */
			for(patch = block->index_patches[object->graph_index].head; patch; patch = patch->ddesc_index_next)
				if(!(patch->flags & PATCH_DATA))
				{
					metadata = 1;
					break;
				}
	}
	
	/* inspect and modify all patches passing through */
	for(patch = block->index_patches[object->graph_index].head; patch; patch = patch_index_next)
	{
		int needs_hold = 1;
		patchdep_t ** deps = &patch->befores;
		
		assert(patch->owner == object);
		patch_index_next = patch->ddesc_index_next; /* in case changes */
		
		if(metadata)
		{
			r = patch_add_depend(info->data, patch);
			if(r < 0)
				kpanic("Holy Mackerel!");
		}
		
		while(*deps)
		{
			patch_t * dep = (*deps)->before.desc;
			/* if it's hold, or if it's on the same block, leave it alone */
			if(dep == info->hold || (dep->block && dep->block->ddesc == block->ddesc))
			{
				deps = &(*deps)->before.next;
				if(dep == info->hold)
					needs_hold = 0;
				continue;
			}
			/* otherwise remove this dependency */
			/* WARNING: this makes the journal incompatible
			 * with patchgroups between different file systems */
			patch_dep_remove(*deps);
		}
		
		if(needs_hold)
		{
			patch->flags |= PATCH_SAFE_AFTER;
			FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, patch, PATCH_SAFE_AFTER);
			r = patch_add_depend(patch, info->hold);
			if(r < 0)
				kpanic("Holy Mackerel!");
			patch->flags &= ~PATCH_SAFE_AFTER;
			FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_CLEAR_FLAGS, patch, PATCH_SAFE_AFTER);
		}
		
		if(engaged)
		{
			/* scan the afters as well, and unhook any patchgroup patches */
			/* WARNING: see warning above */
			deps = &patch->afters;
			while(*deps)
				if(((*deps)->after.desc->flags & PATCH_NO_PATCHGROUP) && (*deps)->after.desc->type == EMPTY)
					patch_dep_remove(*deps);
				else
					deps = &(*deps)->before.next;
			/* and set the patchgroup exemption flag */
			patch->flags |= PATCH_NO_PATCHGROUP;
			FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, patch, PATCH_NO_PATCHGROUP);
		}
	}
	
	if(metadata)
	{
		bool fresh = 0;
		patch_t * head;
		number = journal_bd_lookup_block(object, block, block_number, &fresh);
		assert(number != INVALID_BLOCK);
		journal_block = CALL(info->journal, synthetic_read_block, number, 1, NULL);
		assert(journal_block);
		
		/* copy it to the journal */
		head = WEAK(info->jdata_head);
		if(fresh || !journal_block->all_patches || (journal_block->all_patches->flags & PATCH_INFLIGHT))
		{
#if DEBUG_JOURNAL
			if(!fresh)
				Dprintf("%s() new layer on journal block (in flight: %s)\n", __FUNCTION__, journal_block->all_patches ? "yes" : "no");
#endif
			r = patch_create_full(journal_block, info->journal, bdesc_data(block), &head);
			assert(r >= 0);
		}
		else
		{
#ifndef NDEBUG
			if(head)
			{
				patchdep_t * befores;
				for(befores = journal_block->all_patches->befores; befores; befores = befores->before.next)
					if(befores->before.desc == head)
						break;
				assert(befores);
			}
#endif
			assert(!(journal_block->all_patches->flags & PATCH_ROLLBACK));
			FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_REWRITE_BYTE, journal_block->all_patches);
			memcpy(bdesc_data(journal_block), bdesc_data(block), object->blocksize);
#if PATCH_BYTE_SUM
			journal_block->all_patches->byte.new_sum = patch_byte_sum(block->data, object->blocksize);
#endif
		}
		if(head)
		{
			r = patch_add_depend(info->wait, head);
			assert(r >= 0);
		}
		
		info->recursion = 1;
		info->write_head = NULL;
		r = CALL(info->journal, write_block, journal_block, number);
		info->write_head = info->hold;
		info->recursion = 0;
		assert(r >= 0);
	}
	
	patch_push_down(block, object, info->bd);
	
	r = CALL(info->bd, write_block, block, block_number);
	if(CALL(info->bd, get_block_space) <= 0)
		fstitchd_unlock_callback(journal_bd_unlock_callback, object);
	return r;
}

static int journal_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
	struct journal_info * info = (struct journal_info *) object;
	if(info->keep_w && hash_map_size(info->block_map))
	{
		if(journal_bd_stop_transaction(object) < 0)
			return FLUSH_NONE;
		/* FIXME: check return value here */
		journal_bd_start_transaction(object);
		return FLUSH_DONE;
	}
	return FLUSH_EMPTY;
}

static patch_t ** journal_bd_get_write_head(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) object;
	return &info->write_head;
}

static int32_t journal_bd_get_block_space(BD_t * object)
{
	struct journal_info * info = (struct journal_info *) object;
	return CALL(info->bd, get_block_space);
}

static void journal_bd_callback(void * arg)
{
	BD_t * object = (BD_t *) arg;
	struct journal_info * info = (struct journal_info *) object;
	if(info->keep_w && hash_map_size(info->block_map))
	{
		int r = journal_bd_stop_transaction(object);
		if(r < 0 && r != -EBUSY)
			kpanic("Holy Mackerel!");
		if(r >= 0)
			/* FIXME: check return value here */
			journal_bd_start_transaction(object);
	}
}

static int journal_bd_destroy(BD_t * bd)
{
	struct journal_info * info = (struct journal_info *) bd;
	int r;
	
	if(info->keep_w)
	{
		r = journal_bd_stop_transaction(bd);
		if(r < 0)
			return r;
	}
	
	r = modman_rem_bd(bd);
	if(r < 0)
	{
		/* FIXME: check return value here */
		journal_bd_start_transaction(bd);
		return r;
	}
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
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

static int replay_single_transaction(BD_t * bd, uint32_t transaction_start, uint16_t expected_type)
{
	struct journal_info * info = (struct journal_info *) bd;
	patch_t * head = NULL;
	int r = -ENOMEM;
	
	const uint32_t bnpb = numbers_per_block(bd->blocksize);
	const uint32_t transaction_number = transaction_start / info->trans_total_blocks;
	
	uint32_t block, bnb, db;
	struct commit_record * cr;
	bdesc_t * commit_block = CALL(info->journal, read_block, transaction_start, 1, NULL);
	
	if(!commit_block)
		return -1;
	
	cr = (struct commit_record *) bdesc_data(commit_block);
	if(cr->magic != JOURNAL_MAGIC || cr->type != expected_type)
	{
		printf("%s(): journal subtransaction %d signature mismatch! (0x%08x:%d)\n", __FUNCTION__, transaction_number, cr->magic, cr->type);
		return 0;
	}
	
	/* make sure our block doesn't go anywhere for a while */
	bdesc_autorelease(bdesc_retain(commit_block));
	
	if(expected_type == CRCOMMIT)
	{
		/* create the three EMPTYs we will need for this chain */
		r = patch_create_empty_list(NULL, &info->keep_d, NULL);
		if(r < 0)
			goto error_1;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->keep_d, "keep_d");
		patch_claim_empty(info->keep_d);
		/* make the new complete record (via data) depend on the previous via info->prev_cancel */
		r = patch_create_empty_list(NULL, &info->data, info->keep_d, WEAK(info->prev_cancel), NULL);
		if(r < 0)
			goto error_2;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->data, "data");
		r = patch_create_empty_list(NULL, &info->done, NULL);
		if(r < 0)
		{
			patch_destroy(&info->data);
		error_2:
			patch_destroy(&info->keep_d);
		error_1:
			return r;
		}
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->done, "done");
		patch_claim_empty(info->done);
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
	
	Dprintf("%s(): recovering journal subtransaction %d (%d data blocks, sequence %u)\n", __FUNCTION__, transaction_number, cr->nblocks, cr->seq);
	
	/* bnb is "block number block" number */
	bnb = transaction_start + 1;
	/* db is "data block" number */
	db = bnb + trans_number_block_count(bd->blocksize);
	Dprintf("%s(): first number block %u, first journal block %u\n", __FUNCTION__, bnb, db);
	for(block = 0; block < cr->nblocks; block += bnpb)
	{
		uint32_t index, max = MIN(bnpb, cr->nblocks - block);
		bdesc_t * number_block;
		uint32_t * numbers;
		
		Dprintf("%s(): using number block %u (max = %d, bnpb = %d)\n", __FUNCTION__, bnb, max, bnpb);
		number_block = CALL(info->journal, read_block, bnb++, 1, NULL);
		if(!number_block)
			return -1;
		bdesc_retain(number_block);
		
		numbers = (uint32_t *) bdesc_data(number_block);
		for(index = 0; index != max; index++)
		{
			bdesc_t * output;
			bdesc_t * data_block;
			
			Dprintf("%s(): recovering journal block %u -> data block %u\n", __FUNCTION__, db, numbers[index]);
			data_block = CALL(info->journal, read_block, db++, 1, NULL);
			r = -1;
			if(!data_block)
				goto data_error;
			bdesc_retain(data_block);
			
			output = CALL(info->bd, synthetic_read_block, numbers[index], 1, NULL);
			if(!output)
				goto output_error;
			
			head = NULL;
			r = patch_create_full(output, info->bd, bdesc_data(data_block), &head);
			if(r < 0)
				goto output_error;
			r = patch_add_depend(info->data, head);
			if(r < 0)
				goto patch_error;
			r = CALL(info->bd, write_block, output, numbers[index]);
			if(r < 0)
				goto patch_error;
			bdesc_release(&data_block);
			continue;
			
		patch_error:
			/* FIXME clean up patches */
			assert(0);
		output_error:
			bdesc_release(&data_block);
		data_error:
			bdesc_release(&number_block);
			return r;
		}
		
		bdesc_release(&number_block);
	}
	
	patch_weak_retain(info->done, &info->cr_retain[transaction_start / info->trans_total_blocks].cr, NULL, NULL);
	info->cr_retain[transaction_start / info->trans_total_blocks].seq = cr->seq;
	
	/* only CRCOMMIT records need to be cancelled */
	if(cr->type == CRCOMMIT)
	{
		typeof(cr->type) empty = CREMPTY;
		head = info->data;
		r = patch_create_byte_atomic(commit_block, info->journal, (uint16_t) &((struct commit_record *) NULL)->type, sizeof(cr->type), &empty, &head);
		if(r < 0)
			kpanic("Holy Mackerel!");
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "complete");
		r = patch_add_depend(info->done, head);
		if(r < 0)
			kpanic("Holy Mackerel!");
		/* set the new previous cancellation record */
		patch_weak_retain(head, &info->prev_cancel, NULL, NULL);
		/* clean up the transaction state */
		patch_satisfy(&info->keep_d);
		info->data = NULL;
		info->done = NULL;
		/* and write it to disk */
		info->recursion = 1;
		info->write_head = NULL;
		r = CALL(info->journal, write_block, commit_block, transaction_start);
		info->write_head = info->hold;
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
	struct journal_info * info = (struct journal_info *) bd;
	uint32_t transaction;
	uint32_t min_trans = 0;
	uint32_t min_idx = 0;
	uint16_t recover_count = 0;
	
	for(transaction = 0; transaction < info->cr_count; transaction++)
	{
		struct commit_record * cr;
		uint32_t commit_block_number = transaction * info->trans_total_blocks;
		bdesc_t * commit_block = CALL(info->journal, read_block, commit_block_number, 1, NULL);
		
		if(!commit_block)
			return -1;
		
		Dprintf("%s(): slot %d commit record on journal block %u\n", __FUNCTION__, transaction, commit_block_number);
		cr = (struct commit_record *) bdesc_data(commit_block);
		if(cr->magic != JOURNAL_MAGIC || cr->type != CRCOMMIT)
			continue;
		Dprintf("%s(): transaction %d (sequence %u) will be recovered\n", __FUNCTION__, transaction, cr->seq);
		
		recover_count++;
		info->cr_retain[transaction].seq = cr->seq;
		if(!min_trans || LT32(cr->seq, min_trans))
		{
			min_trans = cr->seq;
			min_idx = transaction;
		}
	}
	printf("%s(): %d transactions will be recovered\n", __FUNCTION__, recover_count);
	
	transaction = min_idx;
	while(recover_count)
	{
		printf("%s(): request recovery of transaction %d (%d left)\n", __FUNCTION__, transaction, recover_count - 1);
		int r = replay_single_transaction(bd, transaction * info->trans_total_blocks, CRCOMMIT);
		if(r < 0)
		{
			if(info->keep_w)
			{
				patch_satisfy(&info->keep_w);
				patch_satisfy(&info->keep_d);
				info->data = NULL;
				if(!info->done->befores)
					patch_satisfy(&info->done);
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
				/* FIXME: this case will generally always happen, and is O(n^2) */
				min_trans = 0;
				/* find lowest remaining sequence number */
				while(scan != transaction)
				{
					if(info->cr_retain[scan].seq && GT32(info->cr_retain[scan].seq, info->cr_retain[transaction].seq))
					{
						if(!min_trans || LT32(info->cr_retain[scan].seq, min_trans))
						{
							min_trans = info->cr_retain[scan].seq;
							min_idx = scan;
						}
					}
					if(++scan == info->cr_count)
						scan = 0;
				}
				assert(min_trans);
				transaction = min_idx;
			}
			else
				transaction = scan;
		}
		else
		{
			info->trans_seq = min_trans + 1;
			if(!info->trans_seq)
				info->trans_seq = 1;
		}
	}
	
	return 0;
}

BD_t * journal_bd(BD_t * disk, uint8_t only_metadata)
{
	struct journal_info * info;
	BD_t * bd;
	
	if(!disk->level)
		return NULL;
	
	if(CALL(disk, get_write_head))
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	BD_INIT(bd, journal_bd);
	OBJMAGIC(bd) = JOURNAL_MAGIC;
	
	info->bd = disk;
	info->journal = NULL;
	info->write_head = NULL;
	bd->blocksize = disk->blocksize;
	bd->numblocks = disk->numblocks;
	bd->atomicsize = disk->atomicsize;
	info->trans_total_blocks = (TRANSACTION_SIZE + bd->blocksize - 1) / bd->blocksize;
	info->trans_data_blocks = info->trans_total_blocks - 1 - trans_number_block_count(bd->blocksize);
	info->keep_w = NULL;
	info->wait = NULL;
	info->keep_h = NULL;
	info->hold = NULL;
	info->keep_d = NULL;
	info->data = NULL;
	info->done = NULL;
	info->trans_slot = 0;
	info->prev_slot = 0;
	/* start the transaction sequence numbering 512 from overflow */
	info->trans_seq = -512;
	WEAK_INIT(info->jdata_head);
	WEAK_INIT(info->prev_cr);
	WEAK_INIT(info->prev_cancel);
	info->cr_count = 0;
	info->cr_retain = NULL;
	info->recursion = 0;
	info->only_metadata = only_metadata;
	bd->level = disk->level;
	bd->graph_index = disk->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
	info->block_map = hash_map_create();
	if(!info->block_map)
	{
		DESTROY(bd);
		return NULL;
	}
	
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
	struct journal_info * info = (struct journal_info *) bd;
	patch_t ** write_head;
	uint16_t level;
	
	if(OBJMAGIC(bd) != JOURNAL_MAGIC)
		return -EINVAL;
	
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
			patch_weak_release(&info->jdata_head, 0);
			patch_weak_release(&info->prev_cr, 0);
			patch_weak_release(&info->prev_cancel, 0);
			for(i = 0; i != info->cr_count; i++)
				if(WEAK(info->cr_retain[i].cr))
					patch_weak_release(&info->cr_retain[i].cr, 0);
			sfree(info->cr_retain, info->cr_count * sizeof(*info->cr_retain));
			info->cr_retain = NULL;
			info->cr_count = 0;
		}
		
		return 0;
	}
	
	/* make sure there is no current journal */
	if(info->journal)
		return -EINVAL;
	
	/* if it is an internal journal, we don't have a
	 * current write head so it won't show up here */
	write_head = CALL(journal, get_write_head);
	if(write_head && *write_head)
		return -EINVAL;
	
	/* make sure the journal device has the same blocksize as the disk */
	if(bd->blocksize != journal->blocksize)
		return -EINVAL;
	
	/* make sure the atomic size of the journal device is big enough */
	if(sizeof(struct commit_record) > journal->atomicsize)
		return -EINVAL;
	
	level = journal->level;
	if(!level || level > bd->level)
		return -EINVAL;
	/* The graph index of the journal must be allowed to be larger than the
	 * BD: it will be in the common case of an internal journal, for
	 * instance. But we're more like an LFS module in our use of the
	 * journal; we create the patches, not just forward them. So it's OK. */
	
	if(modman_inc_bd(journal, bd, "journal") < 0)
		return -EINVAL;
	
	info->journal = journal;
	
	info->cr_count = journal->numblocks / info->trans_total_blocks;
	if(info->cr_count < 3)
	{
		printf("%s(): journal is too small (only %d slots)\n", __FUNCTION__, info->cr_count);
		info->cr_count = 0;
		info->journal = NULL;
		modman_dec_bd(journal, bd);
		return -ENOSPC;
	}
	printf("%s(): journal is %uK (%dx%d blocks)\n", __FUNCTION__, info->cr_count * info->trans_total_blocks * bd->blocksize / 1024, info->cr_count, info->trans_total_blocks);
	
	info->cr_retain = scalloc(info->cr_count, sizeof(*info->cr_retain));
	if(!info->cr_retain)
		kpanic("Holy Mackerel!");
	
	replay_journal(bd);
	/* FIXME: check return value here */
	journal_bd_start_transaction(bd);
	
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
