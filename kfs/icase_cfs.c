#include <lib/platform.h>
#include <lib/vector.h>
#include <lib/dirent.h>

#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <kfs/icase_cfs.h>

#define ICASE_DEBUG 0

#if ICASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct icase_state {
	CFS_t * frontend_cfs;
};
typedef struct icase_state icase_state_t;

struct icase_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	inode_t ino;
};
typedef struct icase_fdesc icase_fdesc_t;

static int icase_ignore (CFS_t * object, inode_t parent, const char * name, char ** string)
{
	Dprintf("ICASEDEBUG: %s( %s )\n", __FUNCTION__, name);
	uint32_t bp = 0;
	fdesc_t * f = NULL;
	struct dirent dir;
	int r, q;

	r = CALL(object, open, parent, O_RDONLY, &f);
	if (f == NULL)
		return -EINVAL;
	
	r = CALL(object, get_root, &(f->common->parent));
	if(r < 0) {
		q = CALL(object, close, f);
		if(q < 0) return q;
		
		return r;
	}

	do {
		r = CALL(object, get_dirent, f, &dir, sizeof(struct dirent), &bp);
		if (r < 0) {
			r = -ENOENT;
			break;
		}
	} while (strcasecmp(name, dir.d_name) != 0);
	
	q = CALL(object, close, f);
	if (q < 0) return q;
	
	if (r >= 0)
		if ((*string = strdup(dir.d_name)) == NULL)
			r = -ENOMEM;
	return r;
}	

//
// icase_cfs

static int icase_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != ICASE_MAGIC)
		return -EINVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int icase_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != ICASE_MAGIC)
		return -EINVAL;
	snprintf(string, length, "case insensitivity is on!");
	return 0;
}

static int icase_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s()\n", __FUNCTION__);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_root, ino);
}

static int icase_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	int r;

	r =  CALL(state->frontend_cfs, lookup, parent, name, ino);
	if(r == -ENOENT)
	{
		char * actual_name = NULL;
		r = icase_ignore(cfs, parent, name, &(actual_name));
		if(r >= 0) {
			r = CALL(state->frontend_cfs, lookup, parent, (const char *)actual_name, ino);
			free(actual_name);
		}
	}
	return r;
}

static int icase_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, open, ino, mode, fdesc);
}

static int icase_create(CFS_t * cfs, inode_t parent, const char * name, int mode, const metadata_set_t * initialmd, fdesc_t ** fdesc, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, create, parent, name, mode, initialmd, fdesc, ino);
}

static int icase_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s\n", __FUNCTION__);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, close, fdesc);
}

static int icase_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t ofhfset, uint32_t size)
{
	Dprintf("%s\n", __FUNCTION__);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, read, fdesc, data, ofhfset, size);
}

static int icase_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t ofhfset, uint32_t size)
{
	Dprintf("%s\n", __FUNCTION__);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, write, fdesc, data, ofhfset, size);
}

static int icase_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s\n", __FUNCTION__);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_dirent, fdesc, entry, size, basep);
}

static int icase_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t size)
{
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, truncate, fdesc, size);
}

static int icase_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	int r;

	r = CALL(state->frontend_cfs, unlink, parent, name);
	
	if(r == -ENOENT)
	{
		char * actual_name = NULL;
		r = icase_ignore(cfs, parent, name, &(actual_name));
		if(r >= 0) {
			r = CALL(state->frontend_cfs, unlink, parent, actual_name);
			free(actual_name);
		}
	}
	return r;
}

static int icase_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, ino, newparent, newname);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, link, ino, newparent, newname);
}

/*
 * Whats wrong with rename right now? If you rename an existing file with its original case 
 * filename to a file which already exists but with a differing case filename, then you get
 * two files which have case insensitivly equivalent filenames.
 * $ls
 * apple orange
 * $mv apple Orange
 * $ls
 * Orange apple orange
 * This is not desired behavior the unlink call in uhfs needs to somehow be aware of case
 * insensitivity. Also we'd like mv apple Apple to remove apple and create Apple but currently
 * this does not work either for the same reason as above.
 */

static int icase_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, \"%s\", %u, \"%s\")\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	int r, q;

	r = CALL(state->frontend_cfs, rename, oldparent, oldname, newparent, newname);

	if (r == -ENOENT) {
		char * actual_newname = NULL;
		char * actual_oldname = NULL;

		r = icase_ignore(cfs, oldparent, oldname, &(actual_oldname));
		q = icase_ignore(cfs, newparent, newname, &(actual_newname));	
		
		if ((r < 0) || (q < 0 && q != -ENOENT))
			return (r < 0) ? r : q;

		if((r >= 0 && q >= 0) || q == -ENOENT )
			actual_newname = (char *)newname;

		r = CALL(state->frontend_cfs, rename, oldparent, actual_oldname, newparent, actual_newname);

		if(r >= 0)
			free(actual_oldname);
		if(q >= 0)
			free(actual_newname);
	}
	return r;
}

static int icase_mkdir(CFS_t * cfs, inode_t parent, const char * name, const metadata_set_t * initialmd, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, mkdir, parent, name, initialmd, ino);
}

static int icase_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	int r;

	r = CALL(state->frontend_cfs, rmdir, parent, name);
	
	if(r == -ENOENT)
	{
		char * actual_name = NULL;
		r = icase_ignore(cfs, parent, name, &(actual_name));
		if(r >= 0) {
			r = CALL(state->frontend_cfs, rmdir, parent, actual_name);
			free(actual_name);
		}
	}
	return r;
}

static size_t icase_get_num_features(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_num_features, ino);
}

static const feature_t * icase_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, num);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_feature, ino, num);
}

static int icase_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, id);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, get_metadata, ino, id, size, data);
}

static int icase_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(%u, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, ino, id, size, (signed int)data);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	return CALL(state->frontend_cfs, set_metadata, ino, id, size, data);
}

static int icase_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, (signed int)cfs);
	icase_state_t * state = (icase_state_t *) OBJLOCAL(cfs);
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_cfs(state->frontend_cfs, cfs);

	free(state);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * icase_cfs(CFS_t * frontend_cfs)
{
	icase_state_t * state;
	CFS_t * cfs;

	if (!frontend_cfs)
		return NULL;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;

	state = malloc(sizeof(*state));
	if (!state)
		goto error_cfs;

	CFS_INIT(cfs, icase, state);
	OBJMAGIC(cfs) = ICASE_MAGIC;

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

  error_cfs:
	free(cfs);
	return NULL;
}
