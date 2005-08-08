#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/hash_map.h>

#include <kfs/bdesc.h>
#include <kfs/fdesc.h>
#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/journal_lfs.h>

#define JOURNAL_PROGRESS_ENABLED
#define JOURNAL_PROGRESS_COLOR 14

#define JOURNAL_DEBUG 0

#if JOURNAL_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/*
 * precondition: CHDESC_PRMARKED is set to 0 for each chdesc in graph.
 *
 * postconditions: CHDESC_PRMAKRED is set to 1 for each chdesc in graph.
 *
 * printf() out all the dependencies.
 *
 * Imported from wb_cache_bd.c (currently not in the build).
 */
static void
print_chdescs(chdesc_t *ch, int num)
{
	chmetadesc_t *p;
	int i;

	for (i = 0; i < num; i++)
		printf("  ");
	switch (ch->type) {
	case BIT:
		printf("ch: 0x%08x BIT dist %d block %d off 0x%x",
			   ch, ch->distance,
			   ch->block->number, ch->bit.offset);
		break;
	case BYTE:
		printf("ch: 0x%08x BYTE dist %d block %d off 0x%x len %d",
			   ch, ch->distance, ch->block->number,
			   ch->byte.offset, ch->byte.length);
		break;
	case NOOP:
		printf("ch: 0x%08x NOOP dist %d", ch, ch->distance);
		break;
	default:
		printf("ch: 0x%08x UNKNOWN TYPE!! type: 0x%x", ch, ch->type);
	}

	if (ch->flags & CHDESC_PRMARKED) {
		printf(" (repeat)\n");
		return;
	} else
		printf("\n");
	ch->flags |= CHDESC_PRMARKED;

	p = ch->dependencies;
	while (p) {
		print_chdescs(p->desc, num+1);
		p = p->next;
	}
}

/*
 * precondition: CHDESC_PRMARKED is set to 1 for each chdesc in graph.
 * postcondition: CHDESC_PRMARKED is set to 0 for each chdesc in graph.
 *
 * Imported from wb_cache_bd.c (currently not in the build).
 */
static void
reset_prmarks(chdesc_t *ch)
{
	chmetadesc_t *p;
	if (!(ch->flags & CHDESC_PRMARKED)) return;
	ch->flags &= ~CHDESC_PRMARKED;
	p = ch->dependencies;
	while (p) {
		reset_prmarks(p->desc);
		p = p->next;
	}
}

struct journal_state {
	BD_t * queue; // the journal_queue_bd
	LFS_t * journal; // the LFS containing the journal file
	fdesc_t * jfdesc; // the fdesc for the journal file
	LFS_t * fs; // the LFS being journaled
	chdesc_t ** commit_chdesc; // ptrs to each record's commit chdesc
	uint16_t ncommit_records, next_trans_slot;
	uint16_t blocksize;
#ifdef JOURNAL_PROGRESS_ENABLED
	size_t jbdescs_size, njbdescs_released, disp_ncols, disp_period, disp_prev;
#endif
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

#define TRANSACTION_PERIOD 5
#define TRANSACTION_SIZE (64*4096)
static const char journal_filename[] = "/.journal";

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
		bdesc_retain(number_block);
		
