// TODO:
// - ls on a directory should also show mounts to names in that directory


#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/vector.h>

#include <kfs/cfs.h>
#include <kfs/uhfs.h>
#include <kfs/table_classifier_cfs.h>

#define TABLE_CLASSIFIER_DEBUG 0

#if TABLE_CLASSIFIER_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// 
// Data structs and initers

struct mount_entry {
	char * path;
	CFS_t * cfs;
};
typedef struct mount_entry mount_entry_t;

struct fid_entry {
	int fid;
	CFS_t * cfs;
};
typedef struct fid_entry fid_entry_t;

/* "TBLCLASS" */
#define TABLE_CLASSIFIER_MAGIC 0x7B1C1A55

struct table_classifier_state {
	uint32_t magic;
	vector_t * mount_table;
	fid_entry_t fid_table[UHFS_MAX_OPEN];
};
typedef struct table_classifier_state table_classifier_state_t;


static mount_entry_t * mount_entry(void)
{
	mount_entry_t * me = malloc(sizeof(*me));
	if (!me)
		return NULL;

	me->path = NULL;
	me->cfs = NULL;
	return me;
}

static void fid_entry_init(fid_entry_t * fe)
{
	fe->fid = -1;
	fe->cfs = NULL;
}


//
// fid_table_t functions

// Add a fid-cfs pair
static int fid_table_add(table_classifier_state_t * state, int fid, CFS_t * cfs)
{
	int i;
	for (i = 0; i < sizeof(state->fid_table)/sizeof(state->fid_table[0]); i++)
	{
		if (!state->fid_table[i].cfs)
		{
			state->fid_table[i].fid = fid;
			state->fid_table[i].cfs = cfs;
			return 0;
		}

		assert(fid != state->fid_table[i].fid);
	}
	return -E_NO_MEM;
}

// Get the existing cfs for fid
static CFS_t * fid_table_get(const table_classifier_state_t * state, int fid)
{
	int i;
	for (i = 0; i < sizeof(state->fid_table)/sizeof(state->fid_table[0]); i++)
	{
		if (state->fid_table[i].fid == fid)
			return state->fid_table[i].cfs;
	}
	return NULL;
}

// Delete teh cfs-fid entry for fid
static int fid_table_del(table_classifier_state_t * state, int fid)
{
	int i;
	for (i = 0; i < sizeof(state->fid_table)/sizeof(state->fid_table[0]); i++)
	{
		if (state->fid_table[i].fid == fid)
		{
			fid_entry_init(&state->fid_table[i]);
			return 0;
		}
	}
	return -E_NOT_FOUND;
}


//
// mount_table_t functions

// Find the index into mount_table of the given mount-point's path
int mount_lookup(vector_t * mount_table, const char * path)
{
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

// Return the cfs associated with the mount name is on, set transformed_name
// to be the filename this cfs expects
static CFS_t * lookup_cfs_name(vector_t * mount_table, const char * name, char ** transformed_name)
{
	const size_t mount_table_size = vector_size(mount_table);
	int i;

	for (i = 0; i < mount_table_size; i++)
	{
		const mount_entry_t *me = (mount_entry_t *) vector_elt(mount_table, i);
		const size_t path_len = strlen(me->path);

		if (!strncmp(me->path, name, path_len))
		{
			*transformed_name = (char *)name + path_len;
			return me->cfs;
		}
		else if ((me->path[0] == '/') && !strncmp(me->path+1, name, path_len-1))
		{
			*transformed_name = (char *)name + path_len - 1;
			return me->cfs;
		}
	}

	return NULL;
}


//
// table_classifier_cfs

static int table_classifier_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	Dprintf("%s(\"%s\", %d, 0x%x)\n", __FUNCTION__, name, mode, page);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);
	int fid;
	int r;

	if (!selected_cfs)
		return -E_NOT_FOUND;

	if ((fid = CALL(selected_cfs, open, newname, mode, page)) < 0)
		return fid;
	if ((r = fid_table_add(state, fid, selected_cfs)) < 0)
		return r;
	return fid;
}

static int table_classifier_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * selected_cfs = fid_table_get(state, fid);
	int r, s;

	if (!selected_cfs)
		return -E_NOT_FOUND;

	r = CALL(selected_cfs, close, fid);
	if (!r)
		if ((s = fid_table_del(state, fid)) < 0)
			return s;
	return r;
}

static int table_classifier_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, read, fid, data, offset, size);
}

static int table_classifier_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, write, fid, data, offset, size);
}

static int table_classifier_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep, offset);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, getdirentries, fid, buf, nbytes, basep);
}

static int table_classifier_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * selected_cfs = fid_table_get(state, fid);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, truncate, fid, size);
}

static int table_classifier_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, unlink, newname);
}

