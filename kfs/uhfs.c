#include <inc/lib.h>
#include <inc/malloc.h>

#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h>

#define UHFS_DEBUG 1


#if UHFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define FIDX(fid) ((uint32_t) (fid) & ~0xFFF)


struct open_file {
	int fid;
	void * page;
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

static void open_file_close(LFS_t * lfs, open_file_t * f)
{
	sys_page_unmap(0, f->page);
	CALL(lfs, free_fdesc, f->fdesc);
	f->page = NULL;
	f->fdesc = NULL;
}

// Scan through f[] and close f's no longer in use by other envs
static void open_file_gc(LFS_t * lfs, struct open_file f[])
{
	size_t i;
	for (i=0; i < UHFS_MAX_OPEN; i++)
	{
		if (!f->page)
			continue;

		if (((struct Page*) UPAGES)[PTX(f->page )].pp_ref == 1)
			open_file_close(lfs, f);
	}
}


static int uhfs_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	uint8_t * cache;
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
	
	/* remap the client's page */
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
	Dprintf("%s()\n", __FUNCTION__);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;

	if (0 < FIDX(fid) || FIDX(fid) >= UHFS_MAX_OPEN)
		return -E_INVAL;

	f = &state->open_file[FIDX(fid)];
	if (!f->page)
		return -E_INVAL;
	assert(f->fdesc);


	open_file_close(state->lfs, f);
	
	return 0;
}

static int uhfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s()\n", __FUNCTION__);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = blockoffset;
	bdesc_t * bd;
	uint32_t size_read = 0;

	if (0 < FIDX(fid) || FIDX(fid) >= UHFS_MAX_OPEN)
		return -E_INVAL;

	f = &state->open_file[FIDX(fid)];
	if (!f->page)
		return -E_INVAL;
	assert(f->fdesc);


	while (size_read < size)
	{
		bd = CALL(state->lfs, get_file_block, f->fdesc, blockoffset + size_read);
		if (!bd)
			return size_read;

		memcpy((uint8_t*)data + size_read, bd->ddesc->data + dataoffset, bd->length - dataoffset);
		dataoffset = 0; /* dataoffset only needed for first block */
		size_read += bd->length;

		bdesc_drop(&bd);
	}

	return size_read;
}

static int uhfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
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
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static size_t uhfs_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s()\n", __FUNCTION__);
	return 0;
}

static const feature_t * uhfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("%s()\n", __FUNCTION__);
	return NULL;
}

static int uhfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
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