		numbers = (uint32_t *) number_block->ddesc->data;
		for(index = 0; index != max; index++)
		{
			int r = -E_UNSPECIFIED;
			bdesc_t * output;
			bdesc_t * data_block = CALL(state->journal, get_file_block, state->jfdesc, db++ * state->blocksize);
			if(!data_block)
				goto data_error;
			bdesc_retain(data_block);
			
			output = CALL(state->queue, read_block, numbers[index]);
			if(!output)
				goto output_error;
			
			//Dprintf("%s(): recovering block %d from journal entry %d\n", __FUNCTION__, numbers[index], block + index);
			
			CALL(state->journal, write_block, output, 0, state->blocksize, data_block->ddesc->data, NULL, NULL);
			bdesc_release(&data_block);
			continue;
			
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
	chdesc_t * chdesc;
} commit_record_holder_t;

static int transaction_stop_slot(journal_state_t * state, uint16_t slot, uint16_t next_slot, uint16_t type, bdesc_t **data_bdescs, size_t ndatabdescs, commit_record_holder_t * crh)
{
	Dprintf("%s(nblocks %u, slot %u, next_slot %u, type %u)\n", __FUNCTION__, ndatabdescs, slot, next_slot, type);
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
	chdesc_t * prev_head;
	chdesc_t * tail;
	BD_t * journal_bd = CALL(state->journal, get_blockdev);
	BD_t * fs_bd      = CALL(state->fs, get_blockdev);

	jdata_chdescs = chdesc_create_noop(NULL, journal_bd);
	if (!jdata_chdescs)
		return -E_NO_MEM;

	// retain so that "add_depend() will often segfault if we don't" doesn't break.
	// TODO: why is this retain needed?
	r = chdesc_weak_retain(jdata_chdescs, &jdata_chdescs);
	assert(r >= 0);

	fsdata_chdescs = chdesc_create_noop(NULL, fs_bd); 
	if (!fsdata_chdescs)
	{
		chdesc_destroy(&jdata_chdescs);
		return -E_NO_MEM;
	}

	r = journal_queue_passthrough(state->queue);
	assert(r >= 0);

	// save space for the commit record
	commit_offset = file_offset;
	file_offset = blknos_end + state->blocksize;

	// Create journal data

	for (i=0; i < ndatabdescs; i++, file_offset += state->blocksize)
	{
		//uint32_t bdesc_number = CALL(state->journal, get_file_block_num, state->jfdesc, file_offset);
		//assert(bdesc_number != -1);
		//bdesc = bdesc_alloc(bdesc_number, state->blocksize);
		bdesc = CALL(state->journal, get_file_block, state->jfdesc, file_offset);
		assert(bdesc); // TODO: handle error

		// TODO: does journal data need to depend on anything, in case of small cache?
		prev_head = NULL;
/*
		r = chdesc_create_full(bdesc, journal_bd, data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
*/

		r = CALL(state->journal, write_block, bdesc, 0, data_bdescs[i]->ddesc->length, data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
		//assert(!lfs_head && !lfs_tail);

		r = chdesc_add_depend(jdata_chdescs, prev_head);
		assert(r >= 0); // TODO: handle error

		// TODO: push down/move jdata_chdescs too?

#ifdef JOURNAL_PROGRESS_ENABLED
		if (++state->njbdescs_released >= state->disp_prev + state->disp_period)
		{
			r = textbar_set_progress(state->njbdescs_released * state->disp_ncols / state->jbdescs_size, JOURNAL_PROGRESS_COLOR);
			assert(r >= 0);
			state->disp_prev = state->njbdescs_released;
		}
#endif
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

			// TODO: remove this need to read the block, it is immediately overwriten
			bdesc = CALL(state->journal, get_file_block, state->jfdesc, blknos_begin + blkno * state->blocksize);
			assert(bdesc); // TODO: handle error

			prev_head = NULL;
/*
			r = chdesc_create_full(bdesc, journal_bd, num_block, &prev_head, &tail);
			assert(r >= 0); // TODO: handle error

			// retain for commit record
			r = chdesc_weak_retain(prev_head, &prev_head);
			assert(r >= 0); // TODO: handle error
*/
			r = CALL(state->journal, write_block, bdesc, 0, state->blocksize, num_block, &prev_head, &tail);
			assert(r >= 0); // TODO: handle error
			//assert(!lfs_head && !lfs_tail);

			r = chdesc_add_depend(jdata_chdescs, prev_head);
			assert(r >= 0); // TODO: handle error
/*
			chdesc_weak_forget(&prev_head);
*/
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
/*
	r = chdesc_create_byte(bdesc, journal_bd, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error

	// retain for creating fsdata_chdescs
	r = chdesc_weak_retain(prev_head, &prev_head);
	assert(r >= 0); // TODO: handle error
*/

	// this single line atomically commits this transaction to disk
	r = CALL(state->journal, write_block, bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	//assert(!lfs_head && !lfs_tail);

	chdesc_weak_release(&jdata_chdescs);

	chdesc_t * commit_chdesc = prev_head;
/*
	chdesc_weak_forget(&prev_head);
*/
	// create fsdata_chdescs
	for (i=0; i < ndatabdescs; i++)
	{
/*
		prev_head = commit_chdesc;

		// TODO: this copies each bdescs' data. All we really want to
		// to make dependencies:
		r = chdesc_create_full(data_bdescs[i], fs_bd, data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
		r = chdesc_add_depend(fsdata_chdescs, prev_head);
		assert(r >= 0); // TODO: handle error
*/
/*
		To replace the previous:
		- add commit_chdesc as dependency for data_bdescs[i] 
		- add data_bdescs[i] as dependency for fsdata_chdescs
*/
	}


	// Prepare to mark as invalidated, caller will do the actual write

	commit.type = CREMPTY;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, commit_offset);
	assert(bdesc); // TODO: handle error

	prev_head = fsdata_chdescs;
/*
	r = chdesc_create_byte(bdesc, fs_bd, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
*/
/*
	r = chdesc_weak_retain(prev_head, &state->commit_chdesc[slot]);
	assert(r >= 0); // TODO: handle error
*/
	state->commit_chdesc[slot] = NULL;

	// save to later mark as invalidated
	crh->bdesc = bdesc; // no need to retain since we've not written it
	crh->cr = &commit;
	crh->chdesc = NULL;
/*
	r = chdesc_weak_retain(prev_head, &crh->chdesc);
	assert(r >= 0); // TODO: handle error
*/
	crh->chdesc = NULL;

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
		uint32_t number = state->commit_chdesc[slot]->block->number;
		
		r = CALL(state->commit_chdesc[slot]->owner, sync, number, NULL);
		assert(r >= 0); // TODO: handle error
		assert(!state->commit_chdesc[slot]);
	}

	return slot;
}

static int transaction_stop(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	bdesc_t ** data_bdescs;
	size_t ndatabdescs;
#ifndef JOURNAL_QUEUE_VECTOR
	bdesc_t * bdesc;
#endif
	size_t i;
	int r;

	//
	// Sort the data_bdescs, allowing for faster disk access.
	// TODO: it'd be nice if this also sorted journal_queue's copy.

	{
#ifndef JOURNAL_QUEUE_VECTOR
		const hash_map_t * data_bdescs_map; // blockno -> bdesc_t *
		hash_map_it_t it;

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

		hash_map_it_init(&it, (hash_map_t *) data_bdescs_map);

		i = 0;
		while ((bdesc = hash_map_val_next(&it)))
			data_bdescs[i++] = bdesc;
		assert(i == ndatabdescs);

		qsort(data_bdescs, ndatabdescs, sizeof(*data_bdescs), bdesc_blockno_compare);
#else
		const vector_t * data_bdescs_vec = journal_queue_blocklist(state->queue);
		if (!data_bdescs_vec)
			return 0;

		ndatabdescs = vector_size(data_bdescs_vec);

		if (!ndatabdescs)
			return 0;

		data_bdescs = malloc(ndatabdescs * sizeof(*data_bdescs));
		if (!data_bdescs)
			return -E_NO_MEM;

		for (i=0; i < ndatabdescs; i++)
			data_bdescs[i] = vector_elt((vector_t *) data_bdescs_vec, i);
#endif
	}

#ifdef DO_JOURNALING
	//
	// Perform the journaling.
	// When breaking this transaction into subtransactions, link them
	// up in reverse so that the last slot is the CRCOMMIT.

	const size_t max_datablks_per_trans = TRANSACTION_SIZE/state->blocksize - trans_number_block_count(state->blocksize) - 1;
	const size_t num_subtransactions = (ndatabdescs + max_datablks_per_trans - 1) / max_datablks_per_trans;
	commit_record_holder_t * chrs;
	size_t prev_slot = -1;

	if (num_subtransactions > state->ncommit_records)
	{
		fprintf(STDERR_FILENO, "WARNING: Journal queue larger than journal, writing and syncing fs... ");

		r = journal_queue_release(state->queue);
		if (r < 0)
		{
			fprintf(STDERR_FILENO, "error releasing journal queue, your future looks dark.\n");
			free(data_bdescs);
			return r;
		}

		CALL(state->journal, sync, NULL);

		fprintf(STDERR_FILENO, "success.\n");

		free(data_bdescs);
		return 0;
	}

#ifdef JOURNAL_PROGRESS_ENABLED
	state->jbdescs_size = ndatabdescs;
	state->njbdescs_released = 0;
	state->disp_prev = 0;
	r = textbar_init(-1);
	assert(r >= 0);
	state->disp_ncols = r;
	state->disp_period = (state->jbdescs_size + state->disp_ncols - 1) / state->disp_ncols;
#endif

	chrs = malloc(num_subtransactions * sizeof(*chrs));
	assert(chrs); // TODO: handle error
	for (i=0; i < num_subtransactions; i++)
		chrs[i].chdesc = NULL;

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
			for (i=0; i < num_subtransactions; i++)
				chdesc_weak_release(&chrs[i].chdesc);
			free(chrs);
			return r;
		}

		prev_slot = slot;
	}

#ifdef JOURNAL_PROGRESS_ENABLED
	r = textbar_close();
	assert(r >= 0);
#endif
#endif // DO_JOURNALING

