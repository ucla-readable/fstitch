/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <modules/ufs_alloc_lastpos.h>

struct ufsmod_alloc_info {
	UFSmod_alloc_t ufsmod_alloc;
	
	struct ufs_info *info;
};

#define GET_UFS_INFO(object) (((struct ufsmod_alloc_info *) (object))->info)

static uint32_t ufs_alloc_lastpos_find_free_block(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = GET_UFS_INFO(object);
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
		r = ufs_read_block_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a block number
		}
	}

	for (num = super->fs_dblkno / super->fs_frag; num < start; num++) {
		r = ufs_read_block_bitmap(info, num);
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
	struct ufs_info * info = GET_UFS_INFO(object);
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
		r = ufs_read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE) {
			savenum = num + 1;
			return num; // returns a fragment number
		}
	}

	for (num = super->fs_dblkno; num < start; num++) {
		r = ufs_read_fragment_bitmap(info, num);
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
	struct ufs_info * info = GET_UFS_INFO(object);
	static uint32_t num = UFS_ROOT_INODE + 1;
	uint32_t start;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	// Find free inode
	for (start = num; num < super->fs_ipg * super->fs_ncg; num++) {
		r = ufs_read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns an inode number
	}

	for (num = UFS_ROOT_INODE + 1; num < start; num++) {
		r = ufs_read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns an inode number
	}

	return INVALID_BLOCK;
}

static int ufs_alloc_lastpos_destroy(UFSmod_alloc_t * obj)
{
	struct ufsmod_alloc_info * info = (struct ufsmod_alloc_info *) obj;
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

UFSmod_alloc_t * ufs_alloc_lastpos(struct ufs_info * info)
{
	struct ufsmod_alloc_info * obj;

	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_ALLOC_INIT(&obj->ufsmod_alloc, ufs_alloc_lastpos);
	obj->info = info;
	return &obj->ufsmod_alloc;
}

