// TODO:
// - ls on a directory should also show mounts to names in that directory


#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/hash_map.h>
#include <lib/vector.h>

#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <kfs/traverse.h>
#include <kfs/mount_selector_cfs.h>

#define MOUNT_SELECTOR_DEBUG 0

#if MOUNT_SELECTOR_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


static CFS_t * singleton_mount_selector_cfs = NULL;

// 
// Data structs and initers


struct mount_selector_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	CFS_t * cfs;
};
typedef struct mount_selector_fdesc mount_selector_fdesc_t;

struct mount_selector_state {
	vector_t * mount_table;
	hash_map_t * cfs_nusers; // CFS_t* -> uint32_t nusers
	uint32_t nopen;
	CFS_t * selected_cfs;
};
typedef struct mount_selector_state mount_selector_state_t;


//
// mount_entry_t functions

static mount_entry_t * mount_entry_create(const char * path, CFS_t * cfs)
{
	mount_entry_t * me = malloc(sizeof(*me));
	if (!me)
		return NULL;

	me->path = strdup(path);
	me->cfs = cfs;
	return me;
}

static void mount_entry_destroy(mount_entry_t * me)
{
	free((void *) me->path);
	me->path = NULL;
	me->cfs = NULL;
	free(me);
}


//
// mount_selector_fdesc_t functions

static int mount_selector_fdesc_create(fdesc_t * inner, CFS_t * cfs, fdesc_t ** fdesc)
{
	mount_selector_fdesc_t * msf = malloc(sizeof(*msf));
	if (!msf)
		return -E_NO_MEM;

	assert(inner && fdesc && cfs);
	msf->common = inner->common;
	msf->inner = inner;
	msf->cfs = cfs;
	*fdesc = (fdesc_t *) msf;
	return 0;
}

static void mount_selector_fdesc_destroy(mount_selector_fdesc_t * msf)
{
	msf->common = NULL;
	msf->inner = NULL;
	msf->cfs = NULL;
	free(msf);
}


//
// cfs_nusers functions

static int cfs_nusers_inc(mount_selector_state_t * state, const CFS_t * cfs)
{
	Dprintf("%s(0x%08x, 0x%08x)\n", __FUNCTION__, state, cfs);
	uint32_t nusers = (uint32_t) hash_map_find_val(state->cfs_nusers, cfs);
	if (nusers)
		(void) hash_map_erase(state->cfs_nusers, cfs);
	return hash_map_insert(state->cfs_nusers, (void *) cfs, (void *) ++nusers);
}

static int cfs_nusers_dec(mount_selector_state_t * state, const CFS_t * cfs)
{
	Dprintf("%s(0x%08x, 0x%08x)\n", __FUNCTION__, state, cfs);
	uint32_t nusers;
	nusers = (uint32_t) hash_map_find_val(state->cfs_nusers, cfs);
	assert(nusers);
	(void) hash_map_erase(state->cfs_nusers, cfs);
	if (nusers == 1)
		return 0;
	else
		return hash_map_insert(state->cfs_nusers, (void *) cfs, (void *) --nusers);
}

static uint32_t cfs_nusers_count(mount_selector_state_t * state, const CFS_t * cfs)
{
	Dprintf("%s(0x%08x, 0x%08x)\n", __FUNCTION__, state, cfs);
	return (uint32_t) hash_map_find_val(state->cfs_nusers, (void *) cfs);
}


//
// mount_table_t functions

// Find the index into mount_table of the given mount-point's path
static int mount_lookup(vector_t * mount_table, const char * path)
{
	Dprintf("%s(0x%08x, \"%s\")\n", __FUNCTION__, mount_table, path);
	const size_t mount_table_size = vector_size(mount_table);
	int i;
	
	for (i = 0; i < mount_table_size; i++)
	{
		const mount_entry_t * me = (mount_entry_t *) vector_elt(mount_table, i);
		if (!strcmp(me->path, path))
			return i;
	}

	return -E_NOT_FOUND;
}