	// Remove all inter-ddesc dependencies to allow the journal_queue to write blocks
	// in an arbitrary order.
	// This code will later be enhanced to do more useful, journaling dependency manipulation,
	// but this inter-ddesc dep removeal is a solid first step.

	{
		Dprintf("//== BEGIN REMOVE EXTERN DEPS\n");
		const hash_map_t * map = journal_queue_blocklist(state->queue);
		hash_map_it_t it;
		hash_map_it_init(&it, (hash_map_t *) map);
		bdesc_t * bdesc;

		while ((bdesc = hash_map_val_next(&it)))
		{
			chdesc_t * changes = bdesc->ddesc->changes;
			chmetadesc_t * scan;
			size_t deps_len = 0;

			Dprintf("/--- %d before\n", bdesc->number);
#if JOURNAL_DEBUG
			print_chdescs(bdesc->ddesc->changes, 0);
			reset_prmarks(bdesc->ddesc->changes);
#endif

			for (scan = changes->dependencies; scan; scan = scan->next)
			{
				if (bdesc->ddesc != scan->desc->block->ddesc)
				{
					if (state->queue == scan->desc->owner)
						assert(0);
					else
						Dprintf("+");
				}
				else
				{
					chmetadesc_t * s;
					for (s = scan->desc->dependencies; s; s = s->next)
					{
						if (bdesc->ddesc != s->desc->block->ddesc)
						{
							assert(scan->desc->owner); // haven't thought this case out
							if (state->queue == scan->desc->owner)
								deps_len++;
							else
								Dprintf("=");
						}
					}
				}
			}

			Dprintf("= %d deps to remove\n", deps_len);
			deps_len *= 2;
			chdesc_t ** deps = malloc(deps_len*sizeof(*deps));
			assert(deps);
			memset(deps, 0, deps_len*sizeof(*deps)); // zero for chdesc_weak_retain()
			size_t i = 0;

			for (scan = changes->dependencies; scan; scan = scan->next)
			{
				if (bdesc->ddesc != scan->desc->block->ddesc)
				{
					if (state->queue == scan->desc->owner)
						assert(0);
				}
				else
				{
					chmetadesc_t * s;
					for (s = scan->desc->dependencies; s; s = s->next)
					{
						if (bdesc->ddesc != s->desc->block->ddesc)
						{
							if (state->queue == scan->desc->owner)
							{
								Dprintf("remember chdesc 0x%08x <- 0x%08x, block %d\n", s->desc, scan->desc, s->desc->block->number);
								assert(i+1 < deps_len);
								r = chdesc_weak_retain(scan->desc, &deps[i++]);
								assert(r >= 0);
								r = chdesc_weak_retain(s->desc, &deps[i++]);
								assert(r >= 0);
							}
							else
								Dprintf("not remembering 0x%08x <- 0x%08x, block %d\n", s->desc, scan->desc, s->desc->block->number);
						}
					}
				}
			}
			assert(i == deps_len);

			for (i=0; i < deps_len && changes; i += 2)
			{
				chdesc_weak_forget(&deps[i]);
				chdesc_weak_forget(&deps[i+1]);
				if (deps[i] && deps[i+1])
				{
					Dprintf("remove chdesc 0x%08x <- 0x%08x, block %d\n", deps[i+1], deps[i], deps[i+1]->block->number);
					chdesc_remove_depend(deps[i], deps[i+1]);
				}
				else
				{
					Dprintf("not removing, dept 0x%08x, depy 0x%08x\n", deps[i], deps[i+1]);
				}
			}
			free(deps);

			Dprintf("|--- %d removed\n", bdesc->number);
#if JOURNAL_DEBUG
			print_chdescs(bdesc->ddesc->changes, 0);
			reset_prmarks(bdesc->ddesc->changes);
#endif
		}
	}

