// TODO:
// - ls on a directory should also show mounts to names in that directory


#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/hash_map.h>
#include <lib/vector.h>

#include <kfs/fidman.h>
#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <kfs/traverse.h>
#include <kfs/table_classifier_cfs.h>

#define TABLE_CLASSIFIER_DEBUG 0

#if TABLE_CLASSIFIER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


static CFS_t * singleton_table_classifier_cfs = NULL;
static CFS_t * selected_cfs = NULL;

// 
// Data structs and initers

struct open_file {
	int fid;
	CFS_t * cfs;
};
typedef struct open_file open_file_t;

struct table_classifier_state {
	vector_t * mount_table;
	hash_map_t * open_files;
};
typedef struct table_classifier_state table_classifier_state_t;


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
// open_file_t functions

static open_file_t * open_file_create(int fid, CFS_t * cfs)
{
	open_file_t * f = malloc(sizeof(*f));
	if (!f)
		return NULL;

	f->fid = fid;
	f->cfs = cfs;
	return f;
}

static void open_file_destroy(open_file_t * f)
{
	f->fid = -1;
	f->cfs = NULL;
	free(f);
}


//
// open_files functions

// Add a fid-cfs pair
static int fid_table_add(table_classifier_state_t * state, int fid, CFS_t * cfs)
{
	Dprintf("%s(0x%08x, %d, 0x%08x)\n", __FUNCTION__, state, fid, cfs);
	open_file_t * f;
	int r;

	f = open_file_create(fid, cfs);
	if (!f)
		return -E_NO_MEM;
	r = hash_map_insert(state->open_files, (void*) fid, f);
	if (r < 0)
	{
		open_file_destroy(f);
		return r;
	}

	return 0;
}

// Get the existing cfs for fid
static CFS_t * fid_table_get(const table_classifier_state_t * state, int fid)
{
	Dprintf("%s(0x%08x, %d)\n", __FUNCTION__, state, fid);
	open_file_t * f;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return NULL;

	return f->cfs;
}

// Delete the cfs-fid entry for fid
static int fid_table_del(table_classifier_state_t * state, int fid)
{
	Dprintf("%s(0x%08x, %d)\n", __FUNCTION__, state, fid);
	open_file_t * f;

	f = hash_map_erase(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	open_file_destroy(f);
	return 0;
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
// table_classifier_cfs

static int table_classifier_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != TABLE_CLASSIFIER_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int table_classifier_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != TABLE_CLASSIFIER_MAGIC)
		return -E_INVAL;
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	
	snprintf(string, length, "fids: %u", hash_map_size(state->open_files));
	return 0;
}

static int table_classifier_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s\n", __FUNCTION__);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, get_root, ino);
}

static int table_classifier_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%d: \"%s\")\n", __FUNCTION__, parent, name);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, lookup, parent, name, ino);
}

static int table_classifier_open(CFS_t * cfs, inode_t ino, int mode)
{
	Dprintf("%s(%d, %d)\n", __FUNCTION__, ino, mode);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	int fid;
	int r;

	if (!selected_cfs)
		return -E_NOT_FOUND;

	if ((fid = CALL(selected_cfs, open, ino, mode)) < 0)
		return fid;
	if ((r = fid_table_add(state, fid, selected_cfs)) < 0)
	{
		(void) CALL(selected_cfs, close, fid);
		return r;
	}
	return fid;
}

static int table_classifier_create(CFS_t * cfs, inode_t parent, const char * name, int mode, inode_t * newino)
{
	Dprintf("%s(%d: \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	int fid;
	int r;

	if (!selected_cfs)
		return -E_NOT_FOUND;

	if ((fid = CALL(selected_cfs, create, parent, name, mode, newino)) < 0)
		return fid;
	if ((r = fid_table_add(state, fid, selected_cfs)) < 0)
	{
		(void) CALL(selected_cfs, close, fid);
		return r;
	}
	return fid;
}


static int table_classifier_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	CFS_t * selected_cfs = fid_table_get(state, fid);
	int r, s;

	if (!selected_cfs)
		return -E_NOT_FOUND;

	r = CALL(selected_cfs, close, fid);
	if (0 <= r)
		if ((s = fid_table_del(state, fid)) < 0)
			return s;
	return r;
}

