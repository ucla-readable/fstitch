#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/fd.h>

#include <kfs/chdesc.h>
#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h>

#define UHFS_DEBUG 1


#if UHFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct open_file {
	int fid;
	struct Fd * page;
	fdesc_t * fdesc;
};
typedef struct open_file open_file_t;

struct uhfs_state {
	LFS_t * lfs;
	open_file_t open_file[UHFS_MAX_OPEN];
};


/* Is this virtual address mapped? */
static int va_is_mapped(void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

static int open_file_free(LFS_t * lfs, open_file_t * f)
{
	sys_page_unmap(0, (void*) f->page);
	CALL(lfs, free_fdesc, f->fdesc);
	f->page = NULL;
	f->fdesc = NULL;
	return 0;
}

/* returns 0 if it is closed in all clients, 1 if it is still open somewhere */
static int open_file_close(LFS_t * lfs, open_file_t * f)
{
	if (!f->page)
		return -E_INVAL;
	if (pageref(f->page) == 1)
		return open_file_free(lfs, f);
	return 1;
}

// Scan through f[] and close f's no longer in use by other envs
static void open_file_gc(LFS_t * lfs, open_file_t f[])
{
	size_t i;
	for (i = 0; i < UHFS_MAX_OPEN; i++)
		open_file_close(lfs, &f[i]);
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


static int uhfs_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	Dprintf("%s(\"%s\", %d, 0x%x)\n", __FUNCTION__, name, mode, page);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	void * cache;
	int r, index;
	
	open_file_gc(state->lfs, state->open_file);

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
	state->open_file[index].fdesc = CALL(state->lfs, lookup_name, name);
	if(!state->open_file[index].fdesc)
	{
		sys_page_unmap(0, cache);
		return -E_NOT_FOUND;
	}
	
	/* good to go, save the client page... */
	state->open_file[index].page = cache;
	
	/* ...and make up a new ID */
	r = state->open_file[index].fid + 1;
	r &= PGSIZE - 1;
	r |= 0x7FFFFFFF & (int) cache;
	state->open_file[index].fid = r;

	return r;
}

static int uhfs_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, fid);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	int idx;
	open_file_t * f;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	f = &state->open_file[idx];

	return open_file_close(state->lfs, f);
}

static int uhfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, 0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	int idx;
	open_file_t * f;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	bdesc_t * bd;
	uint32_t size_read = 0;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	f = &state->open_file[idx];

	while (size_read < size)
	{
		bd = CALL(state->lfs, get_file_block, f->fdesc, blockoffset + (offset % blocksize) - dataoffset + size_read);
		if (!bd)
			return size_read;

		const uint32_t n = MIN(bd->length - dataoffset, size - size_read);
		memcpy((uint8_t*)data + size_read, bd->ddesc->data + dataoffset, n);
		size_read += n;
		dataoffset = 0; /* dataoffset only needed for first block */

		bdesc_drop(&bd);
	}

	return size_read;
}

static int uhfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	int idx;
	open_file_t * f;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	bdesc_t * bd;
	uint32_t size_written = 0;
	chdesc_t * head, * tail;
	int r;

	if ((idx = fid_idx(fid, state->open_file)) < 0)
		return idx;
	f = &state->open_file[idx];

	while (size_written < size)
	{
		/* get the block to write to */
		bd = CALL(state->lfs, get_file_block, f->fdesc, blockoffset + (offset % blocksize) - dataoffset + size_written);
		if (!bd)
			return size_written;

		/* write the data to the block */
		head = NULL;
		tail = NULL;
		const uint32_t n = MIN(bd->length - dataoffset, size - size_written);
		r = CALL(state->lfs, write_block, bd, dataoffset, n, (uint8_t*)data + size_written, &head, &tail);
		if (r < 0)
			return size_written;
		size_written += n;
		dataoffset = 0; /* dataoffset only needed for first block */

		/* no need to do anything with the chdescs */
	}

	return size_written;
}

static int uhfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep, uint32_t offset)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return -E_UNSPECIFIED;
}

static int uhfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	return -E_UNSPECIFIED;
}

static int uhfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	return -E_UNSPECIFIED;
}

static int uhfs_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return -E_UNSPECIFIED;
}

static int uhfs_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return -E_UNSPECIFIED;
}

static size_t uhfs_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, get_num_features, name);
}

static const feature_t * uhfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

   return CALL(state->lfs, get_feature, name, num);
}

static int uhfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, get_metadata_name, name, id, size, data);
}

static int uhfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	chdesc_t * head = NULL, * tail = NULL;
	int r;

	r = CALL(state->lfs, set_metadata_name, name, id, size, data, &head, &tail);
	if (r < 0)
		return r;

	/* no need to do anything with the chdescs */

	return r;
}

static int uhfs_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, sync, name);
}

static int uhfs_destroy(CFS_t * cfs)
{
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * uhfs(LFS_t * lfs)
{
	struct uhfs_state * state;
	CFS_t * cfs;
	
	cfs = malloc(sizeof(*cfs));
	if(!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if(!state)
		goto error_uhfs;
	cfs->instance = state;
	
	ASSIGN(cfs, uhfs, open);
	ASSIGN(cfs, uhfs, close);
	ASSIGN(cfs, uhfs, read);
	ASSIGN(cfs, uhfs, write);
	ASSIGN(cfs, uhfs, getdirentries);
	ASSIGN(cfs, uhfs, truncate);
	ASSIGN(cfs, uhfs, unlink);
	ASSIGN(cfs, uhfs, link);
	ASSIGN(cfs, uhfs, rename);
	ASSIGN(cfs, uhfs, mkdir);
	ASSIGN(cfs, uhfs, rmdir);
	ASSIGN(cfs, uhfs, get_num_features);
	ASSIGN(cfs, uhfs, get_feature);
	ASSIGN(cfs, uhfs, get_metadata);
	ASSIGN(cfs, uhfs, set_metadata);
	ASSIGN(cfs, uhfs, sync);
	ASSIGN_DESTROY(cfs, uhfs, destroy);
	
	state->lfs = lfs;
	memset(state->open_file, 0, sizeof(state->open_file));
	
	return cfs;
	
 error_uhfs:
	free(cfs);
	return NULL;
}
