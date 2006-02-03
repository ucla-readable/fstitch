#include <stdlib.h>
#include <inc/lib.h>
#include <lib/hash_map.h>
#include <lib/vector.h>

#include <kfs/modman.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/fidcloser_cfs.h>


#define FIDCLOSER_DEBUG 0

#if FIDCLOSER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// Because fidcloser_cfs decides when to close a fid based on the pageref
// number for the struct Fd page, fidcloser_cfs would never close any
// files in use by multiple fidclosers.
// Three possibilities to keep this from happening:
// 1- Assume this won't happen.
// 2- Figure out if a given fid/page is already in use by another fidcloser.
// 3- Allow at most one fidcloser to exist at a given time.
// Possibility 3 is safe (1 is not), simpler than 2, and at least for now
// mupltiple fidclosers aren't something we want. so possibility 2 it is:
static bool fidcloser_cfs_exists = 0;

struct open_file {
	int fid;
	const struct Fd * page;
};
typedef struct open_file open_file_t;

struct fidcloser_state {
	hash_map_t * open_files;
	CFS_t * frontend_cfs;
};
typedef struct fidcloser_state fidcloser_state_t;


static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}


//
// open_file_t functions

static open_file_t * open_file_create(int fid, const void * page)
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

static int open_file_close(fidcloser_state_t * state, open_file_t * of)
{
	open_file_t * of_erase;
	int r;

	// There's only work to do when the last reference to a file is closed:
	assert(of->page);
	assert(1 <= pageref(of->page));
	if (1 < pageref(of->page))
	{
		Dprintf("fidcloser_cfs %s: not closing fid %d, %d external refs\n", __FUNCTION__, of->fid, pageref(of->page)-1);
		return 0;
	}

	Dprintf("fidcloser_cfs %s: sending close for fid %d\n", __FUNCTION__, of->fid);
	r = CALL(state->frontend_cfs, close, of->fid);
	if (r < 0)
		return r;

	of_erase = hash_map_erase(state->open_files, (void*) of->fid);
	assert(of == of_erase);
	open_file_destroy(of);

	return 0;
}

static void open_file_gc(fidcloser_state_t * state)
{
	hash_map_it_t hm_it;
	open_file_t * of;
	vector_t * ofs_to_erase;
	int r;

	ofs_to_erase = vector_create();
	hash_map_it_init(&hm_it, state->open_files);
	if (!ofs_to_erase)
	{
		kdprintf(STDERR_FILENO, "fidcloser unable to malloc memory to gc\n");
		return;
	}

	// Gc fids
	// (remove the fids after this, else we would mess up hm_it)

	while ((of = hash_map_val_next(&hm_it)))
	{
		assert(of->page && va_is_mapped(of->page));

		r = vector_push_back(ofs_to_erase, of);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "fidcloser gc: vector_push_back: %e\n", r);
			break;
		}
	}


	// Remove gced open files
	// (set the current cappa to indicate kfsd is closing internally,
	//  rather than using whatever happens to be the current cappa)

	const uint32_t cur_cappa = cfs_ipc_serve_cur_cappa();
	cfs_ipc_serve_set_cur_cappa(0);

	const size_t nofs_to_erase = vector_size(ofs_to_erase);
	size_t i;
	for (i=0; i < nofs_to_erase; i++)
	{
		r = open_file_close(state, vector_elt(ofs_to_erase, i));
		if (r < 0)
			kdprintf(STDERR_FILENO, "fidcloser gc: open_file_close: %e\n", r);
	}

	cfs_ipc_serve_set_cur_cappa(cur_cappa);
	vector_destroy(ofs_to_erase);
}


//
// Intercepted CFS_t functions

static int fidcloser_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FIDCLOSER_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int fidcloser_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FIDCLOSER_MAGIC)
		return -E_INVAL;
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	
	snprintf(string, length, "fids: %u", hash_map_size(state->open_files));
	return 0;
}

static int fidcloser_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	int fid;
	const void * page;
	const void * cache;
	open_file_t * of;
	int r;

	open_file_gc(state);

	page = cfs_ipc_serve_cur_page();
	assert(page && va_is_mapped(page));


	fid = CALL(state->frontend_cfs, open, name, mode);
	if (fid < 0)
		return fid;


	// find a free slot to cache page
	for(cache = FIDCLOSER_CFS_FD_MAP; cache != FIDCLOSER_CFS_FD_END; cache += PGSIZE)
		if(!va_is_mapped(cache))
			break;
	if(cache == FIDCLOSER_CFS_FD_END)
	{
		(void) CALL(state->frontend_cfs, close, fid);
		return -E_MAX_OPEN;
	}

	// remap the client's page to fidcloser's cache
	r = sys_page_map(0, (void*) page, 0, (void*) cache, PTE_U | PTE_P);
	if(r < 0)
	{
		(void) CALL(state->frontend_cfs, close, fid);
		return r;
	}

	// save this open_file
	r = 0;
	of = open_file_create(fid, cache);
	if (!of || ((r = hash_map_insert(state->open_files, (void*) fid, of)) < 0))
	{
		(void) CALL(state->frontend_cfs, close, fid);
		int s = sys_page_unmap(0, (void*) cache);
		assert(0 <= s);
		if (r == 0)
			return -E_NO_MEM;
		else
			return r;
	}

	return fid;
}

static int fidcloser_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
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

static int fidcloser_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_cfs(state->frontend_cfs, cfs);

	// TODO: open_file_gc() here? force close all files?

	state->frontend_cfs = NULL;

	hash_map_destroy(state->open_files);
	state->open_files = NULL;

	fidcloser_cfs_exists = 0;

	free(OBJLOCAL(cfs));
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


//
// Passthrough CFS_t functions

static int fidcloser_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, read, fid, data, offset, size);
}

static int fidcloser_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, write, fid, data, offset, size);
}

static int fidcloser_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, getdirentries, fid, buf, nbytes, basep);
}

static int fidcloser_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, truncate, fid, target_size);
}

static int fidcloser_unlink(CFS_t * cfs, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, unlink, name);
}

static int fidcloser_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, link, oldname, newname);
}

static int fidcloser_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rename, oldname, newname);
}

static int fidcloser_mkdir(CFS_t * cfs, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, mkdir, name);
}

static int fidcloser_rmdir(CFS_t * cfs, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rmdir, name);
}

static size_t fidcloser_get_num_features(CFS_t * cfs, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_num_features, name);
}

static const feature_t * fidcloser_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_feature, name, num);
}

static int fidcloser_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_metadata, name, id, size, data);
}

static int fidcloser_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, set_metadata, name, id, size, data);
}


//
// CFS_t management

CFS_t * fidcloser_cfs(CFS_t * frontend_cfs)
{
	CFS_t * cfs;
	fidcloser_state_t * state;

	if (fidcloser_cfs_exists)
		panic("fidcloser can currently have at most one instance.");

	if (!frontend_cfs)
		return NULL;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;

	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;

	CFS_INIT(cfs, fidcloser, state);
	OBJMAGIC(cfs) = FIDCLOSER_MAGIC;

	state->frontend_cfs = frontend_cfs;
	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	if(modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}
	if(modman_inc_cfs(frontend_cfs, cfs, NULL) < 0)
	{
		modman_rem_cfs(cfs);
		DESTROY(cfs);
		return NULL;
	}

	fidcloser_cfs_exists = 1;

	return cfs;

  error_state:
	free(state);
	OBJLOCAL(cfs) = NULL;
  error_cfs:
	free(cfs);
	cfs = NULL;
	return NULL;
}
