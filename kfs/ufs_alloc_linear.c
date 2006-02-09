#include <kfs/ufs_alloc_linear.h>

// FIXME this is a fairly inefficient way to scan for free blocks
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t ufs_alloc_linear_find_free_block(UFS_Alloc_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free block
	for (num = 0; num < info->super->fs_size / info->super->fs_frag; num++) {
		r = read_block_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num; // returns a block number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free fragments
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t ufs_alloc_linear_find_free_frag(UFS_Alloc_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free fragment
	for (num = 0; num < info->super->fs_size; num++) {
		r = read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
	return num; // returns a fragment number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free inodes
static uint32_t ufs_alloc_linear_find_free_inode(UFS_Alloc_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free inode
	for (num = 0; num < info->super->fs_ipg * info->super->fs_ncg; num++) {
		r = read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num; // returns a inode number
	}

	return INVALID_BLOCK;
}

static int ufs_alloc_linear_get_config(void * object, int level, char * string, size_t length)
{
	snprintf(string, length, "");
	return 0;
}

static int ufs_alloc_linear_get_status(void * object, int level, char * string, size_t length)
{
	snprintf(string, length, "");
	return 0;
}

static int ufs_alloc_linear_destroy(UFS_Alloc_t * obj)
{
	free(OBJLOCAL(obj));
	memset(obj, 0, sizeof(*obj));
	free(obj);

	return 0;
}

UFS_Alloc_t * ufs_alloc_linear(struct lfs_info * info)
{
	UFS_Alloc_t * obj = malloc(sizeof(*obj));

	if (!obj)
		return NULL;

	UFS_ALLOC_INIT(obj, ufs_alloc_linear, info);
	return 0;
}