//
// mount_selector_cfs

static int mount_selector_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != MOUNT_SELECTOR_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int mount_selector_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != MOUNT_SELECTOR_MAGIC)
		return -E_INVAL;
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	
	snprintf(string, length, "open fdescs: %u, active cfses: %u", state->nopen, hash_map_size(state->cfs_nusers));
	return 0;
}

static int mount_selector_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s\n", __FUNCTION__);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, get_root, ino);
}

static int mount_selector_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u: \"%s\")\n", __FUNCTION__, parent, name);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, lookup, parent, name, ino);
}

static int open_common(mount_selector_state_t * state, fdesc_t * inner, fdesc_t ** fdesc)
{
	int r;
	if ((r = mount_selector_fdesc_create(inner, state->selected_cfs, fdesc)) < 0)
		goto exit_open;
	if ((r = cfs_nusers_inc(state, state->selected_cfs)) < 0)
		goto exit_mount_selector_fdesc;

	state->nopen++;
	return 0;

  exit_mount_selector_fdesc:
	(void) cfs_nusers_dec(state, state->selected_cfs);
  exit_open:
	(void) CALL(state->selected_cfs, close, *fdesc);
	*fdesc = NULL;
	return r;
}

static int mount_selector_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	if ((r = CALL(state->selected_cfs, open, ino, mode, &inner)) < 0)
		return r;
	return open_common(state, inner, fdesc);
}

static int mount_selector_create(CFS_t * cfs, inode_t parent, const char * name, int mode, fdesc_t ** fdesc, inode_t * newino)
{
	Dprintf("%s(%u: \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	if ((r = CALL(state->selected_cfs, create, parent, name, mode, &inner, newino)) < 0)
		return r;
	r = open_common(state, inner, fdesc);
	if (r < 0)
		*newino = INODE_NONE;
	return r;
}

static int mount_selector_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, fdesc);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	mount_selector_fdesc_t * msf = (mount_selector_fdesc_t *) fdesc;
	int r;

	r = CALL(msf->cfs, close, msf->inner);
	(void) cfs_nusers_dec(state, msf->cfs);
	(void) mount_selector_fdesc_destroy(msf);
	state->nopen--;
	return r;
}

static int mount_selector_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	mount_selector_fdesc_t * msf = (mount_selector_fdesc_t *) fdesc;
	return CALL(msf->cfs, read, msf->inner, data, offset, size);
}

static int mount_selector_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	mount_selector_fdesc_t * msf = (mount_selector_fdesc_t *) fdesc;
	return CALL(msf->cfs, write, msf->inner, data, offset, size);
}

static int mount_selector_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s(0x%08x, 0x%x, %d, 0x%x)\n", __FUNCTION__, fdesc, entry, size, basep);
	mount_selector_fdesc_t * msf = (mount_selector_fdesc_t *) fdesc;
	return CALL(msf->cfs, get_dirent, msf->inner, entry, size, basep);
}

static int mount_selector_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t size)
{
	Dprintf("%s(0x%08x, %u)\n", __FUNCTION__, fdesc, size);
	mount_selector_fdesc_t * msf = (mount_selector_fdesc_t *) fdesc;
	return CALL(msf->cfs, truncate, msf->inner, size);
}

static int mount_selector_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, unlink, parent, name);
}

static int mount_selector_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, ino, newparent, newname);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, link, ino, newparent, newname);
}

static int mount_selector_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u: \"%s\", %u: \"%s\")\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, rename, oldparent, oldname, newparent, newname);
}

static int mount_selector_mkdir(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u: \"%s\")\n", __FUNCTION__, parent, name);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, mkdir, parent, name, ino);
}

static int mount_selector_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u: \"%s\")\n", __FUNCTION__, parent, name);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, rmdir, parent, name);
}

static size_t mount_selector_get_num_features(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, get_num_features, ino);
}

