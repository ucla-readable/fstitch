#include <inc/lib.h>
#include <malloc.h>
#include <lib/hash_map.h>
#include <lib/vector.h>
#include <inc/dirent.h>

#include <kfs/fidman.h>
#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <kfs/file_hiding_cfs.h>

#define FILE_HIDING_DEBUG 0

#if FILE_HIDING_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// 
// Data structs and initers

struct hide_entry {
	const char * path;
};
typedef struct hide_entry hide_entry_t;

struct open_file {
	int fid;
	const char * path;
};
typedef struct open_file open_file_t;

struct file_hiding_state {
	vector_t * hide_table;
	hash_map_t * open_files;
	CFS_t * frontend_cfs;
};
typedef struct file_hiding_state file_hiding_state_t;


//
// hide_entry_t functions

static hide_entry_t * hide_entry_create(const char * path)
{
	hide_entry_t * me;

	if (!path || strlen(path) < 2 || path[0] != '/')
		return NULL;

	me = malloc(sizeof(*me));
	if (!me)
		return NULL;

	me->path = strdup(path);
	return me;
}

static void hide_entry_destroy(hide_entry_t * me)
{
	free((void *) me->path);
	me->path = NULL;
	free(me);
}


//
// hide_table_t functions

// Find the index for the given path in hide_table
static int hide_lookup(vector_t * hide_table, const char * path)
{
	Dprintf("%s(0x%08x, \"%s\")\n", __FUNCTION__, hide_table, path);
	const size_t hide_table_size = vector_size(hide_table);
	int i;

	for (i = 0; i < hide_table_size; i++)
	{
		const hide_entry_t * me = (hide_entry_t *) vector_elt(hide_table, i);
		if (!strcmp(me->path, path))
			return i;
	}

	return -E_NOT_FOUND;
}


//
// open_file_t functions

static open_file_t * open_file_create(int fid, const char * path)
{
	open_file_t * f = malloc(sizeof(*f));
	if (!f)
		return NULL;

	f->fid = fid;
	f->path = strdup(path);
	return f;
}

static void open_file_destroy(open_file_t * f)
{
	f->fid = -1;
	f->path = NULL;
	free(f);
}


//
// open_files functions

// Add a fid-cfs pair
static int fid_table_add(file_hiding_state_t * state, int fid, const char * path)
{
	Dprintf("%s(0x%08x, %d, 0x%08x)\n", __FUNCTION__, state, fid, cfs);
	open_file_t * f;
	int r;

	f = open_file_create(fid, path);
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
static const char * fid_table_get(const file_hiding_state_t * state, int fid)
{
	Dprintf("%s(0x%08x, %d)\n", __FUNCTION__, state, fid);
	open_file_t * f;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return NULL;

	return f->path;
}

// Delete the cfs-fid entry for fid
static int fid_table_del(file_hiding_state_t * state, int fid)
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
// file_hiding_cfs

static int file_hiding_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int file_hiding_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	snprintf(string, length, "fids: %u", hash_map_size(state->open_files));
	return 0;
}

static int file_hiding_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	int fid;
	int r;

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	if ((fid = CALL(state->frontend_cfs, open, name, mode)) < 0)
		return fid;
	if ((r = fid_table_add(state, fid, name)) < 0)
	{
		(void) CALL(state->frontend_cfs, close, fid);
		return r;
	}
	return fid;
}

static int file_hiding_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	const char * path = fid_table_get(state, fid);
	int r, s;

	if (!path)
		return -E_NOT_FOUND;

	r = CALL(state->frontend_cfs, close, fid);
	if (0 <= r)
		if ((s = fid_table_del(state, fid)) < 0)
			return s;
	return r;
}

static int file_hiding_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	const char * path = fid_table_get(state, fid);

	if (!path)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, read, fid, data, offset, size);
}

static int file_hiding_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	const char * path = fid_table_get(state, fid);

	if (!path)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, write, fid, data, offset, size);
}

