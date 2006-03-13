#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/lfs.h>
#include <kfs/feature.h>
#include <kfs/modman.h>
#include <kfs/opgroup.h>
#include <kfs/opgroup_lfs.h>

struct opgroup_info {
	LFS_t * lfs;
};

/* FIXME: we don't need to create a tail if opgroup_insert_change would just return doing nothing */
static chdesc_t * opgroup_prepare_head(chdesc_t ** head, int * newnoop)
{
	chdesc_t * tail;

	if(!head)
		return NULL;

	if(*head)
	{
		*newnoop = 0;
		return *head;
	}

	tail = chdesc_create_noop(NULL, NULL);
	if(!tail)
		return NULL;

	chdesc_claim_noop(tail);
	*head = tail;
	*newnoop = 1;
	return tail;
}

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

static int opgroup_lfs_get_root(LFS_t * object, inode_t * ino)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_root, ino);	
}

static uint32_t opgroup_lfs_get_blocksize(LFS_t * object)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_blocksize);
}

static BD_t * opgroup_lfs_get_blockdev(LFS_t * object)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_blockdev);
}

static uint32_t opgroup_lfs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	uint32_t block;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return INVALID_BLOCK;

	block = CALL(info->lfs, allocate_block, file, purpose, head);
	if(block != INVALID_BLOCK)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return block;
}

static bdesc_t * opgroup_lfs_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, lookup_block, number);
}

static bdesc_t * opgroup_lfs_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, synthetic_lookup_block, number, synthetic);
}

static int opgroup_lfs_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, cancel_synthetic_block, number);
}

static fdesc_t * opgroup_lfs_lookup_inode(LFS_t * object, inode_t ino)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, lookup_inode, ino);
}

static int opgroup_lfs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, lookup_name, parent, name, ino);
}

static void opgroup_lfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, free_fdesc, fdesc);
}

static uint32_t opgroup_lfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_file_numblocks, file);
}

static uint32_t opgroup_lfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_file_block, file, offset);
}

static int opgroup_lfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_dirent, file, entry, size, basep);
}

static int opgroup_lfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, append_file_block, file, block, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static fdesc_t * opgroup_lfs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, inode_t * newino, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	fdesc_t * fdesc;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return NULL;

	fdesc = CALL(info->lfs, allocate_name, parent, name, type, link, newino, head);
	if(fdesc)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return fdesc;
}

static int opgroup_lfs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, rename, oldparent, oldname, newparent, newname, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static uint32_t opgroup_lfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	uint32_t block;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return INVALID_BLOCK;

	block = CALL(info->lfs, truncate_file_block, file, head);
	if(block != INVALID_BLOCK)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return block;
}

static int opgroup_lfs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, free_block, file, block, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static int opgroup_lfs_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, remove_name, parent, name, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static int opgroup_lfs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, write_block, block, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static size_t opgroup_lfs_get_num_features(LFS_t * object, inode_t ino)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_num_features, ino);
}

static const feature_t * opgroup_lfs_get_feature(LFS_t * object, inode_t ino, size_t num)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_feature, ino, num);
}

static int opgroup_lfs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_metadata_inode, ino, id, size, data);
}

static int opgroup_lfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	return CALL(((struct opgroup_info *) OBJLOCAL(object))->lfs, get_metadata_fdesc, file, id, size, data);
}

static int opgroup_lfs_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, set_metadata_inode, ino, id, size, data, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static int opgroup_lfs_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct opgroup_info * info = (struct opgroup_info *) OBJLOCAL(object);
	int value;
	chdesc_t * tail;
	int newnoop = 0;

	tail = opgroup_prepare_head(head, &newnoop);
	if(!tail)
		return -E_INVAL;

	value = CALL(info->lfs, set_metadata_fdesc, file, id, size, data, head);
	if(value >= 0)
	{
		int r = opgroup_insert_change(*head, tail);
		/* can we do better than this? */
		assert(r >= 0);
	}
	if (newnoop)
		chdesc_autorelease_noop(tail);
	return value;
}

static int opgroup_lfs_destroy(LFS_t * lfs)
{
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_lfs(((struct opgroup_info *) OBJLOCAL(lfs))->lfs, lfs);
	
	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

LFS_t * opgroup_lfs(LFS_t * base)
{
	struct opgroup_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));

	if(!lfs)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, opgroup_lfs, info);
	
	info->lfs = base;
	
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
