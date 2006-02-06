#include <assert.h>
#include <inc/lib.h>
#include <kfs/modman.h>
#include <kfs/opgroup.h>
#include <kfs/cfs_ipc_opgroup.h>

/* just the cfs_ipc_opgroup_scope_* functions */
#define CFS_IPC_OPGROUP_SCOPE_DEBUG 0
/* just the cfs_ipc_opgroup_* functions (does not include scope functions) */
#define CFS_IPC_OPGROUP_DEBUG 1

#if CFS_IPC_OPGROUP_SCOPE_DEBUG
#define DSprintf(x...) printf(x)
#else
#define DSprintf(x...)
#endif

#if CFS_IPC_OPGROUP_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


//
// cfs_ipc_opgroup_scopes


typedef struct scope_entry {
	opgroup_scope_t * scope;
	uintptr_t client_scope_va;
} scope_entry_t;

static scope_entry_t env_scopes[NENV];


static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

static const void * env_scope_cappg(envid_t envid)
{
	return CFS_IPC_OPGROUP_SCOPE_CAPPGS + ENVX(envid)*PGSIZE;
}

static bool env_scope_cappg_is_dead(envid_t envid)
{
	int refcount;

	if (!va_is_mapped(env_scope_cappg(envid)))
		return 0;

	switch ((refcount = pageref(env_scope_cappg(envid))))
	{
		case 2: return 0;
		case 1: return 1;
		case 0:
			assert(0); // can't happen
		default:
			panic("%s: opgroup scope page not designed to have >2 (%d) refs\n", __FUNCTION__, refcount);
	}
}

static void env_scope_init(envid_t envid, opgroup_scope_t * scope, uintptr_t client_scope_va)
{
	env_scopes[ENVX(envid)].scope = scope;
	env_scopes[ENVX(envid)].client_scope_va = client_scope_va;
}

// An env scope is valid if:
// (1) the env has an scope entry and the entry is alive and is mapped
//     into the client at the correct address
// (2) the env has no scope entry
static bool env_scope_is_valid(envid_t envid)
{
	int r;

	if (env_scope_cappg_is_dead(envid))
		return 0;
	if (!va_is_mapped(env_scope_cappg(envid)))
		return 1;

	r = sys_page_is_mapped((void *) env_scope_cappg(envid), envid, env_scopes[ENVX(envid)].client_scope_va);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "%s(%08x): sys_page_is_mapped(): %e\n", __FUNCTION__, envid, r);
		sys_print_backtrace();
	}
	return r == 1;
}

static bool env_scope_exists(envid_t envid)
{
	return env_scope_is_valid(envid) && va_is_mapped(env_scope_cappg(envid));
}

static opgroup_scope_t * env_scope(envid_t envid)
{
	if (!env_scope_exists(envid))
		return NULL;
	return env_scopes[ENVX(envid)].scope;
}

static int env_scope_destroy(envid_t envid)
{
	int r;

	if (pageref(env_scope_cappg(envid)) > 1)
		kdprintf(STDERR_FILENO, "%s(%08x): env scope's cappg still has references, destroying anyway\n", __FUNCTION__, envid);

	if ((r = sys_page_unmap(0, (void *) env_scope_cappg(envid))) < 0)
		return r;
	if (env_scopes[ENVX(envid)].scope)
		opgroup_scope_destroy(env_scopes[ENVX(envid)].scope);
	env_scope_init(envid, NULL, 0);
	return 0;
}


int set_cur_opgroup_scope(envid_t envid)
{
	if (!env_scope_is_valid(envid))
		return -E_BAD_ENV; // error for now, but we might want to init
	opgroup_scope_set_current(env_scope(envid));
	return 0;
}

void clear_cur_opgroup_scope(void)
{
	opgroup_scope_set_current(NULL);
}


int cfs_ipc_opgroup_scope_create(envid_t envid, const void * scope_cappg, uintptr_t envid_scope_cappg_va)
{
	opgroup_scope_t * env_scope;
	int r;
	DSprintf("%s(envid %08x, cap 0x%08x, cap_va 0x%08x)\n", __FUNCTION__, envid, scope_cappg, envid_scope_cappg_va);

	if (!envid_scope_cappg_va)
		return -E_INVAL;

	if (sys_page_is_mapped((void *) scope_cappg, envid, envid_scope_cappg_va) < 1)
		return -E_INVAL;

	if (env_scope_exists(envid))
		return -E_UNSPECIFIED; // disallow re-creation

	if (env_scope_cappg_is_dead(envid))
	{
		r = env_scope_destroy(envid);
		assert(r >= 0);
	}

	if ((r = sys_page_map(0, (void *) scope_cappg, 0, (void *) env_scope_cappg(envid), PTE_U|PTE_P)) < 0)
		return r;

	if (!(env_scope = opgroup_scope_create()))
	{
		(void) sys_page_unmap(0, (void *) env_scope_cappg(envid));
		return -E_NO_MEM;
	}

	env_scope_init(envid, env_scope, envid_scope_cappg_va);

	return 0;
}


