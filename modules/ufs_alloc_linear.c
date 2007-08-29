/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <modules/ufs_alloc_linear.h>

struct ufsmod_alloc_info {
	UFSmod_alloc_t ufsmod_alloc;

	struct ufs_info *info;
};

#define GET_UFS_INFO(object) (((struct ufsmod_alloc_info *) (object))->info)

// FIXME this is a fairly inefficient way to scan for free blocks
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t ufs_alloc_linear_find_free_block(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = super->fs_dblkno / super->fs_frag;

	// Find free block
	for (; num < super->fs_size / super->fs_frag; num++) {
		r = ufs_read_block_bitmap(info, num);
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
	struct ufs_info * info = GET_UFS_INFO(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = super->fs_dblkno;

	// Find free fragment
	for (; num < super->fs_size; num++) {
		r = ufs_read_fragment_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns a fragment number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free inodes
static uint32_t ufs_alloc_linear_find_free_inode(UFSmod_alloc_t * object, fdesc_t * file, int purpose)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	uint32_t num = UFS_ROOT_INODE + 1;

	// Find free inode
	for (; num < super->fs_ipg * super->fs_ncg; num++) {
		r = ufs_read_inode_bitmap(info, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num++; // returns a inode number
	}

	return INVALID_BLOCK;
}

static int ufs_alloc_linear_destroy(UFSmod_alloc_t * obj)
{
	struct ufsmod_alloc_info *info = (struct ufsmod_alloc_info *) obj;
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

UFSmod_alloc_t * ufs_alloc_linear(struct ufs_info * info)
{
	struct ufsmod_alloc_info * obj;

	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_ALLOC_INIT(&obj->ufsmod_alloc, ufs_alloc_linear);
	obj->info = info;
	return &obj->ufsmod_alloc;
}

