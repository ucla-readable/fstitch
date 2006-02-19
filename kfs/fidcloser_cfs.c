#include <stdlib.h>
#include <inc/lib.h>
#include <lib/hash_set.h>
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
// mupltiple fidclosers aren't something we want. so possibility 3 it is:
static bool fidcloser_cfs_exists = 0;

struct fidcloser_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	const struct Fd * page;
};
typedef struct fidcloser_fdesc fidcloser_fdesc_t;

struct fidcloser_state {
	hash_set_t * open_fdescs;
	CFS_t * frontend_cfs;
};
typedef struct fidcloser_state fidcloser_state_t;

static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}


//
// fidcloser_fdesc_t functions

static fidcloser_fdesc_t * fidcloser_fdesc_create(fdesc_t * inner, const void * page)
{
	fidcloser_fdesc_t * ff = malloc(sizeof(*ff));
	if (!ff)
		return NULL;

	ff->common = inner->common;
	ff->inner = inner;
	ff->page = page;
	return ff;
}

static void fidcloser_fdesc_destroy(fidcloser_fdesc_t * ff)
{
	int r;
	if (ff->page && va_is_mapped(ff->page))
	{
		r = sys_page_unmap(0, (void *) ff->page);
		if (r < 0)
			printf("%s: sys_page_unmap(0, 0x%08x): %i\n", __FUNCTION__, ff->page, r);
		assert(0 <= r);
	}
	else
		assert(ff->inner == NULL);

	ff->common = NULL;
	ff->inner = NULL;
	ff->page = NULL;
	free(ff);
}

static int fidcloser_fdesc_close(fidcloser_state_t * state, fidcloser_fdesc_t * ff)
{
	fidcloser_fdesc_t * ff_erase;
	int r;

	// There's only work to do when the last reference to a file is closed:
	assert(ff->page);
	assert(1 <= pageref(ff->page));
	if (1 < pageref(ff->page))
	{
		Dprintf("fidcloser_cfs %s: not closing, %d external refs\n", __FUNCTION__, pageref(ff->page)-1);
		return 0;
	}

	r = CALL(state->frontend_cfs, close, ff->inner);
	if (r < 0)
		return r;

	ff_erase = hash_set_erase(state->open_fdescs, (void*) ff);
	assert(ff == ff_erase);
	fidcloser_fdesc_destroy(ff);

	return 0;
}

static void fdesc_gc(fidcloser_state_t * state)
{
	hash_set_it_t hs_it;
	fidcloser_fdesc_t * ff;
	vector_t * ffs_to_erase;
	int r;

	ffs_to_erase = vector_create();
	hash_set_it_init(&hs_it, state->open_fdescs);
	if (!ffs_to_erase)
	{
		kdprintf(STDERR_FILENO, "fidcloser unable to malloc memory to gc\n");
		return;
	}

	// Gc fdescs
	// (remove the fdescs after this, else we would mess up hs_it)

	while ((ff = hash_set_next(&hs_it)))
	{
		assert(ff->page && va_is_mapped(ff->page));

		r = vector_push_back(ffs_to_erase, ff);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "fidcloser gc: vector_push_back: %i\n", r);
			break;
		}
	}


	// Remove gced open files
	// (set the current cappa to indicate kfsd is closing internally,
	//  rather than using whatever happens to be the current cappa)

	const uint32_t cur_cappa = cfs_ipc_serve_cur_cappa();
	cfs_ipc_serve_set_cur_cappa(0);

	const size_t nffs_to_erase = vector_size(ffs_to_erase);
	size_t i;
	for (i=0; i < nffs_to_erase; i++)
	{
		r = fidcloser_fdesc_close(state, vector_elt(ffs_to_erase, i));
		if (r < 0)
			kdprintf(STDERR_FILENO, "fidcloser gc: open_file_close: %i\n", r);
	}

	cfs_ipc_serve_set_cur_cappa(cur_cappa);
	vector_destroy(ffs_to_erase);
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
	
	snprintf(string, length, "open fdescs: %u", hash_set_size(state->open_fdescs));
	return 0;
}