static int file_hiding_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	const char * path = fid_table_get(state, fid);
	int i, r, s, hidden;
	dirent_t * d;
	char * fname;

	if (!path)
		return -E_NOT_FOUND;

	fname = malloc(strlen(path) + DIRENT_MAXNAMELEN + 1);
	if (!fname)
		return -E_NO_MEM;

	r = CALL(state->frontend_cfs, getdirentries, fid, buf, nbytes, basep);

	// Look for hidden files
	for (i = 0; i < r; )
	{
		d = (dirent_t *) (buf + i);

		strcpy(fname, path);
		strncpy(fname + strlen(fname), d->d_name, DIRENT_MAXNAMELEN);
		hidden = hide_lookup(state->hide_table, fname);
		if (0 <= hidden)
		{
			// Remove a hidden file
			s = d->d_reclen;
			memmove(buf + i, buf + i + s, r - i - s);
			r  -= s;
		}
		else
			i += d->d_reclen;
	}
	free(fname);
	return r;
}

static int file_hiding_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	const char * path = fid_table_get(state, fid);

	if (!path)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, truncate, fid, size);
}

static int file_hiding_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, unlink, name);
}

static int file_hiding_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, oldname) >= 0)
		return -E_NOT_FOUND;
	if (hide_lookup(state->hide_table, newname) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, link, oldname, newname);
}

static int file_hiding_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, oldname) >= 0)
		return -E_NOT_FOUND;
	if (hide_lookup(state->hide_table, newname) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, rename, oldname, newname);
}

static int file_hiding_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, mkdir, name);
}

static int file_hiding_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, rmdir, name);
}

static size_t file_hiding_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, get_num_features, name);
}

static const feature_t * file_hiding_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return NULL;

	return CALL(state->frontend_cfs, get_feature, name, num);
}

static int file_hiding_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, get_metadata, name, id, size, data);
}

static int file_hiding_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, set_metadata, name, id, size, data);
}

static int file_hiding_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, name) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, sync, name);
}

static int file_hiding_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_cfs(state->frontend_cfs, cfs);

	hash_map_destroy(state->open_files);
	vector_destroy(state->hide_table);
	memset(state, 0, sizeof(*state));
	free(state);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * file_hiding_cfs(CFS_t * frontend_cfs)
{
	file_hiding_state_t * state;
	CFS_t * cfs;

	if (!frontend_cfs)
		return NULL;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;

	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;

	CFS_INIT(cfs, file_hiding, state);
	OBJMAGIC(cfs) = FILE_HIDING_MAGIC;

	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	state->hide_table = vector_create();
	if (!state->hide_table)
		goto error_open_files;
	state->frontend_cfs = frontend_cfs;

	if (modman_add_anon_cfs(cfs, __FUNCTION__))
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

  error_open_files:
	hash_map_destroy(state->open_files);
  error_state:
	free(OBJLOCAL(cfs));
  error_cfs:
	free(cfs);
	return NULL;
}

int file_hiding_cfs_hide(CFS_t * cfs, const char * path)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	int r;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	/* force paths to start with / */
	if (path[0] != '/')
		return -E_INVAL;

	// Allow hiding only if the file isn't open
	hash_map_it_t it;
	open_file_t * of;
	hash_map_it_init(&it, state->open_files);
	while ((of = hash_map_val_next(&it)))
	{
		if (!strcmp(path, of->path))
			return -E_INVAL;
	}


	const int already_hidden = hide_lookup(state->hide_table, path);
	if (0 <= already_hidden)
		return -E_INVAL;

	hide_entry_t * me = hide_entry_create(path);
	if (!me)
		return -E_NO_MEM;

	if ((r = vector_push_back(state->hide_table, me)) < 0)
	{
		hide_entry_destroy(me);
		return r;
	}

	fprintf(STDERR_FILENO, "file_hiding_cfs: hiding %s\n", path);
	return 0;
}

int file_hiding_cfs_unhide(CFS_t * cfs, const char *path)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	hide_entry_t * me;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	int idx = hide_lookup(state->hide_table, path);
	if (idx < 0)
		return idx;
	me = vector_elt(state->hide_table, idx);

	fprintf(STDERR_FILENO,"file_hiding_cfs: unhiding %s\n", path);
	vector_erase(state->hide_table, idx);
	hide_entry_destroy(me);

	return 0;
}
