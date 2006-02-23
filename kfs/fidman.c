#include <inc/lib.h> // sys_page_map()
#include <lib/types.h>
#include <assert.h>
#include <lib/panic.h>
#include <inc/error.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/fdesc.h>
#include <kfs/fidman.h>


#define FIDMAN_DEBUG 0

#if FIDMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


typedef uint8_t fid_entry_t;
static fid_entry_t fid_table[MAX_OPEN_FIDS] = {0};


struct fid_info {
	fdesc_t * fdesc;
	const struct Fd * page; // track fid closes
	uint32_t cappa; // track fid permission
};
typedef struct fid_info fid_info_t;

fid_info_t fid_info_table[MAX_OPEN_FIDS]
	= {{.fdesc = NULL, .page = NULL, .cappa = 0}};


static size_t last_fid_tbl_idx = 0;

#define TIME_UNIQ_BITS (8*sizeof(fid_entry_t) - 1)


static void __local_assert(void)
{
	(void) &__local_assert; // silence the compiler's warning of fn's non-use
	static_assert(MAX_OPEN_FIDS <= (1 << (8*sizeof(int) - 1 - TIME_UNIQ_BITS)));
}


//
// fid table maintenance

static bool fid_entry_is_inuse(int fid_tbl_idx)
{
	return fid_table[fid_tbl_idx] >> TIME_UNIQ_BITS;
}

static void mark_fid_entry_used(int fid_tbl_idx, fdesc_t * fdesc, const struct Fd * page, uint32_t cappa)
{
	assert(!fid_entry_is_inuse(fid_tbl_idx));
	fid_table[fid_tbl_idx]++;
	fid_table[fid_tbl_idx] |= 1 << TIME_UNIQ_BITS;
	fid_info_table[fid_tbl_idx].fdesc = fdesc;
	fid_info_table[fid_tbl_idx].page = page;
	fid_info_table[fid_tbl_idx].cappa = cappa;
}

static void mark_fid_entry_empty(int fid_tbl_idx)
{
	assert(fid_entry_is_inuse(fid_tbl_idx));
	fid_table[fid_tbl_idx] &= ~(1 << TIME_UNIQ_BITS);
	fid_info_table[fid_tbl_idx].fdesc = NULL;
	fid_info_table[fid_tbl_idx].page = NULL;
	fid_info_table[fid_tbl_idx].cappa = -1;
}

static int fidtableidx2fid(int fid_tbl_idx)
{
	return ((fid_tbl_idx << TIME_UNIQ_BITS)
			| (fid_table[fid_tbl_idx] & ~(1 << TIME_UNIQ_BITS)));
}

static int fid2fidtableidx(int fid)
{
	if (fid < 0)
		return -E_INVAL;
	return fid >> TIME_UNIQ_BITS;
}

//
// Check helper functions

// fid protect

// Check that the given open file matches the last received ipc capability,
// ensuring that no env's request is able to pass through unless they
// have the Fd page for the request fid.
static int check_capability(fid_info_t * fid_info)
{
	uint32_t cappa = cfs_ipc_serve_cur_cappa();
	if (cappa != fid_info->cappa && cappa)
	{
		kdprintf(STDERR_FILENO, "fidman %s: FAILURE: cappa = 0x%08x, request's cappa = 0x%08x.\n", __FUNCTION__, fid_info->cappa, cappa);
		return -E_IPC_FAILED_CAP;
	}

	return 0;
}


// fid close

static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

static int create_fd_page(const struct Fd ** fd_page)
{
	const void * cap_page;
	const void * cache;
	int r;

	assert(fd_page);

	cap_page = cfs_ipc_serve_cur_page();
	assert(cap_page && va_is_mapped(cap_page));

	// find a free slot to cache cap_page
	for(cache = FIDMAN_FD_MAP; cache != FIDMAN_FD_END; cache += PGSIZE)
		if(!va_is_mapped(cache))
			break;
	if(cache == FIDMAN_FD_END)
		return -E_MAX_OPEN;

	// remap the client's page to fid close's cache
	r = sys_page_map(0, (void*) cap_page, 0, (void*) cache, PTE_U | PTE_P);
	if(r < 0)
		return r;

	*fd_page = cache;
	return 0;
}