static int table_classifier_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, read, fid, data, offset, size);
}

static int table_classifier_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, write, fid, data, offset, size);
}

static int table_classifier_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, getdirentries, fid, buf, nbytes, basep);
}

static int table_classifier_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, truncate, fid, size);
}

static int table_classifier_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%d, \"%s\")\n", __FUNCTION__, parent, name);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, unlink, parent, name);
}

static int table_classifier_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%d, %d, \"%s\")\n", __FUNCTION__, ino, newparent, newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, link, ino, newparent, newname);
}

static int table_classifier_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	Dprintf("%s(%d: \"%s\", %d: \"%s\")\n", __FUNCTION__, oldparent, oldname, newparent, newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, rename, oldparent, oldname, newparent, newname);
}

static int table_classifier_mkdir(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%d: \"%s\")\n", __FUNCTION__, parent, name);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, mkdir, parent, name, ino);
}

static int table_classifier_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%d: \"%s\")\n", __FUNCTION__, parent, name);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, rmdir, parent, name);
}

static size_t table_classifier_get_num_features(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%d)\n", __FUNCTION__, ino);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, get_num_features, ino);
}

static const feature_t * table_classifier_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	Dprintf("%s(%d, 0x%x)\n", __FUNCTION__, ino, num);

	if (!selected_cfs)
		return NULL;

	return CALL(selected_cfs, get_feature, ino, num);
}

static int table_classifier_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(%d, 0x%x)\n", __FUNCTION__, ino, id);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, get_metadata, ino, id, size, data);
}

static int table_classifier_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, ino, id, size, data);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, set_metadata, ino, id, size, data);
}

static int table_classifier_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;

	if (cfs == singleton_table_classifier_cfs)
		singleton_table_classifier_cfs = NULL;

	hash_map_destroy(state->open_files);
	vector_destroy(state->mount_table);
	memset(state, 0, sizeof(*state));
	free(state);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * table_classifier_cfs(void)
{
	table_classifier_state_t * state;
	CFS_t * cfs;
	
	if (singleton_table_classifier_cfs)
		return singleton_table_classifier_cfs;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;

	CFS_INIT(cfs, table_classifier, state);
	OBJMAGIC(cfs) = TABLE_CLASSIFIER_MAGIC;

	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	state->mount_table = get_mount_table();
	if (!state->mount_table)
		goto error_open_files;

	if (modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}

	singleton_table_classifier_cfs = cfs;

	return cfs;

  error_open_files:
	hash_map_destroy(state->open_files);
  error_state:
	free(OBJLOCAL(cfs));
  error_cfs:
	free(cfs);
	return NULL;
}

int table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	int r;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != TABLE_CLASSIFIER_MAGIC)
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

	kdprintf(STDERR_FILENO, "table_classifier_cfs: mount to %s\n", path);
	return 0;
}

int singleton_table_classifier_cfs_add(const char * path, CFS_t * path_cfs)
{
	if (!singleton_table_classifier_cfs)
		return -E_BUSY;
	return table_classifier_cfs_add(singleton_table_classifier_cfs, path, path_cfs);
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char *path)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	table_classifier_state_t * state = (table_classifier_state_t *) OBJLOCAL(cfs);
	mount_entry_t * me;
	CFS_t * path_cfs = NULL;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != TABLE_CLASSIFIER_MAGIC)
		return NULL;

	int idx = mount_lookup(state->mount_table, path);
	if (idx < 0)
		return NULL;
	me = vector_elt(state->mount_table, idx);

	// Allow unmount only if there are no open fids on path.
	// Only at this time because people above us may care and don't know
	// who such users may be.
	hash_map_it_t it;
	open_file_t * of;
	hash_map_it_init(&it, state->open_files);
	while ((of = hash_map_val_next(&it)))
	{
		if (of->cfs == me->cfs)
			return NULL;
	}

	kdprintf(STDERR_FILENO,"table_classifier_cfs: removed mount at %s\n", path);
	vector_erase(state->mount_table, idx);
	path_cfs = me->cfs;
	mount_entry_destroy(me);

	modman_dec_cfs(path_cfs, cfs);

	return path_cfs;
}

void table_classifier_cfs_set(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);

	if (singleton_table_classifier_cfs)
		selected_cfs = cfs;
}