	//
	// Release the data bdescs and mark the commit records as invalidated

	// Release the data bdescs.
	// Release before marking invalid to allow the bds under the journal
	// to force fs data syncing.

	r = journal_queue_release(state->queue);
	if (r < 0)
	{
/*
		free(data_bdescs);
		for (i=0; i < num_subtransactions; i++)
			chdesc_weak_release(&chrs[i].chdesc);
		free(chrs);
*/
		printf("%s:%s(): journal_queue_release(): %e\n", __FILE__, __FUNCTION__, r);
		return r;
	}
/*
	for (i=0; i < num_subtransactions; i++)
	{
		if (i+1 < num_subtransactions)
		{
			r = chdesc_add_depend(chrs[num_subtransactions-1].chdesc, chrs[i].chdesc);
			assert(r >= 0); // TODO: handle error
		}
		chdesc_weak_release(&chrs[i].chdesc);

		chdesc_t * lfs_head = NULL; // FIXME
		chdesc_t * lfs_tail = NULL;
		r = CALL(state->journal, write_block, chrs[i].bdesc, 0, sizeof(chrs[i].cr), &chrs[i].cr, &lfs_head, &lfs_tail);
		assert(r >= 0); // TODO: handle error
		//assert(!lfs_head && !lfs_tail);
	}


	free(data_bdescs);
	free(chrs);
*/
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
		fprintf(STDERR_FILENO, "%s:%s(): transaction_stop(): %e\n", __FILE__, __FUNCTION__, r);