static const feature_t * mount_selector_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, num);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return NULL;

	return CALL(state->selected_cfs, get_feature, ino, num);
}

static int mount_selector_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, id);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, get_metadata, ino, id, size, data);
}

static int mount_selector_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(%u, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, ino, id, size, data);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);

	if (!state->selected_cfs)
		return -E_NOT_FOUND;

	return CALL(state->selected_cfs, set_metadata, ino, id, size, data);
}

static int mount_selector_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;

	if (cfs == singleton_mount_selector_cfs)
		singleton_mount_selector_cfs = NULL;

	hash_map_destroy(state->cfs_nusers);
	vector_destroy(state->mount_table);
	memset(state, 0, sizeof(*state));
	free(state);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * mount_selector_cfs(void)
{
	mount_selector_state_t * state;
	CFS_t * cfs;
	
	if (singleton_mount_selector_cfs)
		return singleton_mount_selector_cfs;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;

	CFS_INIT(cfs, mount_selector, state);
	OBJMAGIC(cfs) = MOUNT_SELECTOR_MAGIC;

	state->selected_cfs = NULL;

	state->cfs_nusers = hash_map_create();
	if (!state->cfs_nusers)
		goto error_state;
	state->nopen = 0;

	state->mount_table = get_mount_table();
	if (!state->mount_table)
		goto error_cfs_nusers;

	if (modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}

	singleton_mount_selector_cfs = cfs;

	return cfs;

  error_cfs_nusers:
	hash_map_destroy(state->cfs_nusers);
  error_state:
	free(OBJLOCAL(cfs));
  error_cfs:
	free(cfs);
	return NULL;
}

int mount_selector_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	int r;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != MOUNT_SELECTOR_MAGIC)
		return -E_INVAL;
	
	/* force paths to start with / */
	if (path[0] != '/')
		return -E_INVAL;

	const int already_mounted = mount_lookup(state->mount_table, path);
	if (0 <= already_mounted)
		return -E_INVAL;

	mount_entry_t * me = mount_entry_create(path, path_cfs);
	if (!me)
		return -E_NO_MEM;

	if ((r = vector_push_back(state->mount_table, me)) < 0)
	{
		mount_entry_destroy(me);
		return r;
	}

	if ((r = modman_inc_cfs(path_cfs, cfs, path)) < 0)
	{
		vector_pop_back(state->mount_table);
		mount_entry_destroy(me);
		return r;
	}

	kdprintf(STDERR_FILENO, "mount_selector_cfs: mount to %s\n", path);
	return 0;
}

int singleton_mount_selector_cfs_add(const char * path, CFS_t * path_cfs)
{
	if (!singleton_mount_selector_cfs)
		return -E_BUSY;
	return mount_selector_cfs_add(singleton_mount_selector_cfs, path, path_cfs);
}

CFS_t * mount_selector_cfs_remove(CFS_t * cfs, const char *path)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(cfs);
	mount_entry_t * me;
	CFS_t * path_cfs = NULL;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != MOUNT_SELECTOR_MAGIC)
		return NULL;

	int idx = mount_lookup(state->mount_table, path);
	if (idx < 0)
		return NULL;
	me = vector_elt(state->mount_table, idx);

	// Allow unmount only if there are no open fdescs on path.
	// Only at this time because people above us may care and don't know
	// who such users may be.
	if (cfs_nusers_count(state, me->cfs) > 0)
		return NULL;

	kdprintf(STDERR_FILENO,"mount_selector_cfs: removed mount at %s\n", path);
	vector_erase(state->mount_table, idx);
	path_cfs = me->cfs;
	mount_entry_destroy(me);

	modman_dec_cfs(path_cfs, cfs);

	return path_cfs;
}

void mount_selector_cfs_set(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);

	if (singleton_mount_selector_cfs)
	{
		mount_selector_state_t * state = (mount_selector_state_t *) OBJLOCAL(singleton_mount_selector_cfs);
		state->selected_cfs = cfs;
	}
}
