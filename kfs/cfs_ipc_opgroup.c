#include <assert.h>
#include <inc/lib.h>
#include <lib/jiffies.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/opgroup.h>
#include <kfs/cfs_ipc_opgroup.h>

/* just the cfs_ipc_opgroup_scope_* functions */
#define CFS_IPC_OPGROUP_SCOPE_DEBUG 0
/* just the cfs_ipc_opgroup_* functions (does not include scope functions) */
#define CFS_IPC_OPGROUP_DEBUG 0

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

#define OPGROUP_SCOPE_GC_PERIOD (10*HZ)


//
// cfs_ipc_opgroup_scopes


typedef struct scope_entry {
	opgroup_scope_t * scope;
	envid_t client;
	uintptr_t client_scope_va;
} scope_entry_t;

static scope_entry_t env_scopes[NENV];

static void opgroup_scope_gc(void);


static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

static bool env_page_is_mapped(const void * pg, envid_t envid, uintptr_t envid_va)
{
	int r = sys_page_is_mapped((void *) pg, envid, envid_va);
	if (r < 0 && r != -E_BAD_ENV)
		panic("sys_page_is_mapped(0x%08x, %08x, 0x%08x): %i\n", pg, envid, envid_va);
	return r == 1;
}

static const void * env_scope_cappg(envid_t envid)
{
	return CFS_IPC_OPGROUP_SCOPE_CAPPGS + ENVX(envid)*PGSIZE;
}

static void env_scope_clear(size_t index)
{
	assert(index < NENV);
	env_scopes[index].scope = NULL;
	env_scopes[index].client_scope_va = 0;
	env_scopes[index].client = 0;
}

static void env_scope_set(envid_t client, opgroup_scope_t * scope, uintptr_t client_scope_va)
{
	assert(client && scope);
	env_scopes[ENVX(client)].scope = scope;
	env_scopes[ENVX(client)].client_scope_va = client_scope_va;
	env_scopes[ENVX(client)].client = client;
}

// An env scope is dead if it was alive and has since died
// (died = exited or otherwise unmapped the page)
static bool env_scope_is_dead(envid_t envid)
{
	bool is_alive;
	int nrefs;

	if (!va_is_mapped(env_scope_cappg(envid)))
		return 0;

	is_alive = env_page_is_mapped(env_scope_cappg(envid), envid, env_scopes[ENVX(envid)].client_scope_va);
	if (is_alive && ((nrefs = pageref(env_scope_cappg(envid))) != 2))
		panic("opgroup scope page not designed to have >2 (%d) refs\n", nrefs);
	return !is_alive;
}

static bool env_scope_exists(envid_t envid)
{
	return (!env_scope_is_dead(envid)) && va_is_mapped(env_scope_cappg(envid));
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
	DSprintf("%s(env_slot = 0x%03x)\n", __FUNCTION__, ENVX(envid));

	if (env_scope_exists(envid))
		kdprintf(STDERR_FILENO, "%s(%08x): env scope is still valid, destroying anyway\n", __FUNCTION__, envid);
	else if ((r = pageref(env_scope_cappg(envid))) > 1)
		kdprintf(STDERR_FILENO, "%s(%08x): env scope's cappg still has %d references, destroying anyway\n", __FUNCTION__, envid, r);

	if ((r = sys_page_unmap(0, (void *) env_scope_cappg(envid))) < 0)
		return r;
	opgroup_scope_destroy(env_scopes[ENVX(envid)].scope);
	env_scope_clear(ENVX(envid));
	return 0;
}


static int set_cur_opgroup_scope(envid_t envid)
{
	if (env_scope_is_dead(envid))
	{
		// The calling env may happen to have the same env slot as a previous,
		// now dead env, that we have not yet gc()ed
		opgroup_scope_gc();
		if (env_scope_is_dead(envid))
			return -E_BAD_ENV; // error for now, but we might want to create
	}
	opgroup_scope_set_current(env_scope(envid));
	return 0;
}