	r = transaction_start(state);
	if (r < 0)
		fprintf(STDERR_FILENO, "%s:%s(): transaction_start(): %e\n", __FILE__, __FUNCTION__, r);
}


//
// Intercepted LFS_t functions

static int journal_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOURNAL_MAGIC)
		return -E_INVAL;

	switch (level)
	{
		case CONFIG_BRIEF:
			snprintf(string, length, "%u kB/s", journal_lfs_max_bandwidth(lfs));
			break;
		case CONFIG_VERBOSE:
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "max avg bandwidth: %u kB/s", journal_lfs_max_bandwidth(lfs));
	}
	return 0;
}

static int journal_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOURNAL_MAGIC)
		return -E_INVAL;
	
	snprintf(string, length, "");
	return 0;
}

static fdesc_t * journal_lookup_name(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	/* hide the journal file */
	if(state->journal == state->fs && !strcmp(name, journal_filename))
		return NULL;
	return CALL(state->fs, lookup_name, name);
}

static int journal_get_dirent(LFS_t * lfs, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int value = CALL(state->fs, get_dirent, file, entry, size, basep);
	const char * hide = (journal_filename[0] == '/') ? &journal_filename[1] : journal_filename;
	/* hide the journal filename - slight hack, hides it from all directories */
	if(value >= 0 && state->journal == state->fs && !strcmp(entry->d_name, hide))
	{
		entry->d_name[0] = 0;
		entry->d_reclen = 0;
		entry->d_namelen = 0;
	}
	return value;
}