static int open_fdesc(fidcloser_state_t * state, fdesc_t * inner, fdesc_t ** outer)
{
	const void * page;
	const void * cache;
	fidcloser_fdesc_t * ff;
	int r;

	page = cfs_ipc_serve_cur_page();
	assert(page && va_is_mapped(page));

	// find a free slot to cache page
	for(cache = FIDCLOSER_CFS_FD_MAP; cache != FIDCLOSER_CFS_FD_END; cache += PGSIZE)
		if(!va_is_mapped(cache))
			break;
	if(cache == FIDCLOSER_CFS_FD_END)
	{
		(void) CALL(state->frontend_cfs, close, inner);
		return -E_MAX_OPEN;
	}

	// remap the client's page to fidcloser's cache
	r = sys_page_map(0, (void*) page, 0, (void*) cache, PTE_U | PTE_P);
	if(r < 0)
	{
		(void) CALL(state->frontend_cfs, close, inner);
		return r;
	}

	// save this open_file
	r = 0;
	ff = fidcloser_fdesc_create(inner, cache);
	if (!ff || ((r = hash_set_insert(state->open_fdescs, (void*) ff)) < 0))
	{
		(void) CALL(state->frontend_cfs, close, inner);
		int s = sys_page_unmap(0, (void*) cache);
		assert(0 <= s);
		*outer = NULL;
		if (r == 0)
			return -E_NO_MEM;
		else
			return r;
	}

	*outer = (fdesc_t *) ff;
	return 0;
}

static int fidcloser_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	fdesc_gc(state);

	r = CALL(state->frontend_cfs, open, ino, mode, &inner);
	if (r < 0)
		return r;

	return open_fdesc(state, inner, fdesc);
}

static int fidcloser_create(CFS_t * cfs, inode_t parent, const char * name, int mode, fdesc_t ** fdesc, inode_t * newino)
{
	Dprintf("%s(parent = %u, name = \"%s\", mode = %d)\n", __FUNCTION__, parent, name, mode);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	fdesc_gc(state);

	r = CALL(state->frontend_cfs, create, parent, name, mode, &inner, newino);
	if (r < 0)
		return r;

	r = open_fdesc(state, inner, fdesc);
	if (r < 0)
		*newino = INODE_NONE;
	return r;
}

static int fidcloser_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fidcloser_fdesc_t * ff = (fidcloser_fdesc_t *) fdesc;
	int r;

	r = fidcloser_fdesc_close(state, ff);
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

	hash_set_destroy(state->open_fdescs);
	state->open_fdescs = NULL;

	fidcloser_cfs_exists = 0;

	free(OBJLOCAL(cfs));
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


//
// Passthrough CFS_t functions

static int fidcloser_get_root(CFS_t * cfs, inode_t * ino)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_root, ino);
}

static int fidcloser_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, lookup, parent, name, ino);
}

static int fidcloser_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fidcloser_fdesc_t * ff = (fidcloser_fdesc_t *) fdesc;
	return CALL(state->frontend_cfs, read, ff->inner, data, offset, size);
}

static int fidcloser_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fidcloser_fdesc_t * ff = (fidcloser_fdesc_t *) fdesc;
	return CALL(state->frontend_cfs, write, ff->inner, data, offset, size);
}

static int fidcloser_getdirentries(CFS_t * cfs, fdesc_t * fdesc, char * buf, int nbytes, uint32_t * basep)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fidcloser_fdesc_t * ff = (fidcloser_fdesc_t *) fdesc;
	return CALL(state->frontend_cfs, getdirentries, ff->inner, buf, nbytes, basep);
}

static int fidcloser_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t target_size)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	fidcloser_fdesc_t * ff = (fidcloser_fdesc_t *) fdesc;
	return CALL(state->frontend_cfs, truncate, ff->inner, target_size);
}

static int fidcloser_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, unlink, parent, name);
}

static int fidcloser_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, link, ino, newparent, newname);
}

static int fidcloser_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rename, oldparent, oldname, newparent, newname);
}

static int fidcloser_mkdir(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, mkdir, parent, name, ino);
}

static int fidcloser_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, rmdir, parent, name);
}

static size_t fidcloser_get_num_features(CFS_t * cfs, inode_t ino)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_num_features, ino);
}

static const feature_t * fidcloser_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_feature, ino, num);
}

static int fidcloser_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_metadata, ino, id, size, data);
}

static int fidcloser_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	fidcloser_state_t * state = (fidcloser_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, set_metadata, ino, id, size, data);
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
	state->open_fdescs = hash_set_create();
	if (!state->open_fdescs)
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
