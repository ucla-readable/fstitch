// jos fs server access through the cfs interface.
// Derived from uhfs.c.

#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>
#include <inc/fd.h>

#include <kfs/fidman.h>
#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <inc/dirent.h>
#include <kfs/josfs_cfs.h>


#define JOSFS_CFS_DEBUG 0


#if JOSFS_CFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct open_file {
	int fid;
	int fd;
};
typedef struct open_file open_file_t;

struct josfs_cfs_state {
	hash_map_t * open_files;
};
typedef struct josfs_cfs_state josfs_cfs_state_t;


//
// open_file_t functions

static open_file_t * open_file_create(int fid, int fd)
{
	open_file_t * f = malloc(sizeof(*f));
	if (!f)
		return NULL;

	f->fid = fid;
	f->fd = fd;
	return f;
}

static void open_file_destroy(open_file_t * f)
{
	f->fid = -1;
	f->fd = -1;
	free(f);
}

static int open_file_close(open_file_t * f)
{
	int r;
	if ((r = close(f->fd)) < 0)
		return r;

	r = release_fid(f->fid);
	assert(0 <= r);

	f->fid = -1;
	f->fd = -1;

	open_file_destroy(f);

	return 0;
}



static int josfs_cfs_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int fd, fid;
	open_file_t * f;
	int r;
	
	/* look up the name */
	fd = jfs_open(name, mode);
	if (fd < 0)
		return fd;

	fid = create_fid();
	if (fid < 0)
		return fid;

	f = open_file_create(fid, fd);
	if (!f)
		return -E_NO_MEM;
	r = hash_map_insert(state->open_files, (void*) fid, f);
	if (r < 0)
	{
		open_file_destroy(f);
		return -E_NO_MEM;
	}

	return fid;
}

static int josfs_cfs_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, fid);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	open_file_t * f;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	return open_file_close(f);
}

static int josfs_cfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, 0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	open_file_t * f;
	int fd;
	int r;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;
	fd = f->fd;

	if ((r = seek(fd, offset)) < 0)
		return r;
	return read(fd, data, size);
}

static int josfs_cfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	open_file_t * f;
	int fd;
	int r;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;
	fd = f->fd;

	if ((r = seek(fd, offset)) < 0)
		return r;
	return write(fd, data, size);
}

static int josfs_cfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	open_file_t * f;
	int fd, r;
	int nbytes_read = 0;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;
	fd = f->fd;

	if ((r = seek(fd, *basep)) < 0)
		return r;

	while (nbytes_read < nbytes)
	{
		int i;
		struct File f;
		uint16_t namelen, reclen;
		dirent_t * ent = (dirent_t *) &buf[nbytes_read];

		// Read a dirent
		if ((r = read(fd, &f, sizeof(struct File))) <= 0)
			break;
		assert(r == sizeof(struct File));
		if (!f.f_name[0])
		{
			*basep += sizeof(struct File);
			continue;
		}

		namelen = strlen(f.f_name);
		namelen = MIN(namelen, sizeof(ent->d_name) - 1);
		reclen = sizeof(*ent) - sizeof(ent->d_name) + namelen + 1;
		
		// Make sure it's not too long
		if(nbytes_read + reclen > nbytes)
			break;
		
		// Pseudo unique fileno generator
		ent->d_fileno = 0;
		for (i = 0; f.f_name[i]; i++)
		{
			ent->d_fileno *= 5;
			ent->d_fileno += f.f_name[i];
		}

		// Store the dirent into *ent
		ent->d_filesize = f.f_size;
		ent->d_reclen = reclen;
		ent->d_type = f.f_type;
		ent->d_namelen = namelen;
		strncpy(ent->d_name, f.f_name, sizeof(ent->d_name));

		// Update position variables
		nbytes_read += reclen;
		*basep += sizeof(struct File);
	}

	return nbytes_read ? nbytes_read : (r < 0) ? r : 0;
}

