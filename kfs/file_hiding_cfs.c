#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/vector.h>
#include <lib/dirent.h>

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
	inode_t ino;
};
typedef struct hide_entry hide_entry_t;

struct file_hiding_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	inode_t ino;
};
typedef struct file_hiding_fdesc file_hiding_fdesc_t;

struct file_hiding_state {
	vector_t * hide_table;
	CFS_t * frontend_cfs;
	uint32_t nopen;
};
typedef struct file_hiding_state file_hiding_state_t;

//
// hide_entry_t functions

static hide_entry_t * hide_entry_create(inode_t ino)
{
	hide_entry_t * me;
	me = malloc(sizeof(*me));
	if (!me)
		return NULL;

	me->ino = ino;
	return me;
}

static void hide_entry_destroy(hide_entry_t * me)
{
	me->ino = INODE_NONE;
	free(me);
}


//
// hide_table_t functions

// Find the index for the given ino in hide_table
static int hide_lookup(vector_t * hide_table, inode_t ino)
{
	Dprintf("%s(0x%08x, %u)\n", __FUNCTION__, hide_table, ino);
	const size_t hide_table_size = vector_size(hide_table);
	int i;

	for (i = 0; i < hide_table_size; i++)
	{
		const hide_entry_t * me = (hide_entry_t *) vector_elt(hide_table, i);
		if (me->ino == ino)
			return i;
	}

	return -E_NOT_FOUND;
}


//
// file_hiding_fdesc_t functions

static int file_hiding_fdesc_create(fdesc_t * inner, inode_t ino, fdesc_t ** fdesc)
{
	file_hiding_fdesc_t * fhf = malloc(sizeof(*fhf));
	if (!fhf)
		return -E_NO_MEM;

	fhf->common = inner->common;
	fhf->inner = inner;
	fhf->ino = ino;

	*fdesc = (fdesc_t *) fhf;
	return 0;
}

static void file_hiding_fdesc_destroy(file_hiding_fdesc_t * fhf)
{
	fhf->common = NULL;
	fhf->inner = NULL;
	fhf->ino = INODE_NONE;
	free(fhf);
}


//
// file_hiding_cfs

static int file_hiding_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int file_hiding_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	snprintf(string, length, "open fdescs: %u", state->nopen);
	return 0;
}

static int file_hiding_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s()\n", __FUNCTION__);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	// root inode hiding is disallowed; others need the root inode
	return CALL(state->frontend_cfs, get_root, ino);
}

static int file_hiding_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t temp_ino;
	int r;

	r = CALL(state->frontend_cfs, lookup, parent, name, &temp_ino);
	if (r >= 0 && hide_lookup(state->hide_table, temp_ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, lookup, parent, name, ino);
}

static int file_hiding_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;

	if (hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	if ((r = CALL(state->frontend_cfs, open, ino, mode, &inner)) < 0)
		return r;
	if ((r = file_hiding_fdesc_create(inner, ino, fdesc)) < 0)
	{
		(void) CALL(state->frontend_cfs, close, inner);
		*fdesc = NULL;
		return r;
	}
	state->nopen++;
	return 0;
}

static int file_hiding_create(CFS_t * cfs, inode_t parent, const char * name, int mode, fdesc_t ** fdesc, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	fdesc_t * inner;
	inode_t temp_ino;
	int r;

	r = CALL(state->frontend_cfs, lookup, parent, name, &temp_ino);
	if (r >= 0 && hide_lookup(state->hide_table, temp_ino) >= 0)
		return -E_NOT_FOUND;

	if ((r = CALL(state->frontend_cfs, create, parent, name, mode, &inner, ino)) < 0)
		return r;

	if ((r = file_hiding_fdesc_create(inner, *ino, fdesc)) < 0)
	{
		(void) CALL(state->frontend_cfs, close, inner);
		*fdesc = NULL;
		*ino = INODE_NONE;
		return r;
	}
	state->nopen++;
	return 0;
}

static int file_hiding_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	file_hiding_fdesc_t * fhf = (file_hiding_fdesc_t *) fdesc;
	int r;

	r = CALL(state->frontend_cfs, close, fhf->inner);
	file_hiding_fdesc_destroy(fhf);
	state->nopen--;
	return r;
}

static int file_hiding_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t ofhfset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, ofhfset, size);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	file_hiding_fdesc_t * fhf = (file_hiding_fdesc_t *) fdesc;

	if (fhf->ino == INODE_NONE)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, read, fhf->inner, data, ofhfset, size);
}

