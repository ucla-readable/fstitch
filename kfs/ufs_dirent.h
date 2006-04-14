#ifndef __KUDOS_KFS_UFS_DIRENT_H
#define __KUDOS_KFS_UFS_DIRENT_H

#include <lib/types.h>

#include <kfs/oo.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/fdesc.h>
#include <kfs/feature.h>
#include <kfs/inode.h>
#include <kfs/lfs.h>
#include <lib/dirent.h>

#include <kfs/ufs_base.h>

/*
 * search_dirent:    Sets 'offset' to the offset of a file named 'file' in
 *                   'dirf'.
 * insert_dirent:    Insert an entry 'dirinfo' to 'dirf'.
 * delete_dirent:    Deletes the entry named 'name' from 'dirf'.
 * get_dirent:       Read an entry of up to 'size' bytes into 'entry' from
 *                   'dirf' starting at an offset described by 'basep'.
 *                   'basep' will be modified by on success.
 * modify_dirent:    Write 'entry' to 'dirf' at an offset described by 'basep'.
 */

struct UFSmod_dirent;
typedef struct UFSmod_dirent UFSmod_dirent_t;

struct UFSmod_dirent {
	OBJECT(UFSmod_dirent_t);
	DECLARE(UFSmod_dirent_t, int, search_dirent, ufs_fdesc_t * dirf, const char * name, inode_t * ino, int * offset);
	DECLARE(UFSmod_dirent_t, int, insert_dirent, ufs_fdesc_t * dirf, struct dirent dirinfo, chdesc_t ** head);
	DECLARE(UFSmod_dirent_t, int, delete_dirent, ufs_fdesc_t * dirf, const char * name, chdesc_t ** head);
	DECLARE(UFSmod_dirent_t, int, get_dirent, ufs_fdesc_t * dirf, struct dirent * entry, uint16_t size, uint32_t * basep);
	DECLARE(UFSmod_dirent_t, int, modify_dirent, ufs_fdesc_t * dirf, struct dirent entry, uint32_t basep, chdesc_t ** head);
};

#define UFS_DIRENT_INIT(ufs, module, info) { \
	OBJ_INIT(ufs, module, info); \
	ASSIGN(ufs, module, search_dirent); \
	ASSIGN(ufs, module, insert_dirent); \
	ASSIGN(ufs, module, delete_dirent); \
	ASSIGN(ufs, module, get_dirent); \
	ASSIGN(ufs, module, modify_dirent); \
}

#endif /* __KUDOS_KFS_UFS_DIRENT_H */
