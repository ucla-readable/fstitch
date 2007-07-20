#include <lib/platform.h>

#include <kfs/lfs.h>
#include <kfs/feature.h>
#include <kfs/modman.h>
#include <kfs/opgroup.h>
#include <kfs/opgroup_lfs.h>

struct opgroup_info {
	LFS_t lfs;
	
	LFS_t * below_lfs;
};

/* TODO: convert this file to use get_write_head instead of opgroup_prepare_head? */

#if 0
static int opgroup_lfs_get_config(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int opgroup_lfs_get_status(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}
#endif

static int opgroup_lfs_get_root(LFS_t * object, inode_t * ino)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_root, ino);	
}

static uint32_t opgroup_lfs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	uint32_t block;
	int r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return INVALID_BLOCK;

	block = CALL(info->below_lfs, allocate_block, file, purpose, head);
	if(block != INVALID_BLOCK)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return block;
}

static bdesc_t * opgroup_lfs_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, lookup_block, number);
}

static bdesc_t * opgroup_lfs_synthetic_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, synthetic_lookup_block, number);
}

static fdesc_t * opgroup_lfs_lookup_inode(LFS_t * object, inode_t ino)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, lookup_inode, ino);
}

static int opgroup_lfs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, lookup_name, parent, name, ino);
}

static void opgroup_lfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	CALL(((struct opgroup_info *) object)->below_lfs, free_fdesc, fdesc);
}

static uint32_t opgroup_lfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_file_numblocks, file);
}

static uint32_t opgroup_lfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_file_block, file, offset);
}

static int opgroup_lfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_dirent, file, entry, size, basep);
}

static int opgroup_lfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, append_file_block, file, block, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static fdesc_t * opgroup_lfs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	fdesc_t * fdesc;
	int r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return NULL;

	fdesc = CALL(info->below_lfs, allocate_name, parent, name, type, link, initialmd, newino, head);
	if(fdesc)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return fdesc;
}

static int opgroup_lfs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, rename, oldparent, oldname, newparent, newname, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static uint32_t opgroup_lfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	uint32_t block;
	int r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return INVALID_BLOCK;

	block = CALL(info->below_lfs, truncate_file_block, file, head);
	if(block != INVALID_BLOCK)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return block;
}

static int opgroup_lfs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, free_block, file, block, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int opgroup_lfs_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, remove_name, parent, name, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int opgroup_lfs_write_block_lfs(LFS_t * object, bdesc_t * block, uint32_t number, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, write_block_lfs, block, number, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static chdesc_t ** opgroup_lfs_get_write_head(LFS_t * object)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	return CALL(info->below_lfs, get_write_head);
}

static int32_t opgroup_lfs_get_block_space(LFS_t * object)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	return CALL(info->below_lfs, get_block_space);
}

static size_t opgroup_lfs_get_max_feature_id(LFS_t * object)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_max_feature_id);
}

static const bool * opgroup_lfs_get_feature_array(LFS_t * object)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_feature_array);
}

static int opgroup_lfs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, void * data)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_metadata_inode, ino, id, size, data);
}

static int opgroup_lfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	return CALL(((struct opgroup_info *) object)->below_lfs, get_metadata_fdesc, file, id, size, data);
}

static int opgroup_lfs_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, set_metadata_inode, ino, id, size, data, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int opgroup_lfs_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) object;
	int value, r;

	r = opgroup_prepare_head(head);
	if(r < 0)
		return r;

	value = CALL(info->below_lfs, set_metadata_fdesc, file, id, size, data, head);
	if(value >= 0)
	{
		r = opgroup_finish_head(*head);
		/* can we do better than this? */
		assert(r >= 0);
	}
	return value;
}

static int opgroup_lfs_destroy(LFS_t * lfs)
{
	struct opgroup_info *info = (struct opgroup_info *) lfs;
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_lfs(info->below_lfs, lfs);
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

LFS_t * opgroup_lfs(LFS_t * base)
{
	struct opgroup_info * info;
	LFS_t * lfs;

	info = malloc(sizeof(*info));
	if(!info)
		return NULL;

	lfs = &info->lfs;
	LFS_INIT(lfs, opgroup_lfs);
	
	info->below_lfs = base;
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