static void clear_cur_opgroup_scope(void)
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

	if (!env_page_is_mapped(scope_cappg, envid, envid_scope_cappg_va))
		return -E_INVAL;

	if (env_scope_exists(envid))
		return -E_UNSPECIFIED; // disallow re-creation

	if (env_scope_is_dead(envid))
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

	env_scope_set(envid, env_scope, envid_scope_cappg_va);

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
	if (env_scope_exists(child) && !env_scope_is_dead(child))
		return -E_INVAL; // disallow re-creation
	if (!env_page_is_mapped(child_scope_cappg, child, child_scope_cappg_va))
		return -E_INVAL;

	if (env_scope_is_dead(child))
	{
		DSprintf("%s: ", __FUNCTION__); // prepend env_scope_destroy's output
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

	env_scope_set(child, child_env_scope, child_scope_cappg_va);

	return 0;
}



//
// opgroupscope_tracker_cfs


// Because opgroupscope_tracker decides when to deactivate an environment's
// opgroup scope based on the pageref number for the env opgroup scope cappa,
// opgroupscope_tracker would never deactivate any scopes in use by
// multiple opgroupscope_trackers.
//
// Three possibilities to keep this from happening:
// 1- Assume this won't happen.
// 2- Figure out if a given page is already in use by another
//    opgroupscope_tracker.
// 3- Allow at most one opgroupscope_tracker to exist at a given time.
// Possibility 3 is safe (1 is not), simpler than 2, and at least for now
// multiple opgroupscope_trackers aren't something we want.
// so possibility 3 it is:
static bool opgroupscope_tracker_cfs_exists = 0;


typedef struct opgroupscope_tracker_state {
	CFS_t * frontend_cfs;
} opgroupscope_tracker_state_t;

static opgroupscope_tracker_state_t this_state;
static CFS_t this_cfs;


static void opgroup_scope_gc(void)
{
	int i, r;
	DSprintf("%s()\n", __FUNCTION__);
	for (i = 0; i < NENV; i++)
	{
		envid_t envid = env_scopes[i].client;
		if (env_scope_is_dead(envid))
		{
			r = env_scope_destroy(envid);
			assert(r >= 0);
			env_scope_clear(ENVX(envid));
		}
	}
}

static void opgroup_scope_gc_callback(void * ignore)
{
	opgroup_scope_gc();
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
		if (env_scope_exists(env_scopes[i].client))
			nenvs++;
	
	snprintf(string, length, "envs: %u", nenvs);
	return 0;
}

static int opgroupscope_tracker_get_root(CFS_t * cfs, inode_t * ino)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, get_root, ino);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, lookup, parent, name, ino);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, open, ino, mode, fdesc);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_create(CFS_t * cfs, inode_t parent, const char * name, int mode, const metadata_set_t * initialmd, fdesc_t ** fdesc, inode_t * newino)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, create, parent, name, mode, initialmd, fdesc, newino);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_close(CFS_t * cfs, fdesc_t * fdesc)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, close, fdesc);
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

	// ignore return because constructor can call before gc is registered
	(void) sched_unregister(opgroup_scope_gc_callback, NULL);

	opgroup_scope_gc();
	for (i = 0; i < NENV; i++)
		if (va_is_mapped(CFS_IPC_OPGROUP_SCOPE_CAPPGS + i*PGSIZE))
			kdprintf(STDERR_FILENO, "%s: cappg %u still mapped\n", __FUNCTION__, i);

	this_state.frontend_cfs = NULL;

	opgroupscope_tracker_cfs_exists = 0;

	memset(cfs, 0, sizeof(*cfs));
	return 0;
}

static int opgroupscope_tracker_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, read, fdesc, data, offset, size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, write, fdesc, data, offset, size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, get_dirent, fdesc, entry, size, basep);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t target_size)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, truncate, fdesc, target_size);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, unlink, parent, name);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, link, ino, newparent, newname);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, rename, oldparent, oldname, newparent, newname);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_mkdir(CFS_t * cfs, inode_t parent, const char * name, const metadata_set_t * initialmd, inode_t * ino)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, mkdir, parent, name, initialmd, ino);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, rmdir, parent, name);
	clear_cur_opgroup_scope();
	return r;
}

static size_t opgroupscope_tracker_get_num_features(CFS_t * cfs, inode_t ino)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return 0;
	r = CALL(this_state.frontend_cfs, get_num_features, ino);
	clear_cur_opgroup_scope();
	return r;
}

