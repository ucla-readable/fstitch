#ifndef __FSTITCH_FSCORE_CFS_H
#define __FSTITCH_FSCORE_CFS_H

#include <fscore/oo.h>
#include <fscore/types.h>
#include <fscore/feature.h>
#include <fscore/inode.h>
#include <fscore/fdesc.h>
#include <lib/dirent.h>

struct CFS {
	OBJECT(CFS_t);
	DECLARE(CFS_t, int, get_root, inode_t * inode);
	DECLARE(CFS_t, int, lookup, inode_t parent, const char * name, inode_t * inode);
	DECLARE(CFS_t, int, open, inode_t inode, int mode, fdesc_t ** fdesc);
	DECLARE(CFS_t, int, create, inode_t parent, const char * name, int mode, const metadata_set_t * initial_metadata, fdesc_t ** fdesc, inode_t * new_inode);
	DECLARE(CFS_t, int, close, fdesc_t * fdesc);
	DECLARE(CFS_t, int, read, fdesc_t * fdesc, page_t * page, void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, write, fdesc_t * fdesc, page_t * page, const void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, get_dirent, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
	DECLARE(CFS_t, int, truncate, fdesc_t * fdesc, uint32_t size);
	DECLARE(CFS_t, int, unlink, inode_t parent, const char * name);
	DECLARE(CFS_t, int, link, inode_t inode, inode_t newparent, const char * newname);
	DECLARE(CFS_t, int, rename, inode_t old_parent, const char * old_name, inode_t new_parent, const char * new_name);
	DECLARE(CFS_t, int, mkdir, inode_t parent, const char * name, const metadata_set_t * initial_metadata, inode_t * inode);
	DECLARE(CFS_t, int, rmdir, inode_t parent, const char * name);
	DECLARE(CFS_t, size_t, get_max_feature_id);
	DECLARE(CFS_t, const bool *, get_feature_array);
	DECLARE(CFS_t, int, get_metadata, inode_t inode, uint32_t id, size_t size, void * data);
	DECLARE(CFS_t, int, set_metadata2, inode_t inode, const fsmetadata_t *fsm, size_t nfsm);
};

#define CFS_INIT(cfs, module) { \
	OBJ_INIT(cfs, module); \
	ASSIGN(cfs, module, get_root); \
	ASSIGN(cfs, module, lookup); \
	ASSIGN(cfs, module, open); \
	ASSIGN(cfs, module, create); \
	ASSIGN(cfs, module, close); \
	ASSIGN(cfs, module, read); \
	ASSIGN(cfs, module, write); \
	ASSIGN(cfs, module, get_dirent); \
	ASSIGN(cfs, module, truncate); \
	ASSIGN(cfs, module, unlink); \
	ASSIGN(cfs, module, link); \
	ASSIGN(cfs, module, rename); \
	ASSIGN(cfs, module, mkdir); \
	ASSIGN(cfs, module, rmdir); \
	ASSIGN(cfs, module, get_max_feature_id); \
	ASSIGN(cfs, module, get_feature_array); \
	ASSIGN(cfs, module, get_metadata); \
	ASSIGN(cfs, module, set_metadata2); \
}

#endif /* __FSTITCH_FSCORE_CFS_H */
