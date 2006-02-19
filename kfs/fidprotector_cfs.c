#include <string.h>
#include <stdlib.h>
#include <inc/error.h>

#include <kfs/modman.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/fidprotector_cfs.h>


#define FIDPROTECTOR_DEBUG 0

#if FIDPROTECTOR_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct fidprotector_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	uint32_t cappa;
};
typedef struct fidprotector_fdesc fidprotector_fdesc_t;

struct fidprotector_state {
	CFS_t * frontend_cfs;
	uint32_t nopen;
};
typedef struct fidprotector_state fidprotector_state_t;


//
// fidprotector_fdesc_t functions

static fidprotector_fdesc_t * fidprotector_fdesc_create(fdesc_t * inner, uint32_t cappa)
{
	fidprotector_fdesc_t * fpf = malloc(sizeof(*fpf));
	if (!fpf)
		return NULL;

	fpf->common = inner->common;
	fpf->inner = inner;
	fpf->cappa = cappa;
	return fpf;
}

static void fidprotector_fdesc_close(fidprotector_state_t * state, fidprotector_fdesc_t * fpf)
{
	state->nopen--;
	fpf->common = NULL;
	fpf->inner = NULL;
	fpf->cappa = -1;
	free(fpf);
}


//
// Capability checking

// Check that the given open file matches the last received ipc capability,
// ensuring that no env's request is able to pass through unless they
// have the Fd page for the request fid.
static int check_capability(const fidprotector_fdesc_t * fpf)
{
	assert(fpf);
	if (cfs_ipc_serve_cur_cappa() != fpf->cappa && cfs_ipc_serve_cur_cappa())
	{
		kdprintf(STDERR_FILENO, "fidprotector %s: FAILURE: cappa = 0x%08x, request's cappa = 0x%08x.\n", __FUNCTION__, fpf->cappa, cfs_ipc_serve_cur_cappa());
		return -E_IPC_FAILED_CAP;
	}

	return 0;
}


//
// Intercepted (but not capability checked) CFS_t functions

static int fidprotector_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FIDPROTECTOR_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int fidprotector_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FIDPROTECTOR_MAGIC)
		return -E_INVAL;
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	
	snprintf(string, length, "open files: %u", state->nopen);
	return 0;
}

static int open_fdesc(fidprotector_state_t * state, fdesc_t * inner, fdesc_t ** outer)
{
	uint32_t cappa;
	fidprotector_fdesc_t * fpf;

	cappa = cfs_ipc_serve_cur_cappa();
	if (cappa == -1)
		kdprintf(STDERR_FILENO, "%s(): warning: capability is the unused-marker\n", __FUNCTION__);

	fpf = fidprotector_fdesc_create(inner, cappa);
	if (!fpf)
	{
		(void) CALL(state->frontend_cfs, close, inner);
		*outer = NULL;
		return -E_NO_MEM;
	}

	state->nopen++;
	*outer = (fdesc_t *) fpf;
	return 0;
}

static int fidprotector_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	r = CALL(state->frontend_cfs, open, ino, mode, &inner);
	if (r < 0)
		return r;

	return open_fdesc(state, inner, fdesc);
}

static int fidprotector_create(CFS_t * cfs, inode_t parent, const char * name, int mode, fdesc_t ** fdesc, inode_t * newino)
{
	Dprintf("%s(%u, \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	r = CALL(state->frontend_cfs, create, parent, name, mode, &inner, newino);
	if (r < 0)
		return r;

	r = open_fdesc(state, inner, fdesc);
	if (r < 0)
		*newino = INODE_NONE;
	return r;
}

static int fidprotector_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_cfs(state->frontend_cfs, cfs);

	state->frontend_cfs = NULL;

	free(OBJLOCAL(cfs));
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


//
// Capability checked CFS_t functions

static int fidprotector_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, fdesc);
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fidprotector_fdesc_t * fpf = (fidprotector_fdesc_t *) fdesc;
	int r;

	if ((r = check_capability(fpf) < 0))
		return r;

	if ((r = CALL(state->frontend_cfs, close, fpf->inner)) < 0)
		return r;

	fidprotector_fdesc_close(state, fpf);

	return 0;
}

static int fidprotector_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fidprotector_fdesc_t * fpf = (fidprotector_fdesc_t *) fdesc;
	assert(fpf);
	int r = check_capability(fpf);
	if (r < 0)
		return r;
	assert(fpf->inner);
	return CALL(state->frontend_cfs, read, fpf->inner, data, offset, size);
}

static int fidprotector_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fidprotector_fdesc_t * fpf = (fidprotector_fdesc_t *) fdesc;
	int r = check_capability(fpf);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, write, fpf->inner, data, offset, size);
}

static int fidprotector_getdirentries(CFS_t * cfs, fdesc_t * fdesc, char * buf, int nbytes, uint32_t * basep)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fidprotector_fdesc_t * fpf = (fidprotector_fdesc_t *) fdesc;
	int r = check_capability(fpf);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, getdirentries, fpf->inner, buf, nbytes, basep);
}

static int fidprotector_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t target_size)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	fidprotector_fdesc_t * fpf = (fidprotector_fdesc_t *) fdesc;
	int r = check_capability(fpf);
	if (r < 0)
		return r;
	return CALL(state->frontend_cfs, truncate, fpf->inner, target_size);
}


//
// Passthrough CFS_t functions

static int fidprotector_get_root(CFS_t * cfs, inode_t * ino)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_root, ino);
}

static int fidprotector_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, lookup, parent, name, ino);
}

static int fidprotector_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, unlink, parent, name);
}

static int fidprotector_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, link, ino, newparent, newname);
}

static int fidprotector_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rename, oldparent, oldname, newparent, newname);
}

static int fidprotector_mkdir(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, mkdir, parent, name, ino);
}

static int fidprotector_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rmdir, parent, name);
}

static size_t fidprotector_get_num_features(CFS_t * cfs, inode_t ino)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_num_features, ino);
}

static const feature_t * fidprotector_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_feature, ino, num);
}

static int fidprotector_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_metadata, ino, id, size, data);
}

static int fidprotector_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	fidprotector_state_t * state = (fidprotector_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, set_metadata, ino, id, size, data);
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

	state = malloc(sizeof(*state));
	if (!state)
	{
		free(cfs);
		return NULL;
	}

	CFS_INIT(cfs, fidprotector, state);
	OBJMAGIC(cfs) = FIDPROTECTOR_MAGIC;

	state->frontend_cfs = frontend_cfs;
	state->nopen = 0;

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

	return cfs;
}
