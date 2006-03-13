#ifndef __KUDOS_KFS_UFS_COMMON_H
#define __KUDOS_KFS_UFS_COMMON_H

#include <lib/types.h>

#include <kfs/lfs.h>
#include <kfs/ufs_base.h>
#include <kfs/ufs_alloc.h>
#include <kfs/ufs_dirent.h>
#include <kfs/ufs_super.h>

struct ufs_parts
{
	LFS_t * base;
	UFSmod_alloc_t * p_allocator;
	UFSmod_dirent_t * p_dirent;
	UFSmod_super_t * p_super;
};

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * csum_block;
	struct UFS_csum * csums;
	struct ufs_parts parts;
	// commonly used values
	uint16_t ipf; // inodes per fragment
	uint32_t * cylstart; // array of cylinder starting block numbers
	hash_map_t * filemap; // keep track of in-memory struct UFS_Files
};

int read_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode * inode);
int write_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode inode, chdesc_t ** head);
int read_cg(struct lfs_info * info, uint32_t num, struct UFS_cg * cg);
uint32_t read_btot(struct lfs_info * info, uint32_t num);
uint16_t read_fbp(struct lfs_info * info, uint32_t num);
int read_inode_bitmap(struct lfs_info * info, uint32_t num);
int read_fragment_bitmap(struct lfs_info * info, uint32_t num);
int read_block_bitmap(struct lfs_info * info, uint32_t num);
int write_btot(struct lfs_info * info, uint32_t num, uint32_t value, chdesc_t ** head);
int write_fbp(struct lfs_info * info, uint32_t num, uint16_t value, chdesc_t ** head);
int write_inode_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head);
int write_fragment_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head);
int write_block_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head);
int update_summary(struct lfs_info * info, int cyl, int ndir, int nbfree, int nifree, int nffree, chdesc_t ** head);
int check_name(const char * p);
uint8_t kfs_to_ufs_type(uint8_t type);
uint8_t ufs_to_kfs_type(uint8_t type);

#endif /* __KUDOS_KFS_UFS_COMMON_H */