static int journal_sync(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
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
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;

	r = transaction_stop(state);
	if (r < 0)
		return r;

	r = modman_rem_lfs(lfs);
	if(r < 0)
	{
		transaction_start(state);
		return r;
	}

	r = sched_unregister(timer_callback, state);
	if (r < 0)
		fprintf(STDERR_FILENO, "%s(): WARNING: sched_unregister(): %e\n", r);

	modman_dec_bd(state->queue, lfs);
	modman_dec_lfs(state->fs, lfs);
	modman_dec_lfs(state->journal, lfs);

	CALL(state->journal, free_fdesc, state->jfdesc);
	state->jfdesc = NULL;

	for(r = 0; r < state->ncommit_records; r++)
		if(state->commit_chdesc[r])
			chdesc_weak_release(&state->commit_chdesc[r]);
	free(state->commit_chdesc);

	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	return 0;
}


//
// Passthrough LFS_t functions using chdescs

// TODO: should these functions set *head to anything?
// (perhaps the commit record or invalidation?)

static bdesc_t * journal_allocate_block(LFS_t * lfs, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	bdesc_t * val;
	val = CALL(state->fs, allocate_block, size, purpose, head, tail);
	return val;
}

static int journal_append_file_block(LFS_t * lfs, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	r = CALL(state->fs, append_file_block, file, block, head, tail);
	return r;
}

static fdesc_t * journal_allocate_name(LFS_t * lfs, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	fdesc_t * val;
	val = CALL(state->fs, allocate_name, name, type, link, head, tail);
	return val;
}

static int journal_rename(LFS_t * lfs, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	/* hide the journal file */
	if(state->journal == state->fs && !strcmp(oldname, journal_filename))
		return -E_NOT_FOUND;
	if(state->journal == state->fs && !strcmp(newname, journal_filename))
		return -E_INVAL;
	r = CALL(state->fs, rename, oldname, newname, head, tail);
	return r;
}

static bdesc_t * journal_truncate_file_block(LFS_t * lfs, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	bdesc_t * val;
	val = CALL(state->fs, truncate_file_block, file, head, tail);
	return val;
}

static int journal_free_block(LFS_t * lfs, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	r = CALL(state->fs, free_block, block, head, tail);
	return r;
}

static int journal_remove_name(LFS_t * lfs, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	/* hide the journal file */
	if(state->journal == state->fs && !strcmp(name, journal_filename))
		return -E_NOT_FOUND;
	r = CALL(state->fs, remove_name, name, head, tail);
	return r;
}

static int journal_write_block(LFS_t * lfs, bdesc_t * block, uint32_t offset, uint32_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	r = CALL(state->fs, write_block, block, offset, size, data, head, tail);
	return r;
}

static int journal_set_metadata_name(LFS_t * lfs, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	/* hide the journal file */
	if(state->journal == state->fs && !strcmp(name, journal_filename))
		return -E_NOT_FOUND;
	r = CALL(state->fs, set_metadata_name, name, id, size, data, head, tail);
	return r;
}

static int journal_set_metadata_fdesc(LFS_t * lfs, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	int r;
	r = CALL(state->fs, set_metadata_fdesc, file, id, size, data, head, tail);
	return r;
}


//
// Passthrough LFS_t functions not using chdescs

static uint32_t journal_get_blocksize(LFS_t * lfs)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return state->blocksize;
}

static BD_t * journal_get_blockdev(LFS_t * lfs)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_blockdev);
}

static bdesc_t * journal_lookup_block(LFS_t * lfs, uint32_t number, uint32_t offset, uint32_t size)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, lookup_block, number, offset, size);
}

