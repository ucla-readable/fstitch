#include <lib/platform.h>

#include <fscore/lfs.h>
#include <fscore/feature.h>
#include <fscore/modman.h>
#include <fscore/patchgroup.h>
#include <fscore/patchgroup_lfs.h>

struct patchgroup_info {
	LFS_t my_lfs;
	
	LFS_t * lfs;
};

/* TODO: convert this file to use get_write_head instead of patchgroup_prepare_head? */

static int patchgroup_lfs_get_root(LFS_t * object, inode_t * ino)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_root, ino);	
}

static uint32_t patchgroup_lfs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	uint32_t block;
	int r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return INVALID_BLOCK;

	block = CALL(info->lfs, allocate_block, file, purpose, head);
	if(block != INVALID_BLOCK)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return block;
}

static bdesc_t * patchgroup_lfs_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	return CALL(((struct patchgroup_info *) object)->lfs, lookup_block, number, page);
}

static bdesc_t * patchgroup_lfs_synthetic_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	return CALL(((struct patchgroup_info *) object)->lfs, synthetic_lookup_block, number, page);
}

static fdesc_t * patchgroup_lfs_lookup_inode(LFS_t * object, inode_t ino)
{
	return CALL(((struct patchgroup_info *) object)->lfs, lookup_inode, ino);
}

static int patchgroup_lfs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	return CALL(((struct patchgroup_info *) object)->lfs, lookup_name, parent, name, ino);
}

static void patchgroup_lfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	CALL(((struct patchgroup_info *) object)->lfs, free_fdesc, fdesc);
}

static uint32_t patchgroup_lfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_file_numblocks, file);
}

static uint32_t patchgroup_lfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_file_block, file, offset);
}

static int patchgroup_lfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_dirent, file, entry, size, basep);
}

static int patchgroup_lfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, append_file_block, file, block, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static fdesc_t * patchgroup_lfs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	fdesc_t * fdesc;
	int r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return NULL;

	fdesc = CALL(info->lfs, allocate_name, parent, name, type, link, initialmd, newino, head);
	if(fdesc)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return fdesc;
}

static int patchgroup_lfs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, rename, oldparent, oldname, newparent, newname, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static uint32_t patchgroup_lfs_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	uint32_t block;
	int r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return INVALID_BLOCK;

	block = CALL(info->lfs, truncate_file_block, file, head);
	if(block != INVALID_BLOCK)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return block;
}

static int patchgroup_lfs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, free_block, file, block, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int patchgroup_lfs_remove_name(LFS_t * object, inode_t parent, const char * name, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, remove_name, parent, name, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int patchgroup_lfs_write_block(LFS_t * object, bdesc_t * block, uint32_t number, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, write_block, block, number, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static patch_t ** patchgroup_lfs_get_write_head(LFS_t * object)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	return CALL(info->lfs, get_write_head);
}

static int32_t patchgroup_lfs_get_block_space(LFS_t * object)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	return CALL(info->lfs, get_block_space);
}

static size_t patchgroup_lfs_get_max_feature_id(LFS_t * object)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_max_feature_id);
}

static const bool * patchgroup_lfs_get_feature_array(LFS_t * object)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_feature_array);
}

static int patchgroup_lfs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, void * data)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_metadata_inode, ino, id, size, data);
}

static int patchgroup_lfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	return CALL(((struct patchgroup_info *) object)->lfs, get_metadata_fdesc, file, id, size, data);
}

static int patchgroup_lfs_set_metadata2_inode(LFS_t * object, inode_t ino, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, set_metadata2_inode, ino, fsm, nfsm, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int patchgroup_lfs_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head)
{
	struct patchgroup_info * info = (struct patchgroup_info *) object;
	int value, r;

	r = patchgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->lfs, set_metadata2_fdesc, file, fsm, nfsm, head);
	if(value >= 0)
	{
		r = patchgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int patchgroup_lfs_destroy(LFS_t * lfs)
{
	struct patchgroup_info *info = (struct patchgroup_info *) lfs;
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_lfs(info->lfs, lfs);
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

LFS_t * patchgroup_lfs(LFS_t * base)
{
	struct patchgroup_info * info;
	LFS_t * lfs;

	info = malloc(sizeof(*info));
	if(!info)
		return NULL;

	lfs = &info->my_lfs;
	LFS_INIT(lfs, patchgroup_lfs);
	
	info->lfs = base;
	lfs->blocksize = base->blocksize;
	lfs->blockdev = base->blockdev;
	
	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_lfs(base, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}
	
	return lfs;
}
