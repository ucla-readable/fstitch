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
};
typedef struct journal_state journal_state_t;


//
// Journaling

#define TRANSACTION_SIZE (1024*1024)
static const char journal_filename[] = "/.journal";

struct commit_record {
	enum {CREMPTY, CRSUBCOMMIT, CRCOMMIT} type;
	size_t nblocks;
	bool commit;
	bool invalid;
	// struct commit_record * next; // TODO: not yet supported
};
typedef struct commit_record commit_record_t;


// TODO
static int ensure_journal_exists(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	state->jfdesc = CALL(state->journal, lookup_name, journal_filename);
	if (!state->jfdesc)
	{
		// TODO: Attempt to create journal_filename?
		return -E_NOT_FOUND;
	}

	return 0;
}

// TODO
static int replay_journal(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	return 0;
}

static int transaction_start(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	return journal_queue_hold(state->queue);
}

static int transaction_stop(journal_state_t * state)
{
	Dprintf("%s()\n", __FUNCTION__);
	size_t file_offset;
	const size_t journal_blocksize = CALL(state->journal, get_blocksize);
	bdesc_t ** data_bdescs;
	size_t ndatabdescs;
	bdesc_t * bdesc;
	size_t i;
	int r;


	//
	// Choose transaction slot.

	{
		size_t transaction_slot;

		// For now, always use transaction_slot 0
		transaction_slot = 0;
		file_offset = transaction_slot * TRANSACTION_SIZE;

		// TODO: Make this transaction's commit record dependent on
		// the non-synced transaction's invalid chdesc.
		// For now, sync this transaction slot.
		//
		// TODO: LFS::sync only allows syncing a file, not a range in the file.
		// For now, sync the entire file.

		// FIXME: this returns -E_INVAL, why?
		/*
		r = CALL(state->journal, sync, journal_filename);
		if (r < 0)
			return r;
		*/
	}


	//
	// Sort the data_bdescs, allowing for faster disk access.
	// TODO: it'd be nice if this also sorted journal_queue's copy.

	{
		const hash_map_t * data_bdescs_map; // blockno -> bdesc_t *
		size_t transaction_size;
		hash_map_it_t * it;

		data_bdescs_map = journal_queue_blocklist(state->queue);
		if (!data_bdescs_map)
			return 0; // nothing to journal

		ndatabdescs = hash_map_size(data_bdescs_map);

		transaction_size = (1 + ndatabdescs) * journal_blocksize;
		if (transaction_size > TRANSACTION_SIZE)
		{
			// TODO: break up into multiple transactions.
			panic("Transaction breakup not yet implemented, transaction size = %d, max transaction size = %d", transaction_size, TRANSACTION_SIZE);
		}

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
	// Create and write journal transaction entries.
	//
	// Dependencies:
	// - journal data -> commit record
	// - commit -> journal data
	// - fs data -> commit
	// - invalid -> fs data

	BD_t * journal_bd;
	// jdata_chdescs will depend on all journal data chdescs
	chdesc_t * jdata_chdescs;
	// fsdata_chdescs will depend on all fs data chdescs
	chdesc_t * fsdata_chdescs;
	commit_record_t commit;
	size_t commit_offset;
	// lfs_head/tail used to check that no metadata changes upon journal writes
	chdesc_t * lfs_head = NULL, * lfs_tail;
	chdesc_t * prev_head, * tail;

	journal_bd = CALL(state->journal, get_blockdev);
	assert(journal_bd);

	assert(sizeof(commit) <= CALL(journal_bd, get_atomicsize));

	jdata_chdescs = chdesc_create_noop(NULL);
	if (!jdata_chdescs)
	{
		free(data_bdescs);
		return -E_NO_MEM;
	}

	fsdata_chdescs = chdesc_create_noop(NULL); 
	if (!fsdata_chdescs)
	{
		free(jdata_chdescs);
		free(data_bdescs);
		return -E_NO_MEM;
	}

	r = depman_add_chdesc(jdata_chdescs);
	if (r < 0)
	{
		free(fsdata_chdescs);
		free(jdata_chdescs);
		free(data_bdescs);
		return r;
	}

	r = depman_add_chdesc(fsdata_chdescs);
	if (r < 0)
	{
		// Ignore possible depman error
		(void) depman_remove_chdesc(jdata_chdescs);
		free(fsdata_chdescs);
		free(jdata_chdescs);
		free(data_bdescs);
		return r;
	}

	r = journal_queue_passthrough(state->queue);
	assert(r >= 0);


	// Create commit record

	commit.type = CRCOMMIT; // TODO: support sub-commit
	commit.nblocks = ROUND32(ndatabdescs, journal_blocksize)/journal_blocksize;
	commit.commit = 0;
	commit.invalid = 0;
	// commit.next = NULL; // TODO: not yet supported

	commit_offset = file_offset;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, file_offset);
	assert(bdesc); // TODO: handle error

	r = chdesc_create_byte(bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	r = depman_add_chdesc(prev_head);
	assert(r >= 0); // TODO: handle error

	r = CALL(state->journal, write_block, bdesc, 0, sizeof(commit), &commit, &lfs_head, &lfs_tail);
	assert(r >= 0); // TODO: handle error
	assert(!lfs_head && !lfs_tail);

	file_offset += journal_blocksize;


	// Create journal data

	chdesc_t * commit_rec_chdesc = prev_head;

	for (i=0; i < ndatabdescs; i++, file_offset += journal_blocksize)
	{
		bdesc = CALL(state->journal, get_file_block, state->jfdesc, file_offset);
		assert(bdesc); // TODO: handle error

		prev_head = commit_rec_chdesc;
		r = chdesc_create_full(bdesc, data_bdescs[i]->ddesc->data, &prev_head, &tail);
		assert(r >= 0); // TODO: handle error
		r = depman_add_chdesc(prev_head);
		assert(r >= 0); // TODO: handle error
		r = chdesc_add_depend(jdata_chdescs, prev_head);
		assert(r >= 0); // TODO: handle error

		r = CALL(state->journal, write_block, bdesc, data_bdescs[i]->offset, data_bdescs[i]->length, data_bdescs[i]->ddesc->data, &lfs_head, &lfs_tail);
		assert(r >= 0); // TODO: handle error
		assert(!lfs_head && !lfs_tail);

	}


	// Mark as commit

	commit.commit = 1;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, commit_offset);
	assert(bdesc); // TODO: handle error

	prev_head = jdata_chdescs;
	r = chdesc_create_byte(bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	r = depman_add_chdesc(prev_head);
	assert(r >= 0); // TODO: handle error

	r = CALL(state->journal, write_block, bdesc, 0, sizeof(commit), &commit, &lfs_head, &lfs_tail);
	assert(r >= 0); // TODO: handle error
	assert(!lfs_head && !lfs_tail);


	// Mark as invalid

	chdesc_t * commit_chdesc = prev_head;

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

	commit.invalid = 1;

	bdesc = CALL(state->journal, get_file_block, state->jfdesc, commit_offset);
	assert(bdesc); // TODO: handle error

	// TODO: invalid should depend on fs data
	r = chdesc_create_byte(bdesc, 0, sizeof(commit), &commit, &prev_head, &tail);
	assert(r >= 0); // TODO: handle error
	r = depman_add_chdesc(prev_head);
	assert(r >= 0); // TODO: handle error

	r = CALL(state->journal, write_block, bdesc, 0, sizeof(commit), &commit, &lfs_head, &lfs_tail);
	assert(r >= 0); // TODO: handle error
	assert(!lfs_head && !lfs_tail);


	//
	// Release the data bdescs.

	r = journal_queue_release(state->queue);
	if (r < 0)
		return r;


	free(data_bdescs);

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

static int journal_write_block(LFS_t * lfs, bdesc_t * block, uint32_t offset, uint32_t size, void * data, chdesc_t ** head, chdesc_t ** tail)
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
	return CALL(state->fs, get_blocksize);
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

	r = ensure_journal_exists(state);
	if (r < 0)
		goto error_state;

	r = replay_journal(state);
	if (r < 0)
		goto error_state;

	r = transaction_start(state);
	if (r < 0)
		goto error_state;

	r = sched_register(timer_callback, state, 5*100);
	if (r < 0)
		goto error_state;

	return lfs;

  error_state:
	free(state);
  error_lfs:
	free(lfs);
	return NULL;
}
