#include <inc/hash_map.h>
#include <inc/vector.h>
#include <inc/malloc.h>
#include <inc/lib.h>

#include <kfs/cfs_ipc_serve.h>
#include <kfs/fidman.h>
#include <kfs/fidfairy_cfs.h>


#define FIDFAIRY_DEBUG 0

#if FIDFAIRY_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// The only fidfairy_cfs instance's CFS_t*
static CFS_t * cfs_fidfairy = NULL;


struct open_file {
	int fid;
	struct Fd * page;
};
typedef struct open_file open_file_t;

struct fidfairy_state {
	hash_map_t * open_files;
	CFS_t * frontend_cfs;
	uint32_t nfids_created;
	void * cur_page;
};
typedef struct fidfairy_state fidfairy_state_t;


static bool va_is_mapped(void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}


//
// open_file_t functions

static open_file_t * open_file_create(int fid, void * page)
{
	open_file_t * of = malloc(sizeof(*of));
	if (!of)
		return NULL;

	of->fid = fid;
	of->page = page;
	return of;
}

static void open_file_destroy(open_file_t * of)
{
	int r;
	if (of->page && va_is_mapped(of->page))
	{
		r = sys_page_unmap(0, (void *) of->page);
		if (r < 0)
			printf("%s: sys_page_unmap(0, 0x%08x): %e\n", __FUNCTION__, of->page, r);
		assert(0 <= r);
	}
	else
		assert(of->fid == -1);

	of->fid = -1;
	of->page = NULL;
	free(of);
}

static int open_file_close(fidfairy_state_t * state, open_file_t * of)
{
	open_file_t * of_erase;
	int r;

	// There's only work to do when the last reference to a file is closed:
	assert(of->page);
	assert(1 <= pageref(of->page));
	if (1 < pageref(of->page))
		return 0;

	r = CALL(state->frontend_cfs, close, of->fid);
	if (r < 0)
		return r;

	of_erase = hash_map_erase(state->open_files, (void*) of->fid);
	assert(of == of_erase);
	open_file_destroy(of);

	return 0;
}

static void open_file_gc(fidfairy_state_t * state)
{
	hash_map_it_t * hm_it;
	open_file_t * of;
	vector_t * ofs_to_erase;
	int r;

	ofs_to_erase = vector_create();
	hm_it = hash_map_it_create();
	if (!ofs_to_erase || !hm_it)
	{
		fprintf(STDERR_FILENO, "fidfairy unable to malloc memory to gc\n");
		if (ofs_to_erase)
			vector_destroy(ofs_to_erase);
		else if (hm_it)
			hash_map_it_destroy(hm_it);
		return;
	}

	// Gc fids
	// (remove the fids after this, else we would mess up hm_it)
	while ((of = hash_map_val_next(state->open_files, hm_it)))
	{
		assert(of->page && va_is_mapped(of->page));

		r = vector_push_back(ofs_to_erase, of);
		if (r < 0)
		{
			fprintf(STDERR_FILENO, "fidfairy gc: vector_push_back: %e\n", r);
			break;
		}
	}
	hash_map_it_destroy(hm_it);

	// Remove gced open files
	const size_t nofs_to_erase = vector_size(ofs_to_erase);
	size_t i;
	for (i=0; i < nofs_to_erase; i++)
	{
		r = open_file_close(state, vector_elt(ofs_to_erase, i));
		if (r < 0)
			fprintf(STDERR_FILENO, "fidfairy gc: open_file_close: %e\n", r);
	}
	vector_destroy(ofs_to_erase);
}


//
// fidman.h implementation

// NOTE: This create_fid() is limited to creating one fid per open request.
// Chris plans to remove this limitation, and decouple fidfairy_cfs and
// create_fid() and remove this restriction.
int create_fid(void)
{
	Dprintf("%s()\n", __FUNCTION__);
	fidfairy_state_t * state;
	void * cache;
	int fid;
	open_file_t * of;
	int r;

	if (!cfs_fidfairy)
		panic("fidman used before being created");
	state = (fidfairy_state_t *) cfs_fidfairy->instance;

	// find a free page
	for(cache = FIDFAIRY_CFS_FD_MAP; cache != FIDFAIRY_CFS_FD_END; cache += PGSIZE)
		if(!va_is_mapped(cache))
			break;
	if(cache == FIDFAIRY_CFS_FD_END)
		return -E_MAX_OPEN;

	// remap the client's page to its new home
	assert(state->cur_page && va_is_mapped(state->cur_page));
	r = sys_page_map(0, state->cur_page, 0, cache, PTE_U | PTE_P);
	if(r < 0)
		return r;
	r = sys_page_unmap(0, state->cur_page);
	assert(r >= 0);
	state->cur_page = NULL;
	
	// make up a new fid
	fid = state->nfids_created++;
	fid &= PGSIZE - 1;
	fid |= 0x7FFFFFFF & (int) cache;

	// save this open_file
	r = 0;
	of = open_file_create(fid, cache);
	if (!of || ((r = hash_map_insert(state->open_files, (void*) fid, of)) < 0))
	{
		int s = sys_page_unmap(0, cache);
		assert(0 <= s);
		if (r == 0)
			return -E_NO_MEM;
		else
			return r;
	}

	return fid;
}