static int josfs_cfs_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	Dprintf("%s(%d, 0x%x)\n", __FUNCTION__, fid, target_size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	open_file_t * f;
	int fd;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;
	fd = f->fd;

	return ftruncate(fd, target_size);
}

static int josfs_cfs_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return jfs_remove(name);
}

// Not supported
static int josfs_cfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	return -E_UNSPECIFIED;
}

static int josfs_cfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	panic("TODO: implement");
	// Either copy the original file and then remove it or rename the file.
	return -E_UNSPECIFIED;
}

static int josfs_cfs_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	int r;

	r = jfs_open(name, O_CREAT | O_MKDIR);
	if (r < 0)
		return r;
	return close(r);
}

static int josfs_cfs_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	panic("TODO: implement");
	// 1. Remove all files and directories in this directory
	// 2. Remove this directory
	return -E_UNSPECIFIED;
}


static const feature_t * josfs_features[] = {&KFS_feature_size, &KFS_feature_filetype};

static size_t josfs_cfs_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);

	return sizeof(josfs_features) / sizeof(josfs_features[0]);
}

static const feature_t * josfs_cfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("\"%s\", 0x%x)\n", __FUNCTION__, name, num);

	if(num < 0 || num >= sizeof(josfs_features) / sizeof(josfs_features[0]))
		return NULL;
	return josfs_features[num];
}

static int josfs_cfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	int fid;
	struct Stat s;
	int r;

	if ((r = fid = jfs_open(name, O_RDONLY)) < 0)
		return r;
	if ((r = fstat(fid, &s)) < 0)
	{
		close(fid);
		return r;
	}
	if ((r = close(fid)) < 0)
		return r;

	if (id == KFS_feature_size.id)
	{
		*data = malloc(sizeof(s.st_size));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(s.st_size);
		**(off_t **) data = s.st_size;
	}
	else if (id == KFS_feature_filetype.id)
	{
		*data = malloc(sizeof(s.st_isdir));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(s.st_isdir);
		**(int **) data = s.st_isdir;
	}
	else
		return -E_INVAL;

	return 0;
}

// Not supported
static int josfs_cfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	return -E_UNSPECIFIED;
}


static int josfs_cfs_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);

	return jfs_sync();
}

static int josfs_cfs_destroy(CFS_t * cfs)
{
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;

	hash_map_destroy(state->open_files);
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * josfs_cfs(void)
{
	josfs_cfs_state_t * state;
	CFS_t * cfs;

	cfs = malloc(sizeof(*cfs));
	if(!cfs)
		return NULL;

	state = malloc(sizeof(*state));
	if(!state)
		goto error_josfs_cfs;
	cfs->instance = state;

	ASSIGN(cfs, josfs_cfs, open);
	ASSIGN(cfs, josfs_cfs, close);
	ASSIGN(cfs, josfs_cfs, read);
	ASSIGN(cfs, josfs_cfs, write);
	ASSIGN(cfs, josfs_cfs, getdirentries);
	ASSIGN(cfs, josfs_cfs, truncate);
	ASSIGN(cfs, josfs_cfs, unlink);
	ASSIGN(cfs, josfs_cfs, link);
	ASSIGN(cfs, josfs_cfs, rename);
	ASSIGN(cfs, josfs_cfs, mkdir);
	ASSIGN(cfs, josfs_cfs, rmdir);
	ASSIGN(cfs, josfs_cfs, get_num_features);
	ASSIGN(cfs, josfs_cfs, get_feature);
	ASSIGN(cfs, josfs_cfs, get_metadata);
	ASSIGN(cfs, josfs_cfs, set_metadata);
	ASSIGN(cfs, josfs_cfs, sync);
	DESTRUCTOR(cfs, josfs_cfs, destroy);

	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	if(modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}

	return cfs;

  error_state:
	free(state);
  error_josfs_cfs:
	free(cfs);
	return NULL;
}
