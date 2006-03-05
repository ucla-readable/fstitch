#ifndef __KUDOS_KFS_UFS_COMMON_H
#define __KUDOS_KFS_UFS_COMMON_H

#include <lib/types.h>
#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <assert.h>

#include <kfs/lfs.h>
#include <kfs/ufs_base.h>
#include <kfs/ufs_alloc.h>
#include <kfs/ufs_dirent.h>

struct ufs_parts
{
	LFS_t * base;
	UFS_Alloc_t * p_allocator;
	UFS_Dirent_t * p_dirent;
};

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	bdesc_t * csum_block;
	struct UFS_Super * super;
	struct UFS_csum * csums;
	struct ufs_parts parts;
	// commonly used values
	uint16_t ipf; // inodes per fragment
	uint32_t * cylstart; // array of cylinder starting block numbers
	hash_map_t * filemap; // keep track of in-memory struct UFS_Files
};

int read_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode * inode);
int write_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode inode, chdesc_t ** head, chdesc_t ** tail);
int read_cg(struct lfs_info * info, uint32_t num, struct UFS_cg * cg);
uint32_t read_btot(struct lfs_info * info, uint32_t num);
uint16_t read_fbp(struct lfs_info * info, uint32_t num);
int read_inode_bitmap(struct lfs_info * info, uint32_t num);
int read_fragment_bitmap(struct lfs_info * info, uint32_t num);
int read_block_bitmap(struct lfs_info * info, uint32_t num);
int write_btot(struct lfs_info * info, uint32_t num, uint32_t value, chdesc_t ** head, chdesc_t ** tail);
int write_fbp(struct lfs_info * info, uint32_t num, uint16_t value, chdesc_t ** head, chdesc_t ** tail);
int write_inode_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail);
int write_fragment_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail);
int write_block_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail);
int update_summary(struct lfs_info * info, int cyl, int ndir, int nbfree, int nifree, int nffree, chdesc_t ** head, chdesc_t ** tail);
int check_name(const char * p);

#endif /* __KUDOS_KFS_UFS_COMMON_H */