int cfs_ipc_opgroup_scope_copy(envid_t parent, envid_t child, const void * child_scope_cappg, uintptr_t child_scope_cappg_va)
{
	opgroup_scope_t * child_env_scope;
	int r;
	DSprintf("scope_copy(parent %08x, child %08x, cap 0x%08x, childva 0x%08x)\n", parent, child, child_scope_cappg, child_scope_cappg_va);

	if (!child_scope_cappg)
		return -E_INVAL;
	if (!env_scope_exists(parent))
		return -E_BAD_ENV;
	if (env_scope_exists(child))
		return -E_INVAL; // disallow re-creation
	if (sys_page_is_mapped((void *) child_scope_cappg, child, child_scope_cappg_va) < 1)
		return -E_INVAL;

	if (env_scope_cappg_is_dead(child))
	{
		r = env_scope_destroy(child);
		assert(r >= 0);
	}

	if ((r = sys_page_map(0, (void *) child_scope_cappg, 0, (void *) env_scope_cappg(child), PTE_U|PTE_P)) < 0)
		return r;

	if (!(child_env_scope = opgroup_scope_copy(env_scope(parent))))
	{
		(void) sys_page_unmap(0, (void *) env_scope_cappg(child));
		return -E_NO_MEM;
	}

	env_scope_init(child, child_env_scope, child_scope_cappg_va);

	return 0;
}



//
// opgroupscope_tracker_cfs


// Because opgroupscope_tracker decides when to deactivate an environment's
// opgroup scope based on the pageref number ofr the env opgroup scope cappa,
// opgroupscope_tracker would never deactivate any scopes in use by
// multiple opgroupscope_trackers.
// Just as fidcloser_cfs.c does, we thus allow at most one
// opgroupscope_tracker to exist at a given time.
static bool opgroupscope_tracker_cfs_exists = 0;


typedef struct opgroupscope_tracker_state {
	CFS_t * frontend_cfs;
} opgroupscope_tracker_state_t;

static opgroupscope_tracker_state_t this_state;
static CFS_t this_cfs;


static void open_opgroup_scope_gc(void)
{
	DSprintf("%s()\n", __FUNCTION__);
	panic("TODO");
}


// Intercepted CFS_t functions

static int opgroupscope_tracker_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != OPGROUPSCOPE_TRACKER_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int opgroupscope_tracker_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != OPGROUPSCOPE_TRACKER_MAGIC)
		return -E_INVAL;
	size_t i, nenvs;

	for (i = 0, nenvs = 0; i < NENV; i++)
		if (va_is_mapped(CFS_IPC_OPGROUP_SCOPE_CAPPGS + i*PGSIZE))
			nenvs++;
	
	snprintf(string, length, "envs: %u", nenvs);
	return 0;
}

static int opgroupscope_tracker_open(CFS_t * cfs, const char * name, int mode)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, open, name, mode);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_close(CFS_t * cfs, int fid)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, close, fid);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	size_t i;
	int r;

	
	if((r = modman_rem_cfs(cfs)) < 0)
		return r;
	modman_dec_cfs(this_state.frontend_cfs, cfs);

	open_opgroup_scope_gc();
	for (i = 0; i < NENV; i++)
		if (va_is_mapped(CFS_IPC_OPGROUP_SCOPE_CAPPGS + i*PGSIZE))
			kdprintf(STDERR_FILENO, "%s: cappg %u still mapped\n", __FUNCTION__, i);

	this_state.frontend_cfs = NULL;

	opgroupscope_tracker_cfs_exists = 0;

	memset(cfs, 0, sizeof(*cfs));
	return 0;
}

static int opgroupscope_tracker_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, read, fid, data, offset, size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, write, fid, data, offset, size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, getdirentries, fid, buf, nbytes, basep);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, truncate, fid, target_size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_unlink(CFS_t * cfs, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, unlink, name);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, link, oldname, newname);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, rename, oldname, newname);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_mkdir(CFS_t * cfs, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, mkdir, name);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_rmdir(CFS_t * cfs, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, rmdir, name);
	clear_cur_opgroup_scope();
	return r;
}

