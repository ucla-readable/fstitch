#include <inc/lib.h>
#include <inc/malloc.h>

#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h>

#define MAX_OPEN 256

struct uhfs_state {
	struct {
		int fid;
		void * page;
		fdesc_t * fdesc;
	} open_file[MAX_OPEN];
};

static int uhfs_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_close(CFS_t * cfs, int fid)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep, uint32_t offset)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_unlink(CFS_t * cfs, const char * name)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_mkdir(CFS_t * cfs, const char * name)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_rmdir(CFS_t * cfs, const char * name)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static size_t uhfs_get_num_features(CFS_t * cfs, const char * name)
{
	printf("%s()\n", __FUNCTION__);
	return 0;
}

static const feature_t * uhfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	printf("%s()\n", __FUNCTION__);
	return NULL;
}

static int uhfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_sync(CFS_t * cfs, const char * name)
{
	printf("%s()\n", __FUNCTION__);
	return -E_UNSPECIFIED;
}

static int uhfs_destroy(CFS_t * cfs)
{
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

CFS_t * uhfs(void)
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
	
	return cfs;
	
 error_uhfs:
	free(cfs);
	return NULL;
}
