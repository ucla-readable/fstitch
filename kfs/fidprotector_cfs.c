#include <inc/hash_map.h>
#include <inc/malloc.h>

#include <kfs/cfs_ipc_serve.h>
#include <kfs/fidprotector_cfs.h>


#define FIDPROTECTOR_DEBUG 0

#if FIDPROTECTOR_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct open_file {
	int fid;
	uint32_t cappa;
};
typedef struct open_file open_file_t;

struct fidprotector_state {
	hash_map_t * open_files;
	CFS_t * frontend_cfs;
};
typedef struct fidprotector_state fidprotector_state_t;


//
// open_file_t functions

static open_file_t * open_file_create(int fid, uint32_t cappa)
{
	open_file_t * of = malloc(sizeof(*of));
	if (!of)
		return NULL;

	of->fid = fid;
	of->cappa = cappa;
	return of;
}

static void open_file_close(open_file_t * of)
{
	of->fid = -1;
	of->cappa = -1;
	free(of);
}


//
// Capability checking

// Check that the given open file matches the last received ipc capability,
// ensuring that no env's request is able to pass through unless they
// have the Fd page for the request fid.
static int check_capability(const open_file_t * of)
{
	if (cfs_ipc_serve_cur_cappa() != of->cappa && cfs_ipc_serve_cur_cappa())
	{
		fprintf(STDERR_FILENO, "fidprotector %s: FAILURE for fid %d. fid's cappa = 0x%08x, request's cappa = 0x%08x.\n", __FUNCTION__, of->fid, of->cappa, cfs_ipc_serve_cur_cappa());
		return -E_IPC_FAILED_CAP;
	}

	return 0;
}

// Convenience function for check_capability()
static int check_capability_fid(const fidprotector_state_t * state, int fid)
{
	const open_file_t * of = hash_map_find_val(state->open_files, (void*) fid);
	if (!of)
		return -E_INVAL;
	return check_capability(of);
}


//
// Intercepted (but not capability checked) CFS_t functions

static int fidprotector_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	int fid;
	uint32_t cappa;
	open_file_t * of;
	int r;

	fid = CALL(state->frontend_cfs, open, name, mode);
	if (fid < 0)
		return fid;

	cappa = cfs_ipc_serve_cur_cappa();
	if (cappa == -1)
		fprintf(STDERR_FILENO, "%s(\"%s\"): warning: capability is the unused-marker\n", __FUNCTION__, name);

	r = 0;
	of = open_file_create(fid, cappa);
	if (!of || ((r = hash_map_insert(state->open_files, (void*) fid, of)) < 0))
	{
		(void) CALL(state->frontend_cfs, close, fid);
		if (r == 0)
			return -E_NO_MEM;
		else
			return r;
	}

	return fid;
}

static int fidprotector_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;

	state->frontend_cfs = NULL;

	hash_map_destroy(state->open_files);
	state->open_files = NULL;

	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


//
// Capability checked CFS_t functions

static int fidprotector_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	open_file_t * of;
	int r;

	if (!(of = hash_map_find_val(state->open_files, (void*) fid)))
		return -E_INVAL;
	if ((r = check_capability(of)) < 0)
		return r;

	if ((r = CALL(state->frontend_cfs, close, fid)) < 0)
		return r;

	open_file_close(of);

	return 0;
}

static int fidprotector_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	int r = check_capability_fid(state, fid);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, read, fid, data, offset, size);
}

static int fidprotector_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	int r = check_capability_fid(state, fid);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, write, fid, data, offset, size);
}

static int fidprotector_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	int r = check_capability_fid(state, fid);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, getdirentries, fid, buf, nbytes, basep);
}

static int fidprotector_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	int r = check_capability_fid(state, fid);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, truncate, fid, target_size);
}


//
// Passthrough CFS_t functions

static int fidprotector_unlink(CFS_t * cfs, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, unlink, name);
}

static int fidprotector_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, link, oldname, newname);
}

static int fidprotector_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, rename, oldname, newname);
}

static int fidprotector_mkdir(CFS_t * cfs, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, mkdir, name);
}

static int fidprotector_rmdir(CFS_t * cfs, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, rmdir, name);
}

static size_t fidprotector_get_num_features(CFS_t * cfs, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_num_features, name);
}

static const feature_t * fidprotector_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_feature, name, num);
}

static int fidprotector_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, get_metadata, name, id, size, data);
}

static int fidprotector_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, set_metadata, name, id, size, data);
}

static int fidprotector_sync(CFS_t * cfs, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) cfs->instance;
	return CALL(state->frontend_cfs, sync, name);
}


//
// CFS_t management

CFS_t * fidprotector_cfs(CFS_t * frontend_cfs)
{
	CFS_t * cfs;
	fidprotector_state_t * state;

	if (!frontend_cfs)
		return NULL;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;

	ASSIGN(cfs, fidprotector, open);
	ASSIGN(cfs, fidprotector, close);
	ASSIGN(cfs, fidprotector, read);
	ASSIGN(cfs, fidprotector, write);
	ASSIGN(cfs, fidprotector, getdirentries);
	ASSIGN(cfs, fidprotector, truncate);
	ASSIGN(cfs, fidprotector, unlink);
	ASSIGN(cfs, fidprotector, link);
	ASSIGN(cfs, fidprotector, rename);
	ASSIGN(cfs, fidprotector, mkdir);
	ASSIGN(cfs, fidprotector, rmdir);
	ASSIGN(cfs, fidprotector, get_num_features);
	ASSIGN(cfs, fidprotector, get_feature);
	ASSIGN(cfs, fidprotector, get_metadata);
	ASSIGN(cfs, fidprotector, set_metadata);
	ASSIGN(cfs, fidprotector, sync);
	ASSIGN_DESTROY(cfs, fidprotector, destroy);


	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;
	cfs->instance = state;
	state->frontend_cfs = frontend_cfs;
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
	return NULL;
}
