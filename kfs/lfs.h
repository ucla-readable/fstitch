#ifndef __KUDOS_KFS_LFS_H
#define __KUDOS_KFS_LFS_H

#include <inc/types.h>

#include <kfs/oo.h>

#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/fdesc.h>
#include <kfs/feature.h>
#include <kfs/dirent.h>

/* FIXME add change descriptors */

struct LFS;
typedef struct LFS LFS_t;

#define TYPE_FILE 0
#define TYPE_DIR 1
#define TYPE_SYMLINK 2

/* Ideally, LFS wouldn't have any calls that weren't directly related to blocks.
 * However, the on-disk structure of directory files is a part of the specification
 * of the filesystem. So we have to handle it inside the LFS module. Thus a few of
 * the calls below (like "get_dirent") seem a little bit higher-level than you would
 * otherwise expect from such a low-level interface. */

struct LFS {
	DESTRUCTOR(LFS_t);
	DECLARE(LFS_t, uint32_t, get_blocksize);
	DECLARE(LFS_t, bdesc_t *, allocate_block, uint32_t size, int purpose);
	DECLARE(LFS_t, bdesc_t *, lookup_block, uint32_t number, uint32_t offset, uint32_t size);
	DECLARE(LFS_t, fdesc_t *, lookup_name, const char * name);
	DECLARE(LFS_t, void, free_fdesc, fdesc_t * fdesc);
	DECLARE(LFS_t, bdesc_t *, get_file_block, fdesc_t * file, uint32_t offset);
	DECLARE(LFS_t, int, get_dirent, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep);
	DECLARE(LFS_t, int, append_file_block, fdesc_t * file, bdesc_t * block);
	DECLARE(LFS_t, fdesc_t *, allocate_name, char * name, uint8_t type, fdesc_t * link);
	DECLARE(LFS_t, int, rename, const char * oldname, const char * newname);
	DECLARE(LFS_t, bdesc_t *, truncate_file_block, fdesc_t * file);
	DECLARE(LFS_t, int, free_block, bdesc_t * block);
	DECLARE(LFS_t, int, apply_changes, chdesc_t * changes);
	DECLARE(LFS_t, int, remove_name, const char * name);
	DECLARE(LFS_t, int, write_block, bdesc_t * block, uint32_t offset, uint32_t size, void * data);
	DECLARE(LFS_t, size_t, get_num_features, const char * name);
	DECLARE(LFS_t, const feature_t *, get_feature, const char * name, size_t num);
	DECLARE(LFS_t, int, get_metadata, const char * name, uint32_t id, size_t * size, void ** data);
	DECLARE(LFS_t, int, set_metadata, const char * name, uint32_t id, size_t size, const void * data);
	DECLARE(LFS_t, int, sync, const char * name);
	void * instance;
};

#endif /* __KUDOS_KFS_LFS_H */
