#include <inc/malloc.h>
#include <inc/hash_map.h>

#include <kfs/bdesc.h>
#include <kfs/fdesc.h>
#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/sched.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/journal_lfs.h>


#define JOURNAL_DEBUG 0

#if JOURNAL_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct journal_state {
	BD_t * queue;
	LFS_t * journal;
	fdesc_t * jfdesc;
	LFS_t * fs;
	chdesc_t ** commit_chdesc;
	uint16_t ncommit_records, next_trans_slot;
	uint16_t blocksize;
};
typedef struct journal_state journal_state_t;


//
// Journaling

// A transaction's layout on disk:
// blkno    | description
// ---------+-------------
// 0        | commit_record_t
// 1..k     | disk blknos for where each journal data block goes,
//          | k = trans_number_block_count()
// k+1..end | the journal data blocks

#define TRANSACTION_SIZE (64*4096)
//(100*1024)
static const char journal_filename[] = "/.journal";

/* "SAFEDATA" */
#define JOURNAL_MAGIC 0x5AFEDA7A

#define CREMPTY     0
#define CRSUBCOMMIT 1
#define CRCOMMIT    2

// This structure will be used on disk, so we use all exact size types
struct commit_record {
	uint32_t magic;
	uint16_t type, next;
	uint32_t nblocks;
};
typedef struct commit_record commit_record_t;


// Return the number of journal data block numbers that fit in a disk block
static size_t numbers_per_block(uint16_t blksize)
{
	return blksize / sizeof(uint32_t);
}

// Return the number of blocks reserved in a transaction for the journal data
// block numbers
static size_t trans_number_block_count(uint16_t blksize)
{
	const size_t nos_per_blk = numbers_per_block(blksize);
	const size_t nblks_transaction = ROUNDDOWN32(TRANSACTION_SIZE, blksize) / blksize;

	return (nblks_transaction - 1 + nos_per_blk) / (nos_per_blk + 1);
}


// TODO
static int ensure_journal_exists(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	if (state->jfdesc)
		return -E_FILE_EXISTS;
	state->jfdesc = CALL(state->journal, lookup_name, journal_filename);
	if (!state->jfdesc)
	{
		// TODO: Attempt to create journal_filename?
		return -E_NOT_FOUND;
	}

	return 0;
}

/* transaction_start is the block number containing the commit record */
static int replay_single_transaction(journal_state_t * state, uint32_t transaction_start, uint16_t expected_type)
{
	const size_t bnpb = numbers_per_block(state->blocksize);
	const uint32_t transaction_blocks = TRANSACTION_SIZE / state->blocksize;
	
	uint32_t block, bnb, db;
	struct commit_record * cr;
	bdesc_t * commit_block = CALL(state->journal, get_file_block, state->jfdesc, transaction_start * state->blocksize);
	if(!commit_block)
		return -E_UNSPECIFIED;
	bdesc_retain(&commit_block);
	
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
		int r = replay_single_transaction(state, block, CRSUBCOMMIT);
		if(r < 0)
		{
			bdesc_release(&commit_block);
			return r;
		}
	}
	
	printf("%s(): recovering journal transaction %d (%u data blocks)\n", __FUNCTION__, transaction_start / transaction_blocks, cr->nblocks);
	
	/* bnb is "block number block" number */
	bnb = transaction_start + 1;
	/* db is "data block" number */
	db = bnb + trans_number_block_count(state->blocksize);
	for(block = 0; block < cr->nblocks; block += bnpb)
	{
		uint32_t index, max = MIN(bnpb, cr->nblocks - block);
		uint32_t * numbers;
		bdesc_t * number_block = CALL(state->journal, get_file_block, state->jfdesc, bnb * state->blocksize);
		if(!number_block)
		{
			bdesc_release(&commit_block);
			return -E_UNSPECIFIED;
		}
		bdesc_retain(&number_block);
		
		numbers = (uint32_t *) number_block->ddesc->data;
		for(index = 0; index != max; index++)
		{
			int r = -E_UNSPECIFIED;
			bdesc_t * output;
			bdesc_t * data_block = CALL(state->journal, get_file_block, state->jfdesc, db++ * state->blocksize);
			if(!data_block)
				goto data_error;
			bdesc_retain(&data_block);
			
			output = CALL(state->queue, read_block, numbers[index]);
			if(!output)
				goto output_error;
			
			//Dprintf("%s(): recovering block %d from journal entry %d\n", __FUNCTION__, numbers[index], block + index);
			if((r = bdesc_touch(output)) < 0)
				goto touch_error;
			
			memcpy(output->ddesc->data, data_block->ddesc->data, state->blocksize);
			CALL(state->queue, write_block, output);
			bdesc_drop(&data_block);
			continue;
			
		  touch_error:
			bdesc_drop(&output);
		  output_error:
			bdesc_retain(&data_block);
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
		const typeof(cr->type) empty = CREMPTY;
		CALL(state->journal, write_block, commit_block, (uint16_t) &((struct commit_record *) NULL)->type, sizeof(cr->type), &empty, NULL, NULL);
	}
	bdesc_release(&commit_block);
	
	return 0;
}

