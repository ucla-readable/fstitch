#include <lib/string.h>

#include <kfs/ufs_alloc_lastpos.h>

static uint32_t ufs_alloc_lastpos_find_free_block(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
	static uint32_t savenum = INVALID_BLOCK;
	uint32_t start, num;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (savenum != INVALID_BLOCK)
		num = savenum;
	else
		num = super->fs_dblkno / super->fs_frag;

	// Find free block
	for (start = num; num < super->fs_size / super->fs_frag; num++) {
		r = read_block_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a block number
		}
	}

	for (num = super->fs_dblkno / super->fs_frag; num < start; num++) {
		r = read_block_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a block number
		}
	}

	return INVALID_BLOCK;
}

static uint32_t ufs_alloc_lastpos_find_free_frag(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
	static uint32_t savenum = INVALID_BLOCK;
	uint32_t start, num;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (savenum != INVALID_BLOCK)
		num = savenum;
	else
		num = super->fs_dblkno;

	// Find free fragment
	for (start = num; num < super->fs_size; num++) {
		r = read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a fragment number
		}
	}

	for (num = super->fs_dblkno; num < start; num++) {
		r = read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a fragment number
		}
	}

	return INVALID_BLOCK;
}

static uint32_t ufs_alloc_lastpos_find_free_inode(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
	static uint32_t num = UFS_ROOT_INODE + 1;
	uint32_t start;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	// Find free inode
	for (start = num; num < super->fs_ipg * super->fs_ncg; num++) {
		r = read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns an inode number
	}

	for (num = UFS_ROOT_INODE + 1; num < start; num++) {
		r = read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns an inode number
	}

	return INVALID_BLOCK;
}

static int ufs_alloc_lastpos_get_config(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_alloc_lastpos_get_status(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_alloc_lastpos_destroy(UFSmod_alloc_t * obj)
{
	memset(obj, 0, sizeof(*obj));
	free(obj);
	return 0;
}

UFSmod_alloc_t * ufs_alloc_lastpos(struct ufs_info * info)
{
	UFSmod_alloc_t * obj;

	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_ALLOC_INIT(obj, ufs_alloc_lastpos, info);
	return obj;
}

