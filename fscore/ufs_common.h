#ifndef __FSTITCH_FSCORE_UFS_COMMON_H
#define __FSTITCH_FSCORE_UFS_COMMON_H

#include <fscore/lfs.h>
#include <fscore/ufs_base.h>
#include <fscore/ufs_alloc.h>
#include <fscore/ufs_dirent.h>
#include <fscore/ufs_cg.h>
#include <fscore/ufs_super.h>

struct ufs_parts
{
	LFS_t * base;
	UFSmod_alloc_t * p_allocator;
	UFSmod_dirent_t * p_dirent;
	UFSmod_cg_t * p_cg;
	UFSmod_super_t * p_super;
};

struct ufs_info
{
	LFS_t lfs;
	
	BD_t * ubd;
	patch_t ** write_head;
	bdesc_t * csum_block;
	struct UFS_csum * csums;
	struct ufs_parts parts;
	// commonly used values
	uint16_t ipf; // inodes per fragment
	hash_map_t * filemap; // keep track of in-memory struct UFS_Files
};

int ufs_read_inode(struct ufs_info * info, uint32_t num, struct UFS_dinode * inode);
int ufs_write_inode(struct ufs_info * info, uint32_t num, struct UFS_dinode inode, patch_t ** head);
uint32_t ufs_read_btot(struct ufs_info * info, uint32_t num);
uint16_t ufs_read_fbp(struct ufs_info * info, uint32_t num);
int ufs_read_inode_bitmap(struct ufs_info * info, uint32_t num);
int ufs_read_fragment_bitmap(struct ufs_info * info, uint32_t num);
int ufs_read_block_bitmap(struct ufs_info * info, uint32_t num);
int ufs_write_btot(struct ufs_info * info, uint32_t num, uint32_t value, patch_t ** head);
int ufs_write_fbp(struct ufs_info * info, uint32_t num, uint16_t value, patch_t ** head);
int ufs_write_inode_bitmap(struct ufs_info * info, uint32_t num, bool value, patch_t ** head);
extern char * frsum_warning;
int ufs_write_fragment_bitmap(struct ufs_info * info, uint32_t num, bool value, patch_t ** head);
int ufs_write_block_bitmap(struct ufs_info * info, uint32_t num, bool value, patch_t ** head);
int ufs_update_summary(struct ufs_info * info, int cyl, int ndir, int nbfree, int nifree, int nffree, patch_t ** head);
int ufs_check_name(const char * p);
uint8_t fstitch_to_ufs_type(uint8_t type);
uint8_t ufs_to_fstitch_type(uint8_t type);

#endif /* __FSTITCH_FSCORE_UFS_COMMON_H */
