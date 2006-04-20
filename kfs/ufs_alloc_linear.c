#include <lib/string.h>

#include <kfs/ufs_alloc_linear.h>

// FIXME this is a fairly inefficient way to scan for free blocks
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t ufs_alloc_linear_find_free_block(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = super->fs_dblkno / super->fs_frag;

	// Find free block
	for (; num < super->fs_size / super->fs_frag; num++) {
		r = read_block_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns a block number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free fragments
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t ufs_alloc_linear_find_free_frag(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = super->fs_dblkno;

	// Find free fragment
	for (; num < super->fs_size; num++) {
		r = read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns a fragment number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free inodes
static uint32_t ufs_alloc_linear_find_free_inode(UFSmod_alloc_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = UFS_ROOT_INODE + 1;

	// Find free inode
	for (; num < super->fs_ipg * super->fs_ncg; num++) {
		r = read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns a inode number
	}

	return INVALID_BLOCK;
}

static int ufs_alloc_linear_get_config(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_alloc_linear_get_status(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_alloc_linear_destroy(UFSmod_alloc_t * obj)
{
	free(OBJLOCAL(obj));
	memset(obj, 0, sizeof(*obj));
	free(obj);

	return 0;
}

UFSmod_alloc_t * ufs_alloc_linear(struct lfs_info * info)
{
	UFSmod_alloc_t * obj;

	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_ALLOC_INIT(obj, ufs_alloc_linear, info);
	return obj;
}