static bool fid_is_closeable(int fid)
{
	int idx;
	fid_info_t * fid_info;

	idx = fid2fidtableidx(fid);
	if (idx < 0 || idx > MAX_OPEN_FIDS || !fid_entry_is_inuse(fid2fidtableidx(fid)))
		return 0;
	fid_info = &fid_info_table[idx];

	assert(fid_info->page);
	assert(pageref(fid_info->page) >= 1);

	return (pageref(fid_info->page) <= 1);
}

//
// Public interface

void gc_fids(void)
{
	uint32_t cur_cappa;
	size_t idx;
	int r;

	cur_cappa = cfs_ipc_serve_cur_cappa();
	cfs_ipc_serve_set_cur_cappa(0);

	for (idx = 0; idx < MAX_OPEN_FIDS; idx++)
	{
		fid_info_t * fid_info;
		if (!fid_entry_is_inuse(idx))
			continue;
		fid_info = &fid_info_table[idx];

		assert(fid_info->page && va_is_mapped(fid_info->page));

		r = release_fid(fidtableidx2fid(idx));
		if (r < 0 && r != -E_BUSY)
			kdprintf(STDERR_FILENO, "fidman gc: release_fid: %i\n", r);
	}

	cfs_ipc_serve_set_cur_cappa(cur_cappa);
}

int create_fid(fdesc_t * fdesc)
{
	uint32_t cappa;
	const struct Fd * fd_page;
	size_t idx, count;
	int r;

	cappa = cfs_ipc_serve_cur_cappa();
	if (cappa == -1)
		kdprintf(STDERR_FILENO, "%s(): warning: capability is the unused-marker\n", __FUNCTION__);

	gc_fids();

	for (idx=++last_fid_tbl_idx, count=0; count<MAX_OPEN_FIDS; idx++, count++)
	{
		idx %= MAX_OPEN_FIDS;
		if (!fid_entry_is_inuse(idx))
		{
			if ((r = create_fd_page(&fd_page)) < 0)
				return r;
			mark_fid_entry_used(idx, fdesc, fd_page, cappa);
			last_fid_tbl_idx = idx;
			Dprintf("%s() returning %d\n", __FUNCTION__, fidtableidx2fid(idx));
			return fidtableidx2fid(idx);
		}
	}

	Dprintf("%s() returning -E_MAX_OPEN\n", __FUNCTION__);
	return -E_MAX_OPEN;
}

int release_fid(int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	int idx;
	fid_info_t * fid_info;
	int r;

	idx = fid2fidtableidx(fid);
	if (idx < 0 || idx > MAX_OPEN_FIDS || !fid_entry_is_inuse(fid2fidtableidx(fid)))
		return -E_INVAL;
	fid_info = &fid_info_table[idx];

	if (!fid_is_closeable(fid))
		return -E_BUSY;

	// fid close
	assert(fid_info->page && va_is_mapped(fid_info->page));
	r = sys_page_unmap(0, (void *) fid_info->page);
	if (r < 0)
		kdprintf(STDERR_FILENO, "%s: sys_page_unmap(0, 0x%08x): %i\n", __FUNCTION__, fid_info->page, r);
	assert(r >= 0);

	mark_fid_entry_empty(idx);
	return 0;
}

int fid_fdesc(int fid, fdesc_t ** fdesc)
{
	int idx;
	fid_info_t * fid_info;
	int r;

	*fdesc = NULL;

	idx = fid2fidtableidx(fid);
	if (idx < 0 || idx > MAX_OPEN_FIDS || !fid_entry_is_inuse(fid2fidtableidx(fid)))
	{
		Dprintf("%s(%d): invalid fid\n", __FUNCTION__, fid);
		return -E_INVAL;
	}
	fid_info = &fid_info_table[idx];

	if ((r = check_capability(fid_info)) < 0)
	{
		Dprintf("%s(%d): capability check failed\n", __FUNCTION__, fid);
		return r;
	}

	*fdesc = fid_info->fdesc;
	Dprintf("%s(%d) returning fdesc 0x%08x\n", __FUNCTION__, fid, *fdesc);
	return 0;
}

bool fid_closeable_fdesc(int fid, fdesc_t ** fdesc)
{
	int idx;

	if (!fid_is_closeable(fid))
	{
		*fdesc = NULL;
		return 0;
	}

	idx = fid2fidtableidx(fid);
	assert(!(idx < 0 || idx > MAX_OPEN_FIDS || !fid_entry_is_inuse(fid2fidtableidx(fid))));
	*fdesc = fid_info_table[idx].fdesc;
	return 1;
}