static int file_hiding_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t ofhfset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, ofhfset, size);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	file_hiding_fdesc_t * fhf = (file_hiding_fdesc_t *) fdesc;

	if (fhf->ino == INODE_NONE)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, write, fhf->inner, data, ofhfset, size);
}

static int file_hiding_getdirentries(CFS_t * cfs, fdesc_t * fdesc, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	file_hiding_fdesc_t * fhf = (file_hiding_fdesc_t *) fdesc;
	int i, r, s, hidden;
	dirent_t * d;

	if (fhf->ino == INODE_NONE)
		return -E_NOT_FOUND;

	r = CALL(state->frontend_cfs, getdirentries, fhf->inner, buf, nbytes, basep);

	// Look for hidden files
	for (i = 0; i < r; )
	{
		d = (dirent_t *) (buf + i);
		hidden = hide_lookup(state->hide_table, d->d_fileno);
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
	return r;
}

static int file_hiding_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t size)
{
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	file_hiding_fdesc_t * fhf = (file_hiding_fdesc_t *) fdesc;

	if (fhf->ino == INODE_NONE)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, truncate, fhf->inner, size);
}

static int file_hiding_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t ino;
	int r;

	r = CALL(state->frontend_cfs, lookup, parent, name, &ino);
	if (r >= 0 && hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, unlink, parent, name);
}

static int file_hiding_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, ino, newparent, newname);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t newino;
	int r;

	if (hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	r = CALL(state->frontend_cfs, lookup, newparent, newname, &newino);
	if (r >= 0 && hide_lookup(state->hide_table, newino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, link, ino, newparent, newname);
}

static int file_hiding_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, \"%s\", %u, \"%s\")\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t ino;
	int r;

	r = CALL(state->frontend_cfs, lookup, oldparent, oldname, &ino);
	if (r >= 0 && hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;
	r = CALL(state->frontend_cfs, lookup, newparent, newname, &ino);
	if (r >= 0 && hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, rename, oldparent, oldname, newparent, newname);
}

static int file_hiding_mkdir(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t newino;
	int r;

	r = CALL(state->frontend_cfs, lookup, parent, name, &newino);
	if (r >= 0 && hide_lookup(state->hide_table, newino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, mkdir, parent, name, ino);
}

static int file_hiding_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	inode_t ino;
	int r;

	r = CALL(state->frontend_cfs, lookup, parent, name, &ino);
	if (r >= 0 && hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, rmdir, parent, name);
}

static size_t file_hiding_get_num_features(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, get_num_features, ino);
}

static const feature_t * file_hiding_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, ino) >= 0)
		return NULL;

	return CALL(state->frontend_cfs, get_feature, ino, num);
}

static int file_hiding_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, id);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);

	if (hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, get_metadata, ino, id, size, data);
}

static int file_hiding_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(%u, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, ino, id, size, data);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	if (hide_lookup(state->hide_table, ino) >= 0)
		return -E_NOT_FOUND;

	return CALL(state->frontend_cfs, set_metadata, ino, id, size, data);
}

static int file_hiding_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_cfs(state->frontend_cfs, cfs);

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

	state->hide_table = vector_create();
	if (!state->hide_table)
		goto error_state;
	state->frontend_cfs = frontend_cfs;
	state->nopen = 0;

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

  error_state:
	free(OBJLOCAL(cfs));
  error_cfs:
	free(cfs);
	return NULL;
}

int file_hiding_cfs_hide(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	int r;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	/* NOTE: hiding ino only prevents future actions from seeing/using ino */

	const int already_hidden = hide_lookup(state->hide_table, ino);
	if (0 <= already_hidden)
		return -E_INVAL;

	hide_entry_t * me = hide_entry_create(ino);
	if (!me)
		return -E_NO_MEM;

	if ((r = vector_push_back(state->hide_table, me)) < 0)
	{
		hide_entry_destroy(me);
		return r;
	}

	kdprintf(STDERR_FILENO, "file_hiding_cfs: hiding %u\n", ino);
	return 0;
}

int file_hiding_cfs_unhide(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	file_hiding_state_t * state = (file_hiding_state_t *) OBJLOCAL(cfs);
	hide_entry_t * me;

	/* make sure this is really a table classifier */
	if (OBJMAGIC(cfs) != FILE_HIDING_MAGIC)
		return -E_INVAL;

	int idx = hide_lookup(state->hide_table, ino);
	if (idx < 0)
		return idx;
	me = vector_elt(state->hide_table, idx);

	kdprintf(STDERR_FILENO,"file_hiding_cfs: unhiding %u\n", ino);
	vector_erase(state->hide_table, idx);
	hide_entry_destroy(me);

	return 0;
}