static size_t opgroupscope_tracker_get_num_features(CFS_t * cfs, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return 0;
	r = CALL(this_state.frontend_cfs, get_num_features, name);
	clear_cur_opgroup_scope();
	return r;
}

static const feature_t * opgroupscope_tracker_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	const feature_t * f;
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return NULL;
	f = CALL(this_state.frontend_cfs, get_feature, name, num);
	clear_cur_opgroup_scope();
	return f;
}

static int opgroupscope_tracker_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, get_metadata, name, id, size, data);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, set_metadata, name, id, size, data);
	clear_cur_opgroup_scope();
	return r;
}


// CFS_t management

CFS_t * opgroupscope_tracker_cfs(CFS_t * frontend_cfs)
{
	size_t i;

	if (opgroupscope_tracker_cfs_exists)
		panic("opgroupscope_tracker_cfs can currently have at most one instance.");

	if (!frontend_cfs)
		return NULL;

	CFS_INIT(&this_cfs, opgroupscope_tracker, &this_state);
	OBJMAGIC(&this_cfs) = OPGROUPSCOPE_TRACKER_MAGIC;

	for (i = 0; i < NENV; i++)
		if (va_is_mapped(CFS_IPC_OPGROUP_SCOPE_CAPPGS + i*PGSIZE))
			panic("%s: cappg %u is in use.\n", __FUNCTION__, i);
	memset(env_scopes, 0, sizeof(env_scopes));
	this_state.frontend_cfs = frontend_cfs;

	if(modman_add_anon_cfs(&this_cfs, __FUNCTION__))
	{
		DESTROY(&this_cfs);
		return NULL;
	}
	if(modman_inc_cfs(frontend_cfs, &this_cfs, NULL) < 0)
	{
		modman_rem_cfs(&this_cfs);
		DESTROY(&this_cfs);
		return NULL;
	}

	opgroupscope_tracker_cfs_exists = 1;

	return &this_cfs;
}



//
// cfs_ipc_opgroup


opgroup_id_t cfs_ipc_opgroup_create(envid_t envid, int flags)
{
	opgroup_t * opgroup;
	opgroup_id_t opgroupid;
	int r = -E_BAD_ENV;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, flags = %d): env has no scope\n", __FUNCTION__, envid, flags);
		return r;
	}
	Dprintf("%s(env = %08x, flags = %d)\n", __FUNCTION__, envid, flags);
	opgroup = opgroup_create(flags);
	opgroupid = opgroup_id(opgroup);
	clear_cur_opgroup_scope();
	Dprintf("\t%s: opgroup = 0x%08x, opgroupid = %d\n", __FUNCTION__, opgroup, opgroupid);
	return opgroupid;
}

int cfs_ipc_opgroup_add_depend(envid_t envid, opgroup_id_t dependent_id, opgroup_id_t dependency_id)
{
	int r = -E_BAD_ENV;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, dependent_id = %d, dependency_id = %d): env has no scope\n", __FUNCTION__, envid, dependent_id, dependency_id);
		return r;
	}
	Dprintf("%s(env = %08x, dependent_id = %d, dependency_id = %d)\n", __FUNCTION__, envid, dependent_id, dependency_id);
	r = opgroup_add_depend(opgroup_lookup(dependent_id), opgroup_lookup(dependency_id));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_engage(envid_t envid, opgroup_id_t opgroupid)
{
	int r = -E_BAD_ENV;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, opgroupid = %d): env has no scope\n", __FUNCTION__, envid, opgroupid);
		return r;
	}
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	r = opgroup_engage(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_disengage(envid_t envid, opgroup_id_t opgroupid)
{
	int r = -E_BAD_ENV;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, opgroupid = %d): env has no scope\n", __FUNCTION__, envid, opgroupid);
		return r;
	}
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	r = opgroup_disengage(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_release(envid_t envid, opgroup_id_t opgroupid)
{
	int r = -E_BAD_ENV;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, opgroupid = %d): env has no scope\n", __FUNCTION__, envid, opgroupid);
		return r;
	}
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	r = opgroup_release(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_abandon(envid_t envid, opgroup_id_t opgroupid)
{
	int r = -E_BAD_ENV;
	opgroup_t * opgroup;
	if (!env_scope_exists(envid) || (r = set_cur_opgroup_scope(envid)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x, opgroupid = %d): env has no scope\n", __FUNCTION__, envid, opgroupid);
		return r;
	}
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	opgroup = opgroup_lookup(opgroupid);
	r = opgroup_abandon(&opgroup);
	clear_cur_opgroup_scope();
	return r;
}