static void journal_free_fdesc(LFS_t * lfs, fdesc_t * fdesc)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, free_fdesc, fdesc);
}

static uint32_t journal_get_file_numblocks(LFS_t * lfs, fdesc_t * file)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_file_numblocks, file);

}

static uint32_t journal_get_file_block_num(LFS_t * lfs, fdesc_t * file, uint32_t offset)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_file_block_num, file, offset);
}

static bdesc_t * journal_get_file_block(LFS_t * lfs, fdesc_t * file, uint32_t offset)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_file_block, file, offset);
}

static size_t journal_get_num_features(LFS_t * lfs, const char * name)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_num_features, name);
}

static const feature_t * journal_get_feature(LFS_t * lfs, const char * name, size_t num)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);
	return CALL(state->fs, get_feature, name, num);
}

static int journal_get_metadata_name(LFS_t * lfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);

	/* hide the journal file */
	if(state->journal == state->fs && !strcmp(name, journal_filename))
		return -E_NOT_FOUND;

	// Intercept because journal_lfs is a higher lfs than state->fs.
	// TODO: journal_lfs should either only intercept if state->fs
	// reports to support this feature or journal_lfs ensures this
	// feature is in get_features() and get_feature().
	if (id == KFS_feature_file_lfs_name.id)
	{
		*data = strdup(name);
		if (!*data)
			return -E_NO_MEM;
		*size = strlen(*data);
		return 0;
	}
	if (id == KFS_feature_file_lfs.id)
	{
		*data = malloc(sizeof(lfs));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(lfs);
		memcpy(*data, &lfs, sizeof(lfs));
		return 0;
	}

	return CALL(state->fs, get_metadata_name, name, id, size, data);
}

static int journal_get_metadata_fdesc(LFS_t * lfs, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	journal_state_t * state = (journal_state_t *) OBJLOCAL(lfs);

	// Intercept because journal_lfs is a higher lfs than state->fs.
	// TODO: journal_lfs should either only intercept if state->fs
	// reports to support this feature or journal_lfs ensures this
	// feature is in get_features() and get_feature().
	if (id == KFS_feature_file_lfs.id)
	{
		*data = malloc(sizeof(lfs));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(lfs);
		memcpy(*data, &lfs, sizeof(lfs));
		return 0;
	}

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

	LFS_INIT(lfs, journal, state);
	OBJMAGIC(lfs) = JOURNAL_MAGIC;

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

	r = sched_register(timer_callback, state, TRANSACTION_PERIOD*100);
	if (r < 0)
		goto error_chdescs;

	if(modman_add_anon_lfs(lfs, __FUNCTION__))
		goto error_add;
	if(modman_inc_lfs(journal, lfs, "Journal") < 0)
		goto error_inc_1;
	if(modman_inc_lfs(fs, lfs, "Filesystem") < 0)
		goto error_inc_2;
	if(modman_inc_bd(fs_queue, lfs, "Queue") < 0)
		goto error_inc_3;

	return lfs;

  error_chdescs:
	free(state->commit_chdesc);
  error_state:
	free(state);
  error_lfs:
	free(lfs);
	return NULL;
	
  error_inc_3:
	modman_dec_lfs(fs, lfs);
  error_inc_2:
	modman_dec_lfs(journal, lfs);
  error_inc_1:
	modman_rem_lfs(lfs);
  error_add:
	DESTROY(lfs);
	return NULL;
}

size_t journal_lfs_max_bandwidth(const LFS_t * lfs)
{
	const journal_state_t * state = (const journal_state_t *) OBJLOCAL(lfs);
	if(OBJMAGIC(lfs) == JOURNAL_MAGIC)
	{
		size_t bytes_per_slot = TRANSACTION_SIZE - state->blocksize * (trans_number_block_count(state->blocksize) + 1);
		return (state->ncommit_records * (bytes_per_slot / 1024)) / TRANSACTION_PERIOD;
	}
	return -E_INVAL;
}
