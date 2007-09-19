/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_LFS_H
#define __FSTITCH_FSCORE_LFS_H

#include <fscore/oo.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>
#include <fscore/fdesc.h>
#include <fscore/feature.h>
#include <fscore/inode.h>
#include <fscore/patchgroup.h>
#include <lib/dirent.h>

/* lfs_add_fork_head() should be called inside an LFS operation for each
 * patch graph fork head not reachable from *head upon return */
#define lfs_add_fork_head(head) patchgroup_finish_head(head)

/* Ideally, LFS wouldn't have any calls that weren't directly related to
 * blocks. However, the on-disk structure of directory files is a part
 * of the specification of the filesystem. So we have to handle it
 * inside the LFS module. Thus a few of the calls below (like
 * "get_dirent") seem a little bit higher-level than you would otherwise
 * expect from such a low-level interface.
 */

/* "head" parameter:
 * - The functions below that take a head parameter are mutators and
 *   will pass back the changes they make using that pointer. The
 *   patch subgraphs they generate will be set up so that
 *   the head pointer depends on the whole subgraph.
 *   To make something depend on the resulting subgraph, make it depend
 *   on the head.
 * - head is both an input and output parameter: if input *head is
 *   non-NULL, the newly created subgraph will be made to depend on it.
 *   Thus to make the resulting subgraph depend on something else, pass
 *   that something else in as *head. If that something else is not
 *   known ahead of time, create a null patch, claim it,
 *   and pass it in as *head. Afterwards, hook up the null change
 *   descriptor appropriately and unclaim it.
 */

struct LFS {
	OBJECT(LFS_t);
	DECLARE(LFS_t, int, get_root, inode_t * ino);
	uint16_t blocksize;
	BD_t * blockdev;
	DECLARE(LFS_t, uint32_t, allocate_block, fdesc_t * file, int purpose, patch_t ** head);
	DECLARE(LFS_t, bdesc_t *, lookup_block, uint32_t number, page_t * page);
	DECLARE(LFS_t, bdesc_t *, synthetic_lookup_block, uint32_t number, page_t * page);
	DECLARE(LFS_t, fdesc_t *, lookup_inode, inode_t ino);
	DECLARE(LFS_t, int, lookup_name, inode_t parent, const char * name, inode_t * ino);
	DECLARE(LFS_t, void, free_fdesc, fdesc_t * fdesc);
	DECLARE(LFS_t, uint32_t, get_file_numblocks, fdesc_t * file);
	DECLARE(LFS_t, uint32_t, get_file_block, fdesc_t * file, uint32_t offset);
	DECLARE(LFS_t, int, get_dirent, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
	DECLARE(LFS_t, int, append_file_block, fdesc_t * file, uint32_t block, patch_t ** head);
	DECLARE(LFS_t, fdesc_t *, allocate_name, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initial_metadata, inode_t * newino, patch_t ** head);
	DECLARE(LFS_t, int, rename, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head);
	DECLARE(LFS_t, uint32_t, truncate_file_block, fdesc_t * file, patch_t ** head);
	DECLARE(LFS_t, int, free_block, fdesc_t * file, uint32_t block, patch_t ** head);
	DECLARE(LFS_t, int, remove_name, inode_t parent, const char * name, patch_t ** head);
	DECLARE(LFS_t, int, write_block, bdesc_t * block, uint32_t number, patch_t ** head);
	DECLARE(LFS_t, patch_t **, get_write_head);
	/* see bd.h for a description of get_block_space */
	DECLARE(LFS_t, int32_t, get_block_space);
	DECLARE(LFS_t, size_t, get_max_feature_id);
	DECLARE(LFS_t, const bool *, get_feature_array);
	DECLARE(LFS_t, int, get_metadata_inode, inode_t ino, uint32_t id, size_t size, void * data);
	DECLARE(LFS_t, int, get_metadata_fdesc, const fdesc_t * file, uint32_t id, size_t size, void * data);
	DECLARE(LFS_t, int, set_metadata2_inode, inode_t ino, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head);
	DECLARE(LFS_t, int, set_metadata2_fdesc, fdesc_t * file, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head);
};

#define LFS_INIT(lfs, module) { \
	OBJ_INIT(lfs, module); \
	ASSIGN(lfs, module, get_root); \
	(lfs)->blocksize = 0; (lfs)->blockdev = NULL; \
	ASSIGN(lfs, module, allocate_block); \
	ASSIGN(lfs, module, lookup_block); \
	ASSIGN(lfs, module, synthetic_lookup_block); \
	ASSIGN(lfs, module, lookup_inode); \
	ASSIGN(lfs, module, lookup_name); \
	ASSIGN(lfs, module, free_fdesc); \
	ASSIGN(lfs, module, get_file_numblocks); \
	ASSIGN(lfs, module, get_file_block); \
	ASSIGN(lfs, module, get_dirent); \
	ASSIGN(lfs, module, append_file_block); \
	ASSIGN(lfs, module, allocate_name); \
	ASSIGN(lfs, module, rename); \
	ASSIGN(lfs, module, truncate_file_block); \
	ASSIGN(lfs, module, free_block); \
	ASSIGN(lfs, module, remove_name); \
	ASSIGN(lfs, module, write_block); \
	ASSIGN(lfs, module, get_write_head); \
	ASSIGN(lfs, module, get_block_space); \
	ASSIGN(lfs, module, get_max_feature_id); \
	ASSIGN(lfs, module, get_feature_array); \
	ASSIGN(lfs, module, get_metadata_inode); \
	ASSIGN(lfs, module, get_metadata_fdesc); \
	ASSIGN(lfs, module, set_metadata2_inode); \
	ASSIGN(lfs, module, set_metadata2_fdesc); \
}

#endif /* __FSTITCH_FSCORE_LFS_H */
