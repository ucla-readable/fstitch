#ifndef __KUDOS_KFS_CFS_H
#define __KUDOS_KFS_CFS_H

#include <inc/types.h>

#include <kfs/oo.h>
#include <kfs/feature.h>

struct CFS;
typedef struct CFS CFS_t;

struct CFS {
	DESTRUCTOR(CFS_t);
	DECLARE(CFS_t, int, open, const char * name, int mode);
	DECLARE(CFS_t, int, close, int fid);
	DECLARE(CFS_t, int, read, int fid, void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, write, int fid, const void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, truncate, int fid, uint32_t size);
	DECLARE(CFS_t, int, unlink, const char * name);
	DECLARE(CFS_t, int, link, const char * oldname, const char * newname);
	DECLARE(CFS_t, int, rename, const char * oldname, const char * newname);
	DECLARE(CFS_t, int, mkdir, const char * name);
	DECLARE(CFS_t, int, rmdir, const char * name);
	DECLARE(CFS_t, size_t, get_num_features, const char * name);
	DECLARE(CFS_t, const feature_t *, get_feature, const char * name, size_t num);
	DECLARE(CFS_t, int, get_metadata, const char * name, uint32_t id, size_t * size, void * data);
	DECLARE(CFS_t, int, set_metadata, const char * name, uint32_t id, size_t size, const void * data);
	DECLARE(CFS_t, int, sync, const char * name);
	void * instance;
};

#endif /* __KUDOS_KFS_CFS_H */
