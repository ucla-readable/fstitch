#include <inc/types.h>
#include <kfs/oo.h>

struct feature;

struct CFS;
typedef struct CFS CFS_t;

struct CFS {
	DESTRUCTOR(CFS_t);
	DECLARE(CFS_t, int, open, const char * name, int mode);
	DECLARE(CFS_t, int, close, int fid);
	DECLARE(CFS_t, int, read, int fid, void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, write, int fid, void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, truncate, int fid, uint32_t size);
	DECLARE(CFS_t, int, unlink, const char * name);
	DECLARE(CFS_t, int, link, const char * oldname, const char * newname);
	DECLARE(CFS_t, int, rename, const char * oldname, const char * newname);
	DECLARE(CFS_t, int, mkdir, const char * name);
	DECLARE(CFS_t, int, rmdir, const char * name);
	DECLARE(CFS_t, const struct feature *, get_features);
	DECLARE(CFS_t, int, get_metadata, int fid, uint32_t id, ...);
	DECLARE(CFS_t, int, set_metadata, int fid, uint32_t id, ...);
	DECLARE(CFS_t, int, sync, int fid);
	void * instance;
};
