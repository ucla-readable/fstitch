// jos fs server access through the cfs interface.
// Derived from uhfs.c.

#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/fd.h>

#include <kfs/chdesc.h>
#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h> // for UHFS' limits and ranges
#include <kfs/josfs_cfs.h>


#define JOSFS_CFS_DEBUG 0


#if JOSFS_CFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct open_file {
	int fid;
	struct Fd * page;
	int fd;
};
typedef struct open_file open_file_t;

struct josfs_cfs_state {
	open_file_t open_file[UHFS_MAX_OPEN];
};
typedef struct josfs_cfs_state josfs_cfs_state_t;


/* Is this virtual address mapped? */
static int va_is_mapped(void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}


static int open_file_free(open_file_t * f)
{
	int r;
	if ((r = close(f->fd)) < 0)
		return r;

	sys_page_unmap(0, (void*) f->page);
	f->page = NULL;
	/* we do not reset fid here on purpose */
	f->fd = -1;
	return 0;
}

/* returns 0 if it is closed in all clients, 1 if it is still open somewhere */
static int open_file_close(open_file_t * f)
{
	if (!f->page)
		return -E_INVAL;
	if (pageref(f->page) == 1)
		return open_file_free(f);
	return 1;
}

// Scan through f[] and close f's no longer in use by other envs
static void open_file_gc(open_file_t f[])
{
	size_t i;
	for (i = 0; i < UHFS_MAX_OPEN; i++)
		open_file_close(&f[i]);
}

static int fid_idx(int fid, open_file_t f[])
{
	uint32_t ufid = fid;
	struct Fd * fd;
	int idx;

	if ((uint32_t)UHFS_FD_MAP >> 31)
		ufid |= 0x80000000;
	fd = (struct Fd *) (ufid & ~(PGSIZE - 1));
	if (!va_is_mapped(fd))
		return -E_INVAL;

	idx = fd->fd_kpl.index;
	if (idx <0 || UHFS_MAX_OPEN <= idx)
		return -E_INVAL;

	if (f[idx].fid != fid)
		return -E_INVAL;

	assert(f[idx].page && f[idx].fid);

	return idx;
}



static int josfs_cfs_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	Dprintf("%s(\"%s\", %d, 0x%x)\n", __FUNCTION__, name, mode, page);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	void * cache;
	int r, index;
	
	open_file_gc(state->open_file);

	/* find an available index */
	for(index = 0; index != UHFS_MAX_OPEN; index++)
		if(!state->open_file[index].page)
			break;
	if(index == UHFS_MAX_OPEN)
		return -E_MAX_OPEN;
	
	/* find a free page */
	for(cache = UHFS_FD_MAP; cache != UHFS_FD_END; cache += PGSIZE)
		if(!va_is_mapped(cache))
			break;
	if(cache == UHFS_FD_END)
		return -E_MAX_OPEN;
	
	/* store the index in the client's page */
	((struct Fd *) page)->fd_kpl.index = index;

	/* remap the client's page read-only in its new home */
	r = sys_page_map(0, page, 0, cache, PTE_U | PTE_P);
	if(r < 0)
		return r;
	sys_page_unmap(0, page);
	
	/* now look up the name */
	r = jfs_open(name, mode);
	if (r < 0)
	{
		sys_page_unmap(0, cache);
		return r;
	}
	state->open_file[index].fd = r;
	
	/* good to go, save the client page... */
	state->open_file[index].page = cache;
	
	/* ...and make up a new ID */
	r = state->open_file[index].fid + 1;
	r &= PGSIZE - 1;
	r |= 0x7FFFFFFF & (int) cache;
	state->open_file[index].fid = r;

	return r;
}

static int josfs_cfs_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, fid);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int idx;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;

	return open_file_close(&state->open_file[idx]);
}

static int josfs_cfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, 0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int idx;
	int fd;
	int r;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	fd = state->open_file[idx].fd;

	if ((r = seek(fd, offset)) < 0)
		return r;
	return read(fd, data, size);
}

static int josfs_cfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int idx;
	int fd;
	int r;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	fd = state->open_file[idx].fd;

	if ((r = seek(fd, offset)) < 0)
		return r;
	return write(fd, data, size);
}

static int josfs_cfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep, offset);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	int idx;
	int fd;
	int i, j, k;
	int nbytes_read = 0;
	dirent_t ent;
	int r = 0;
	struct File f;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	fd = state->open_file[idx].fd;

	if ((r = seek(fd, *basep)) < 0)
		goto exit;

	for (i=0; nbytes_read < nbytes; i++)
	{
		// Read a dirent
		if ((r = read(fd, &f, sizeof(struct File))) < 0)
			goto exit;

		// Pseudo unique fileno generator
		ent.d_fileno = 0;
		k = 1;
		const int f_name_len = strlen(f.f_name);
		for (j = 0; j < f_name_len; j++)
		{
			ent.d_fileno += j * k;
			k = k * 2;
		}

		// Store the dirent into ent
		ent.d_type = f.f_type;
		ent.d_reclen = sizeof(ent.d_fileno) + sizeof(ent.d_type) + sizeof(ent.d_reclen) + sizeof(ent.d_namelen) + strlen(f.f_name) + 1;
		ent.d_namelen = MIN(strlen(f.f_name), sizeof(ent.d_name));
		strncpy(ent.d_name, f.f_name, MIN(ent.d_namelen+1, sizeof(ent.d_name)));

		// Store the dirent ent into the buffer
		if (ent.d_reclen > nbytes_read - nbytes)
			break;
		memcpy(buf, &ent, ent.d_reclen);

		nbytes_read += ent.d_reclen;
		buf += ent.d_reclen;
		*basep += sizeof(struct File);
	}

  exit:
	if (!nbytes || nbytes_read > 0)
		return nbytes_read;
	else
		return r;
}

static int josfs_cfs_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	Dprintf("%s(%d, 0x%x)\n", __FUNCTION__, fid, target_size);
	josfs_cfs_state_t * state = (josfs_cfs_state_t *) cfs->instance;
	const int file_idx = fid_idx(fid, state->open_file);
	int fd;

	if (file_idx < 0)
		return file_idx;
	fd = state->open_file[file_idx].fd;

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
	ASSIGN_DESTROY(cfs, josfs_cfs, destroy);

	memset(state->open_file, 0, sizeof(state->open_file));

	return cfs;

 error_josfs_cfs:
	free(cfs);
	return NULL;
}