static int replay_journal(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	
	const uint32_t transaction_blocks = TRANSACTION_SIZE / state->blocksize;
	uint32_t transaction;

	Dprintf("Journal: %d transactions, %d blocks each.\n", state->ncommit_records, transaction_blocks);
	for(transaction = 0; transaction < state->ncommit_records; transaction++)
	{
		int r = replay_single_transaction(state, transaction * transaction_blocks, CRCOMMIT);
		if(r < 0)
			return r;
	}
	
	return 0;
}

static int transaction_start(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	return journal_queue_hold(state->queue);
}


typedef struct {
	bdesc_t * bdesc;
	commit_record_t * cr;
} commit_record_holder_t;

static int transaction_stop_slot(journal_state_t * state, uint16_t slot, uint16_t next_slot, uint16_t type, bdesc_t **data_bdescs, size_t ndatabdescs, commit_record_holder_t * crh)
{
	Dprintf("%s(slot %u, next_slot %u, type %u)\n", __FUNCTION__, slot, next_slot, type);
	size_t file_offset;
	bdesc_t * bdesc;
	size_t i;
	int r;

	file_offset = slot * TRANSACTION_SIZE;
	const size_t blknos_begin = file_offset + state->blocksize;
	const size_t blknos_end = blknos_begin + state->blocksize*(trans_number_block_count(state->blocksize) - 1);


	//
	// Create and write journal transaction entries.
	//
	// Dependencies:
	// - commit record -> journal data
	// - fs data -> commit record
	// - commit invalidation -> fs data

	// jdata_chdescs will depend on all journal data chdescs
	chdesc_t * jdata_chdescs;
	// fsdata_chdescs will depend on all fs data chdescs
	chdesc_t * fsdata_chdescs;
	commit_record_t commit;
	size_t commit_offset;
	// lfs_head/tail used to check that no metadata changes upon journal writes
	chdesc_t * lfs_head;
	chdesc_t * lfs_tail;
	chdesc_t * prev_head;
	chdesc_t * tail;

	jdata_chdescs = chdesc_create_noop(NULL);
	if (!jdata_chdescs)
		return -E_NO_MEM;

	fsdata_chdescs = chdesc_create_noop(NULL); 
	if (!fsdata_chdescs)
	{
		chdesc_destroy(&jdata_chdescs);
		return -E_NO_MEM;
	}

	r = depman_add_chdesc(jdata_chdescs);
	if (r < 0)
	{
		chdesc_destroy(&fsdata_chdescs);
		chdesc_destroy(&jdata_chdescs);
		return r;
	}

	r = depman_add_chdesc(fsdata_chdescs);
	if (r < 0)
	{
		// Ignore possible depman error
		(void) depman_remove_chdesc(jdata_chdescs);
		chdesc_destroy(&fsdata_chdescs);
		return r;
	}

	r = journal_queue_passthrough(state->queue);
	assert(r >= 0);

	// save space for the commit record
	commit_offset = file_offset;
	file_offset = blknos_end + state->blocksize;

	// Create journal data

	for (i=0; i < ndatabdescs; i++, file_offset += state->blocksize)
	{
		bdesc = CALL(state->journal, get_file_block, state->jfdesc, file_offset);
		assert(bdesc); // TODO: handle error

		// TODO: does journal data need to depend on anything, in case of small cache?
		prev_head = NULL;
		r = chdesc_create_full(bdesc, data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
		r = depman_add_chdesc(prev_head);
		assert(r >= 0); // TODO: handle error
		r = chdesc_add_depend(jdata_chdescs, prev_head);
		assert(r >= 0); // TODO: handle error

		lfs_head = NULL;
		lfs_tail = NULL;
		r = CALL(state->journal, write_block, bdesc, data_bdescs[i]->offset, data_bdescs[i]->length, data_bdescs[i]->ddesc->data, &lfs_head, &lfs_tail);
		assert(r >= 0); // TODO: handle error
		//assert(!lfs_head && !lfs_tail);
	}


	// Write journal data block numbers

	{
		const size_t blknos_per_block = numbers_per_block(state->blocksize);
		const size_t nblocks_jdbn = trans_number_block_count(state->blocksize);
		size_t blkno;
		size_t bdescno = 0;
		uint32_t * num_block;
		uint32_t * cur_blkno_entry;

		num_block = malloc(state->blocksize);
		assert(num_block); // TODO: handle error

		for (blkno=0; blkno < nblocks_jdbn && bdescno < ndatabdescs; blkno++)
		{
			// unused space can have any value, fill with 0xff for readability
			memset(num_block, 0xff, state->blocksize);
			// set cur_blkno_entry to the beginning
			cur_blkno_entry = num_block;

			for (i=0; i < blknos_per_block && bdescno < ndatabdescs; i++, bdescno++, cur_blkno_entry++)
				*cur_blkno_entry = data_bdescs[bdescno]->number;

			bdesc = CALL(state->journal, get_file_block, state->jfdesc, blknos_begin + blkno * state->blocksize);
			assert(bdesc); // TODO: handle error

			prev_head = NULL;
			r = chdesc_create_full(bdesc, num_block, &prev_head, &tail);
			assert(r >= 0); // TODO: handle error
			r = depman_add_chdesc(prev_head);
			assert(r >= 0); // TODO: handle error
			r = chdesc_add_depend(jdata_chdescs, prev_head);
			assert(r >= 0); // TODO: handle error

			// retain for commit record
			r = chdesc_weak_retain(prev_head, &prev_head);
			assert(r >= 0); // TODO: handle error

			lfs_head = NULL;
			lfs_tail = NULL;
			r = CALL(state->journal, write_block, bdesc, 0, state->blocksize, num_block, &lfs_head, &lfs_tail);
			assert(r >= 0); // TODO: handle error
			//assert(!lfs_head && !lfs_tail);

			chdesc_weak_forget(&prev_head);
		}

		free(num_block);
		num_block = NULL;
		cur_blkno_entry = NULL;
	}


	// Create commit record

	commit.magic = JOURNAL_MAGIC;
	commit.type = type;
	commit.next = next_slot;
	commit.nblocks = ndatabdescs;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, commit_offset);
	assert(bdesc); // TODO: handle error

	prev_head = jdata_chdescs;
	r = chdesc_create_byte(bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	r = depman_add_chdesc(prev_head);
	assert(r >= 0); // TODO: handle error

	// retain for creating fsdata_chdescs
	r = chdesc_weak_retain(prev_head, &prev_head);
	assert(r >= 0); // TODO: handle error

	lfs_head = NULL;
	lfs_tail = NULL;
	// this single line atomically commits this transaction to disk
	r = CALL(state->journal, write_block, bdesc, 0, sizeof(commit), &commit, &lfs_head, &lfs_tail);
	assert(r >= 0); // TODO: handle error
	//assert(!lfs_head && !lfs_tail);

	chdesc_t * commit_chdesc = prev_head;
	chdesc_weak_forget(&prev_head);

	// create fsdata_chdescs
	for (i=0; i < ndatabdescs; i++)
	{
		prev_head = commit_chdesc;
		// TODO: this copies each bdescs' data. All we really want to
		// to make dependencies:
		r = chdesc_create_full(data_bdescs[i], data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
		r = depman_add_chdesc(prev_head);
		assert(r >= 0); // TODO: handle error
		r = chdesc_add_depend(fsdata_chdescs, prev_head);
		assert(r >= 0); // TODO: handle error
	}


	// Mark as invalidated

	commit.type = CREMPTY;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, commit_offset);
	assert(bdesc); // TODO: handle error

	prev_head = fsdata_chdescs;
	r = chdesc_create_byte(bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	r = depman_add_chdesc(prev_head);
	assert(r >= 0); // TODO: handle error

	r = chdesc_weak_retain(prev_head, &state->commit_chdesc[slot]);
	assert(r >= 0); // TODO: handle error

	// save the bdesc and commit record for later marking as invalidated
	crh->bdesc = bdesc;
	crh->cr = &commit;

	return 0;
}

static size_t use_next_trans_slot(journal_state_t * state)
{
	size_t slot;
	int r;

	slot = state->next_trans_slot;
	state->next_trans_slot = (state->next_trans_slot+1) % state->ncommit_records;

	if (state->commit_chdesc[slot])
	{
		// TODO: Make this transaction dependent on the non-synced
		// transaction's invalid chdesc.
		// For now, sync this transaction slot.
		// NOTE: if we do update this to depend rather than sync,
		bdesc_t * bdesc;

		bdesc = state->commit_chdesc[slot]->block;
			
		r = CALL(bdesc->bd, sync, bdesc);
		assert(r >= 0); // TODO: handle error
		assert(!state->commit_chdesc[slot]);
	}

	return slot;
}

static int transaction_stop(journal_state_t * state)
{
	Dprintf("%s() [transaction slot %u]\n", __FUNCTION__, state->next_trans_slot);
	bdesc_t ** data_bdescs;
	size_t ndatabdescs;
	bdesc_t * bdesc;
	size_t i;
	int r;


	//
	// Sort the data_bdescs, allowing for faster disk access.
	// TODO: it'd be nice if this also sorted journal_queue's copy.

	{
		const hash_map_t * data_bdescs_map; // blockno -> bdesc_t *
		hash_map_it_t * it;

		data_bdescs_map = journal_queue_blocklist(state->queue);
		if (!data_bdescs_map)
			return 0; // nothing to journal

		ndatabdescs = hash_map_size(data_bdescs_map);

		// Do no work if no entries.
		if (!ndatabdescs)
			return 0;

		data_bdescs = malloc(ndatabdescs * sizeof(*data_bdescs));
		if (!data_bdescs)
			return -E_NO_MEM;

		it = hash_map_it_create();
		if (!it)
		{
			free(data_bdescs);
			return -E_NO_MEM;
		}

		i = 0;
		while ((bdesc = hash_map_val_next((hash_map_t *) data_bdescs_map, it)))
			data_bdescs[i++] = bdesc;
		assert(i == ndatabdescs);

		qsort(data_bdescs, ndatabdescs, sizeof(*data_bdescs), bdesc_blockno_compare);

		hash_map_it_destroy(it);
	}


	//
	// Perform the journaling.
	// When breaking this transaction into subtransactions, link them
	// up in reverse so that the last slot is the CRCOMMIT.

	const size_t max_datablks_per_trans = TRANSACTION_SIZE/state->blocksize - trans_number_block_count(state->blocksize) - 1;
	const size_t num_subtransactions = ROUNDUP32(ndatabdescs, max_datablks_per_trans) / max_datablks_per_trans;
	commit_record_holder_t * chrs;
	size_t prev_slot = -1;

	if (num_subtransactions > state->ncommit_records)
		panic("num_subtransactions (%u) > state->ncommit_records (%u), death!", num_subtransactions, state->ncommit_records);

	chrs = malloc(num_subtransactions * sizeof(chrs[0]));
	assert(chrs); // TODO: handle error

	for (i=0; i < ndatabdescs; i += max_datablks_per_trans)
	{
		size_t slot;
		uint32_t nblocks;
		uint16_t type;

		slot = use_next_trans_slot(state);

		if (!i)
			prev_slot = slot; // indicates this is the beginning of the chain

		nblocks = MIN(ndatabdescs - i, max_datablks_per_trans);

		if (i + max_datablks_per_trans < ndatabdescs)
			type = CRSUBCOMMIT;
		else
			type = CRCOMMIT;

		r = transaction_stop_slot(state, slot, prev_slot, type, data_bdescs+i, nblocks, &chrs[i/max_datablks_per_trans]);
		if (r < 0)
		{
			free(data_bdescs);
			return r;
		}

		prev_slot = slot;
	}

	//
	// Release the data bdescs and mark the commit records as invalidated

	// Release the data bdescs.
	// Release before marking invalid to allow the bds under the journal
	// to force fs data syncing.

	r = journal_queue_release(state->queue);
	if (r < 0)
	{
		free(data_bdescs);
		free(chrs);
		return r;
	}

	for (i=0; i < num_subtransactions; i++)
	{
		chdesc_t * lfs_head = NULL;
		chdesc_t * lfs_tail = NULL;
		r = CALL(state->journal, write_block, chrs[i].bdesc, 0, sizeof(chrs[i].cr), &chrs[i].cr, &lfs_head, &lfs_tail);
		assert(r >= 0); // TODO: handle error
		//assert(!lfs_head && !lfs_tail);
	}


	free(data_bdescs);
	free(chrs);

	return 0;
}


//
//

// Start a new transaction at each callback.
static void timer_callback(void * arg)
{
	journal_state_t * state = (journal_state_t *) arg;
	int r;

	r = transaction_stop(state);
	if (r < 0)
		fprintf(STDERR_FILENO, "%s:%s: transaction_stop: %e\n", __FILE__, __FUNCTION__, r);

	r = transaction_start(state);
	if (r < 0)
		fprintf(STDERR_FILENO, "%s:%s: transaction_start: %e\n", __FILE__, __FUNCTION__, r);
}


//
// Intercepted LFS_t functions

static int journal_sync(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r, fs_r;

	r = transaction_stop(state);
	if (r < 0)
		return r;

	fs_r = CALL(state->fs, sync, name);
	if (fs_r < 0)
		return fs_r;

	r = transaction_start(state);
	if (r < 0)
		return r;

	return fs_r;
}

static int journal_destroy(LFS_t * lfs)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;

	r = transaction_stop(state);
	if (r < 0)
		return r;

	CALL(state->journal, free_fdesc, state->jfdesc);
	state->jfdesc = NULL;

	for(r = 0; r < state->ncommit_records; r++)
		if(state->commit_chdesc[r])
			chdesc_weak_release(&state->commit_chdesc[r]);
	free(state->commit_chdesc);

	free(lfs->instance);
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	return 0;
}


//
// Passthrough LFS_t functions using chdescs

static void eat_chdesc_graph(chdesc_t * c)
{
	chmetadesc_t * scan;
	int r;

	// eat_chdesc_graph() can be passed NULL, watch out:
	if (!c)
		return;

	while ((scan = c->dependencies))
		eat_chdesc_graph(scan->desc);

	r = depman_remove_chdesc(c);
	assert(r >= 0);
}

static bdesc_t * journal_allocate_block(LFS_t * lfs, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	bdesc_t * val;
	val = CALL(state->fs, allocate_block, size, purpose, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return val;
}

static int journal_append_file_block(LFS_t * lfs, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, append_file_block, file, block, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static fdesc_t * journal_allocate_name(LFS_t * lfs, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	fdesc_t * val;
	val = CALL(state->fs, allocate_name, name, type, link, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return val;
}

static int journal_rename(LFS_t * lfs, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, rename, oldname, newname, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static bdesc_t * journal_truncate_file_block(LFS_t * lfs, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	bdesc_t * val;
	val = CALL(state->fs, truncate_file_block, file, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return val;
}

static int journal_free_block(LFS_t * lfs, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, free_block, block, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static int journal_remove_name(LFS_t * lfs, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, remove_name, name, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static int journal_write_block(LFS_t * lfs, bdesc_t * block, uint32_t offset, uint32_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, write_block, block, offset, size, data, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static int journal_set_metadata_name(LFS_t * lfs, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, set_metadata_name, name, id, size, data, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}

static int journal_set_metadata_fdesc(LFS_t * lfs, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	int r;
	r = CALL(state->fs, set_metadata_fdesc, file, id, size, data, head, tail);
	eat_chdesc_graph(*head);
	*head = *tail = NULL;
	return r;
}


//
// Passthrough LFS_t functions not using chdescs

static uint32_t journal_get_blocksize(LFS_t * lfs)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return state->blocksize;
}

static BD_t * journal_get_blockdev(LFS_t * lfs)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_blockdev);
}

static bdesc_t * journal_lookup_block(LFS_t * lfs, uint32_t number, uint32_t offset, uint32_t size)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, lookup_block, number, offset, size);
}

static fdesc_t * journal_lookup_name(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, lookup_name, name);
}

static void journal_free_fdesc(LFS_t * lfs, fdesc_t * fdesc)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, free_fdesc, fdesc);
}

static uint32_t journal_get_file_numblocks(LFS_t * lfs, fdesc_t * file)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_file_numblocks, file);

}

