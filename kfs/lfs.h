#ifndef __KUDOS_KFS_LFS_H
#define __KUDOS_KFS_LFS_H

#include <inc/types.h>

#include <kfs/oo.h>

#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/fdesc.h>
#include <kfs/feature.h>

/* FIXME add change descriptors */

struct LFS;
typedef struct LFS LFS_t;

struct LFS {
	DESTRUCTOR(LFS_t);
	DECLARE(LFS_t, bdesc_t *, allocate_block, uint32_t size, int purpose);
	DECLARE(LFS_t, bdesc_t *, lookup_block, uint32_t number, uint32_t offset, uint32_t size);
	DECLARE(LFS_t, fdesc_t *, lookup_name, const char * name);
	DECLARE(LFS_t, bdesc_t *, get_file_block, fdesc_t * file, uint32_t offset);
	DECLARE(LFS_t, int, append_file_block, fdesc_t * file, bdesc_t * block);
	DECLARE(LFS_t, fdesc_t *, allocate_name, char * name, int type, fdesc_t * link);
	DECLARE(LFS_t, int, rename, const char * oldname, const char * newname);
	DECLARE(LFS_t, bdesc_t *, truncate_file_block, fdesc_t * file);
	DECLARE(LFS_t, int, free_block, bdesc_t * block);
	DECLARE(LFS_t, int, apply_changes, chdesc_t * changes);
	DECLARE(LFS_t, int, remove_name, const char * name);
	DECLARE(LFS_t, int, write_block, bdesc_t * block, uint32_t offset, uint32_t size, void * data);
	DECLARE(LFS_t, const feature_t *, get_features);
	DECLARE(LFS_t, int, get_metadata, fdesc_t * file, uint32_t id, size_t * size, void * data);
	DECLARE(LFS_t, int, set_metadata, fdesc_t * file, uint32_t id, size_t size, const void * data);
	DECLARE(LFS_t, int, sync, fdesc_t * file);
	void * instance;
};

#endif /* __KUDOS_KFS_LFS_H */