static const feature_t * opgroupscope_tracker_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	const feature_t * f;
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return NULL;
	f = CALL(this_state.frontend_cfs, get_feature, ino, num);
	clear_cur_opgroup_scope();
	return f;
}

static int opgroupscope_tracker_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, get_metadata, ino, id, size, data);
	clear_cur_opgroup_scope();
	return r;
}

static int opgroupscope_tracker_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	int r = set_cur_opgroup_scope(cfs_ipc_serve_cur_envid());
	if (r < 0)
		return r;
	r = CALL(this_state.frontend_cfs, set_metadata, ino, id, size, data);
	clear_cur_opgroup_scope();
	return r;
}


// CFS_t management

CFS_t * opgroupscope_tracker_cfs(CFS_t * frontend_cfs)
{
	size_t i;
	int r;

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

	if ((r = sched_register(opgroup_scope_gc_callback, NULL, OPGROUP_SCOPE_GC_PERIOD)) < 0)
	{
		DESTROY(&this_cfs);
		return NULL;
	}

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


static int set_cur_opgroup_scope_wrap(envid_t envid, const char * call_fn)
{
	int r;
	if (!env_scope_exists(envid))
	{
		kdprintf(STDERR_FILENO, "%s(env = %08x): env has no scope\n", __FUNCTION__, envid);
		return -E_BAD_ENV;
	}

	r = set_cur_opgroup_scope(envid);
	assert(r >= 0);

	return 0;
}


opgroup_id_t cfs_ipc_opgroup_create(envid_t envid, int flags)
{
	int r;
	opgroup_t * opgroup;
	opgroup_id_t opgroupid;
	Dprintf("%s(env = %08x, flags = %d)\n", __FUNCTION__, envid, flags);
	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	opgroup = opgroup_create(flags);
	opgroupid = opgroup_id(opgroup);
	clear_cur_opgroup_scope();
	Dprintf("\t%s: opgroup = 0x%08x, opgroupid = %d\n", __FUNCTION__, opgroup, opgroupid);
	return opgroupid;
}

int cfs_ipc_opgroup_add_depend(envid_t envid, opgroup_id_t dependent_id, opgroup_id_t dependency_id)
{
	int r;
	Dprintf("%s(env = %08x, dependent_id = %d, dependency_id = %d)\n", __FUNCTION__, envid, dependent_id, dependency_id);

	// Adding a dependent to dependency_id requires that dependency_id to
	// be disengaged. Because exiting a process disengages, we must
	// gc() all scopes that contain dependency_id to ensure it is
	// disengaged if it should be. Because we do not have a map of
	// opgroup ids to scopes, gc() all scopes:
	opgroup_scope_gc();

	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	r = opgroup_add_depend(opgroup_lookup(dependent_id), opgroup_lookup(dependency_id));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_engage(envid_t envid, opgroup_id_t opgroupid)
{
	int r;
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	r = opgroup_engage(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_disengage(envid_t envid, opgroup_id_t opgroupid)
{
	int r;
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	r = opgroup_disengage(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_release(envid_t envid, opgroup_id_t opgroupid)
{
	int r;
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);

	// Releasing an atomic opgroup requires that opgroupid to be disengaged.
	// Because exiting a process disengages, we must gc() all scopes that
	// contain opgroupid to ensure it is disengaged if it should be. Because
	// we do not have a map of opgroup ids to scopes, gc() all scopes:
	// TODO: only call for atomic opgroups and when opgroupid is engaged
	// (these facts are private to opgroup.c)
	opgroup_scope_gc();

	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	r = opgroup_release(opgroup_lookup(opgroupid));
	clear_cur_opgroup_scope();
	return r;
}

int cfs_ipc_opgroup_abandon(envid_t envid, opgroup_id_t opgroupid)
{
	int r;
	opgroup_t * opgroup;
	Dprintf("%s(env = %08x, opgroupid = %d)\n", __FUNCTION__, envid, opgroupid);
	if ((r = set_cur_opgroup_scope_wrap(envid, __FUNCTION__)))
		return r;
	opgroup = opgroup_lookup(opgroupid);
	r = opgroup_abandon(&opgroup);
	clear_cur_opgroup_scope();
	return r;
}
