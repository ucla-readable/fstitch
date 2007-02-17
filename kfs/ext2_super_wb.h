#ifndef __KUDOS_KFS_EXT2_SUPER_WB_H
#define __KUDOS_KFS_EXT2_SUPER_WB_H

#include <kfs/ext2_super.h>
#include <kfs/ext2_base.h>

EXT2mod_super_t * ext2_super_wb(LFS_t * lfs, struct ext2_info * info);

#endif /* __KUDOS_KFS_EXT2_SUPER_WB_H */