static int table_classifier_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * oldnewname, * newnewname;
	CFS_t * old_selected_cfs, * new_selected_cfs;

	old_selected_cfs = lookup_cfs_name(state->mount_table, oldname, &oldnewname);
	new_selected_cfs = lookup_cfs_name(state->mount_table, newname, &newnewname);
	if (old_selected_cfs != new_selected_cfs)
		return -E_INVAL;

	return CALL(old_selected_cfs, link, oldnewname, newnewname);
}

static int table_classifier_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * oldnewname, * newnewname;
	CFS_t * old_selected_cfs, * new_selected_cfs;

	old_selected_cfs = lookup_cfs_name(state->mount_table, oldname, &oldnewname);
	new_selected_cfs = lookup_cfs_name(state->mount_table, newname, &newnewname);
	if (old_selected_cfs != new_selected_cfs)
		return -E_INVAL;

	return CALL(old_selected_cfs, rename, oldnewname, newnewname);
}

static int table_classifier_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, mkdir, newname);
}

static int table_classifier_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, rmdir, newname);
}

static size_t table_classifier_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, get_num_features, newname);
}

static const feature_t * table_classifier_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return NULL;

	return CALL(selected_cfs, get_feature, newname, num);
}

static int table_classifier_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, get_metadata, newname, id, size, data);
}

static int table_classifier_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, set_metadata, newname, id, size, data);
}

static int table_classifier_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	char * newname = NULL;
	CFS_t * selected_cfs = lookup_cfs_name(state->mount_table, name, &newname);

	if (!selected_cfs)
		return -E_NOT_FOUND;

	return CALL(selected_cfs, sync, newname);
}

static int table_classifier_destroy(CFS_t * cfs)
{
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;

	vector_destroy(state->mount_table);
	memset(cfs->instance, 0, sizeof(*cfs->instance));
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * table_classifier_cfs(const char * paths[], CFS_t * cfses[], size_t num_entries)
{
	table_classifier_state_t * state;
	CFS_t * cfs;
	size_t i;
	int r;
	
	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;
	cfs->instance = state;
	state->magic = TABLE_CLASSIFIER_MAGIC;
	
	ASSIGN(cfs, table_classifier, open);
	ASSIGN(cfs, table_classifier, close);
	ASSIGN(cfs, table_classifier, read);
	ASSIGN(cfs, table_classifier, write);
	ASSIGN(cfs, table_classifier, getdirentries);
	ASSIGN(cfs, table_classifier, truncate);
	ASSIGN(cfs, table_classifier, unlink);
	ASSIGN(cfs, table_classifier, link);
	ASSIGN(cfs, table_classifier, rename);
	ASSIGN(cfs, table_classifier, mkdir);
	ASSIGN(cfs, table_classifier, rmdir);
	ASSIGN(cfs, table_classifier, get_num_features);
	ASSIGN(cfs, table_classifier, get_feature);
	ASSIGN(cfs, table_classifier, get_metadata);
	ASSIGN(cfs, table_classifier, set_metadata);
	ASSIGN(cfs, table_classifier, sync);
	ASSIGN_DESTROY(cfs, table_classifier, destroy);

	for (i = 0; i < sizeof(state->fid_table)/sizeof(state->fid_table[0]); i++)
		fid_entry_init(&state->fid_table[i]);


	state->mount_table = vector_create();
	if (!state->mount_table)
		goto error_state;

	for (i = 0; i < num_entries; i++)
		if ((r = table_classifier_cfs_add(cfs, paths[i], cfses[i])) < 0)
			goto error_mount_table;

	return cfs;

  error_mount_table:
	vector_destroy(state->mount_table);
  error_state:
	free(cfs->instance);
  error_cfs:
	free(cfs);
	return NULL;
}

int table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	int r;

	/* make sure this is really a table classifier */
	if (state->magic != TABLE_CLASSIFIER_MAGIC)
		return -E_INVAL;

	const int already_mounted = mount_lookup(state->mount_table, path);
	if (0 <= already_mounted)
		return -E_INVAL;

	mount_entry_t * me = mount_entry();
	if (!me)
		return -E_NO_MEM;
	me->path = (char *) path;
	me->cfs  = path_cfs;

	if ((r = vector_push_back(state->mount_table, me)) < 0)
	{
		free(me);
		return r;
	}

	fprintf(STDERR_FILENO, "table_classifier_cfs: mount to %s\n", path);
	return 0;
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char *path)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	table_classifier_state_t * state = (table_classifier_state_t *) cfs->instance;
	CFS_t * path_cfs = NULL;

	/* make sure this is really a table classifier */
	if (state->magic != TABLE_CLASSIFIER_MAGIC)
		return NULL;

	int idx = mount_lookup(state->mount_table, path);
	if (idx < 0)
		return NULL;

	fprintf(STDERR_FILENO,"table_classifier_cfs: removed mount at %s\n", path);
	path_cfs = vector_elt(state->mount_table, idx);
	vector_erase(state->mount_table, idx);
	return path_cfs;
}
