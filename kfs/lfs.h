#ifndef __KUDOS_KFS_LFS_H
#define __KUDOS_KFS_LFS_H

#include <inc/types.h>

#include <kfs/oo.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/fdesc.h>
#include <kfs/feature.h>
#include <kfs/dirent.h>

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

/* The functions below that take "head" and "tail" parameters are mutators and
 * will pass back the changes they make using these pointers. The change
 * descriptor subgraphs they generate will be set up so that the head pointer
 * depends on the whole subgraph, and the tail pointer is depended upon by the
 * whole graph. Thus to make the resulting subgraph depend on something else,
 * make its tail depend on that. To make something else depend on the resulting
 * subgraph, make it depend on its head. */

struct LFS {
	DESTRUCTOR(LFS_t);
	DECLARE(LFS_t, uint32_t, get_blocksize);
	DECLARE(LFS_t, BD_t *, get_blockdev);
	DECLARE(LFS_t, bdesc_t *, allocate_block, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, bdesc_t *, lookup_block, uint32_t number, uint32_t offset, uint32_t size);
	DECLARE(LFS_t, fdesc_t *, lookup_name, const char * name);
	DECLARE(LFS_t, void, free_fdesc, fdesc_t * fdesc);
	DECLARE(LFS_t, uint32_t, get_filesize, fdesc_t * file);
	DECLARE(LFS_t, bdesc_t *, get_file_block, fdesc_t * file, uint32_t offset);
	DECLARE(LFS_t, int, get_dirent, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep);
	DECLARE(LFS_t, int, append_file_block, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, fdesc_t *, allocate_name, char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, int, rename, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, bdesc_t *, truncate_file_block, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, int, free_block, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, int, remove_name, const char * name, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, int, write_block, bdesc_t * block, uint32_t offset, uint32_t size, void * data, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, size_t, get_num_features, const char * name);
	DECLARE(LFS_t, const feature_t *, get_feature, const char * name, size_t num);
	DECLARE(LFS_t, int, get_metadata, const char * name, uint32_t id, size_t * size, void ** data);
	DECLARE(LFS_t, int, set_metadata, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(LFS_t, int, sync, const char * name);
	void * instance;
};

#endif /* __KUDOS_KFS_LFS_H */