static bdesc_t * journal_get_file_block(LFS_t * lfs, fdesc_t * file, uint32_t offset)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_file_block, file, offset);
}

static int journal_get_dirent(LFS_t * lfs, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_dirent, file, entry, size, basep);
}

static size_t journal_get_num_features(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_num_features, name);
}

static const feature_t * journal_get_feature(LFS_t * lfs, const char * name, size_t num)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_feature, name, num);
}

static int journal_get_metadata_name(LFS_t * lfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_metadata_name, name, id, size, data);
}

static int journal_get_metadata_fdesc(LFS_t * lfs, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	journal_state_t * state = (journal_state_t *) lfs->instance;
	return CALL(state->fs, get_metadata_fdesc, file, id, size, data);
}


//
//

LFS_t * journal_lfs(LFS_t * journal, LFS_t * fs, BD_t * fs_queue)
{
	LFS_t * lfs;
	journal_state_t * state;
	uint16_t blocksize;
	BD_t * journal_bd;
	int r;

	if (!journal || !fs || !fs_queue)
		return NULL;

	// Check that queue is valid and directly below the base lfs.
	// It is not stricly necessary that queue be directly below base lfs,
	// but at least for now we assume this.
	if (fs_queue != CALL(fs, get_blockdev))
		return NULL;
	if (!journal_queue_detect(fs_queue))
		return NULL;

	// Make sure the journal device has the same block size as the
	// filesystem, for better performance
	blocksize = CALL(fs, get_blocksize);
	if (blocksize != CALL(journal, get_blocksize))
		return NULL;

	// Make sure the atomic size of the journal device is big enough
	journal_bd = CALL(journal, get_blockdev);
	if (sizeof(commit_record_t) > CALL(journal_bd, get_atomicsize))
		return NULL;

	lfs = malloc(sizeof(*lfs));
	if (!lfs)
		return NULL;

	state = malloc(sizeof(*state));
	if (!state)
		goto error_lfs;
	lfs->instance = state;

	ASSIGN(lfs, journal, get_blocksize);
	ASSIGN(lfs, journal, get_blockdev);
	ASSIGN(lfs, journal, allocate_block);
	ASSIGN(lfs, journal, lookup_block);
	ASSIGN(lfs, journal, lookup_name);
	ASSIGN(lfs, journal, free_fdesc);
	ASSIGN(lfs, journal, get_file_numblocks);
	ASSIGN(lfs, journal, get_file_block);
	ASSIGN(lfs, journal, get_dirent);
	ASSIGN(lfs, journal, append_file_block);
	ASSIGN(lfs, journal, allocate_name);
	ASSIGN(lfs, journal, rename);
	ASSIGN(lfs, journal, truncate_file_block);
	ASSIGN(lfs, journal, free_block);
	ASSIGN(lfs, journal, remove_name);
	ASSIGN(lfs, journal, write_block);
	ASSIGN(lfs, journal, get_num_features);
	ASSIGN(lfs, journal, get_feature);
	ASSIGN(lfs, journal, get_metadata_name);
	ASSIGN(lfs, journal, get_metadata_fdesc);
	ASSIGN(lfs, journal, set_metadata_name);
	ASSIGN(lfs, journal, set_metadata_fdesc);
	ASSIGN(lfs, journal, sync);
	ASSIGN_DESTROY(lfs, journal, destroy);

	state->queue = fs_queue;
	state->journal = journal;
	state->jfdesc = NULL;
	state->fs = fs;
	state->blocksize = blocksize;
	state->next_trans_slot = 0;

	r = ensure_journal_exists(state);
	if (r < 0)
		goto error_state;

	state->ncommit_records = CALL(state->journal, get_file_numblocks, state->jfdesc) / (TRANSACTION_SIZE / state->blocksize);
	if (!state->ncommit_records)
	{
		fprintf(STDERR_FILENO, "Not enough room in journal file for even one transaction.\n");
		goto error_state;
	}

	state->commit_chdesc = calloc((size_t) state->ncommit_records, sizeof(*state->commit_chdesc));
	if (!state->commit_chdesc)
		goto error_state;

	r = replay_journal(state);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "Unable to replay journal.\n");
		goto error_chdescs;
	}

	r = transaction_start(state);
	if (r < 0)
		goto error_chdescs;

	r = sched_register(timer_callback, state, 5*100);
	if (r < 0)
		goto error_chdescs;

	return lfs;

  error_chdescs:
	free(state->commit_chdesc);
  error_state:
	free(state);
  error_lfs:
	free(lfs);
	return NULL;
}