//
// fidfairy_cfs implementation

// Intercepted CFS_t functions

static int fidfairy_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	int fid;
	open_file_t * of;

	open_file_gc(state);

	state->cur_page = cfs_ipc_serve_cur_page();
	assert(state->cur_page);

	fid = CALL(state->frontend_cfs, open, name, mode);
	if (fid < 0)
	{
		// Check that create_fid() was not called
		// (this code could be enhanced to clean up when this happens)
		assert(state->cur_page);

		sys_page_unmap(0, state->cur_page);
		state->cur_page = NULL;
		return fid;
	}

	// An approximate check that fid is the one created by create_fid():
	assert(!state->cur_page);
	of = hash_map_find_val(state->open_files, (void*) fid);
	assert(of);
	assert(of->page);
	assert(FIDFAIRY_CFS_FD_MAP <= (void*) of->page && (void*) of->page < FIDFAIRY_CFS_FD_END);
	assert(va_is_mapped(of->page));
	assert((fid & (PGSIZE-1)) == state->nfids_created - 1);

	return fid;
}

static int fidfairy_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	open_file_t * of;
	int r;

	of = hash_map_find_val(state->open_files, (void*) fid);
	if (!of)
		return -E_INVAL;
	r = open_file_close(state, of);
	if (r < 0)
		return r;
	return 0;
}

static int fidfairy_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;

	// TODO: open_file_gc() here? force close all files?

	state->frontend_cfs = NULL;
	state->nfids_created = 0;
	state->cur_page = NULL;

	hash_map_destroy(state->open_files);
	state->open_files = NULL;

	cfs_fidfairy = NULL;

	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

// Passthrough CFS_t functions

static int fidfairy_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, read, fid, data, offset, size);
}

static int fidfairy_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, write, fid, data, offset, size);
}

static int fidfairy_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, getdirentries, fid, buf, nbytes, basep);
}

static int fidfairy_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, truncate, fid, target_size);
}

static int fidfairy_unlink(CFS_t * cfs, const char * name)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, unlink, name);
}

static int fidfairy_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, link, oldname, newname);
}

static int fidfairy_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, rename, oldname, newname);
}

static int fidfairy_mkdir(CFS_t * cfs, const char * name)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, mkdir, name);
}

static int fidfairy_rmdir(CFS_t * cfs, const char * name)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, rmdir, name);
}

static size_t fidfairy_get_num_features(CFS_t * cfs, const char * name)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_num_features, name);
}

static const feature_t * fidfairy_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_feature, name, num);
}

static int fidfairy_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_metadata, name, id, size, data);
}

static int fidfairy_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, set_metadata, name, id, size, data);
}

static int fidfairy_sync(CFS_t * cfs, const char * name)
{
	fidfairy_state_t * state = (fidfairy_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, sync, name);
}


//
// CFS_t management

CFS_t * fidfairy_cfs(CFS_t * frontend_cfs)
{
	CFS_t * cfs;
	fidfairy_state_t * state;

	if (cfs_fidfairy)
		panic("fidfairy can currently have at most one instance");


	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	cfs_fidfairy = cfs;

	ASSIGN(cfs, fidfairy, open);
	ASSIGN(cfs, fidfairy, close);
	ASSIGN(cfs, fidfairy, read);
	ASSIGN(cfs, fidfairy, write);
	ASSIGN(cfs, fidfairy, getdirentries);
	ASSIGN(cfs, fidfairy, truncate);
	ASSIGN(cfs, fidfairy, unlink);
	ASSIGN(cfs, fidfairy, link);
	ASSIGN(cfs, fidfairy, rename);
	ASSIGN(cfs, fidfairy, mkdir);
	ASSIGN(cfs, fidfairy, rmdir);
	ASSIGN(cfs, fidfairy, get_num_features);
	ASSIGN(cfs, fidfairy, get_feature);
	ASSIGN(cfs, fidfairy, get_metadata);
	ASSIGN(cfs, fidfairy, set_metadata);
	ASSIGN(cfs, fidfairy, sync);
	ASSIGN_DESTROY(cfs, fidfairy, destroy);


	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;
	cfs->instance = state;
	state->frontend_cfs = frontend_cfs;
	state->nfids_created = 0;
	state->cur_page = NULL;
	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	return cfs;

  error_state:
	free(state);
	cfs->instance = NULL;
  error_cfs:
	free(cfs);
	cfs = NULL;
	cfs_fidfairy = NULL;
	return NULL;
}
