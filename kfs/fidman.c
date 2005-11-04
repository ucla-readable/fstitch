#include <lib/types.h>
#include <assert.h>
#include <inc/error.h>
#include <kfs/fidman.h>


#define FIDMAN_DEBUG 0

#if FIDMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


typedef uint8_t fid_entry_t;
static fid_entry_t fid_table[MAX_OPEN_FIDS] = {0};

static size_t last_fid_tbl_idx = 0;

#define TIME_UNIQ_BITS (8*sizeof(fid_entry_t) - 1)


static void __local_assert(void)
{
	(void) &__local_assert; // silence the compiler's warning of fn's non-use
	static_assert(MAX_OPEN_FIDS <= (1 << (8*sizeof(int) - 1 - TIME_UNIQ_BITS)));
}


static bool fid_entry_is_inuse(int fid_tbl_idx)
{
	return fid_table[fid_tbl_idx] >> TIME_UNIQ_BITS;
}

static void mark_fid_entry_usage(int fid_tbl_idx, bool inuse)
{
	if (inuse)
	{
		assert(!fid_entry_is_inuse(fid_tbl_idx));
		fid_table[fid_tbl_idx]++;
		fid_table[fid_tbl_idx] |= 1 << TIME_UNIQ_BITS;
	}
	else
	{
		assert(fid_entry_is_inuse(fid_tbl_idx));
		fid_table[fid_tbl_idx] &= ~(1 << TIME_UNIQ_BITS);
	}
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


int create_fid(void)
{
	size_t idx, count;
	for (idx=++last_fid_tbl_idx, count=0; count<MAX_OPEN_FIDS; idx++, count++)
	{
		idx %= MAX_OPEN_FIDS;
		if (!fid_entry_is_inuse(idx))
		{
			mark_fid_entry_usage(idx, 1);
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
	int idx = fid2fidtableidx(fid);
	if (idx < 0)
		return -E_INVAL;
	mark_fid_entry_usage(idx, 0);
	return 0;
}
