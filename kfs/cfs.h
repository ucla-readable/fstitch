#ifndef __KUDOS_KFS_CFS_H
#define __KUDOS_KFS_CFS_H

#include <lib/types.h>

#include <kfs/oo.h>
#include <kfs/feature.h>
#include <kfs/inode.h>
#include <kfs/fdesc.h>

struct CFS;
typedef struct CFS CFS_t;

struct CFS {
	OBJECT(CFS_t);
	DECLARE(CFS_t, int, get_root, inode_t * inode);
	DECLARE(CFS_t, int, lookup, inode_t parent, const char * name, inode_t * inode);
	DECLARE(CFS_t, int, open, inode_t inode, int mode, fdesc_t ** fdesc);
	DECLARE(CFS_t, int, create, inode_t parent, const char * name, int mode, fdesc_t ** fdesc, inode_t * new_inode);
	DECLARE(CFS_t, int, close, fdesc_t * fdesc);
	DECLARE(CFS_t, int, read, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, write, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size);
	DECLARE(CFS_t, int, getdirentries, fdesc_t * fdesc, char * buf, int nbytes, uint32_t * basep);
	DECLARE(CFS_t, int, truncate, fdesc_t * fdesc, uint32_t size);
	DECLARE(CFS_t, int, unlink, inode_t parent, const char * name);
	DECLARE(CFS_t, int, link, inode_t inode, inode_t newparent, const char * newname);
	DECLARE(CFS_t, int, rename, inode_t old_parent, const char * old_name, inode_t new_parent, const char * new_name);
	DECLARE(CFS_t, int, mkdir, inode_t parent, const char * name, inode_t * inode);
	DECLARE(CFS_t, int, rmdir, inode_t parent, const char * name);
	DECLARE(CFS_t, size_t, get_num_features, inode_t inode);
	DECLARE(CFS_t, const feature_t *, get_feature, inode_t inode, size_t num);
	DECLARE(CFS_t, int, get_metadata, inode_t inode, uint32_t id, size_t * size, void ** data);
	DECLARE(CFS_t, int, set_metadata, inode_t inode, uint32_t id, size_t size, const void * data);
};

#define CFS_INIT(cfs, module, info) { \
	OBJ_INIT(cfs, module, info); \
	ASSIGN(cfs, module, get_root); \
	ASSIGN(cfs, module, lookup); \
	ASSIGN(cfs, module, open); \
	ASSIGN(cfs, module, create); \
	ASSIGN(cfs, module, close); \
	ASSIGN(cfs, module, read); \
	ASSIGN(cfs, module, write); \
	ASSIGN(cfs, module, getdirentries); \
	ASSIGN(cfs, module, truncate); \
	ASSIGN(cfs, module, unlink); \
	ASSIGN(cfs, module, link); \
	ASSIGN(cfs, module, rename); \
	ASSIGN(cfs, module, mkdir); \
	ASSIGN(cfs, module, rmdir); \
	ASSIGN(cfs, module, get_num_features); \
	ASSIGN(cfs, module, get_feature); \
	ASSIGN(cfs, module, get_metadata); \
	ASSIGN(cfs, module, set_metadata); \
}

#endif /* __KUDOS_KFS_CFS_H */
