/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_WAFFLE_H
#define __FSTITCH_MODULES_WAFFLE_H

/* A waffle file system has two special blocks:
 * Block 0 is reserved for bootloaders, etc. and is not used.
 * Block 1 is reserved for the superblock, which contains the snapshots.
 * All other blocks are referred to in some way by a snapshot.
 * As in WAFL, waffle inodes may store file data in the inode if the file is
 * small, but the indirect block tree is not always constant height. */

#define WAFFLE_BLOCK_SIZE 4096
#define WAFFLE_DIRECT_POINTERS 24
#define WAFFLE_BLOCK_POINTERS (WAFFLE_BLOCK_SIZE / 4)

/* max size (in bytes) of an inode using blocks of type X */
#define WAFFLE_INLINE_SIZE ((WAFFLE_DIRECT_POINTERS + 2) * 4)
#define WAFFLE_DIRECT_SIZE (WAFFLE_BLOCK_SIZE * WAFFLE_DIRECT_POINTERS)
#define WAFFLE_INDIRECT_SIZE (WAFFLE_DIRECT_SIZE + WAFFLE_BLOCK_SIZE * WAFFLE_BLOCK_POINTERS)
/* This is larger than WAFFLE_SIZE_FILE_SIZE below, so we exclude it. */
/* #define WAFFLE_DINDIRECT_SIZE (WAFFLE_INDIRECT_SIZE + WAFFLE_BLOCK_SIZE * WAFFLE_BLOCK_POINTERS * WAFFLE_BLOCK_POINTERS) */

/* max number of blocks of an inode using blocks of type X */
#define WAFFLE_DIRECT_BLOCKS WAFFLE_DIRECT_POINTERS
#define WAFFLE_INDIRECT_BLOCKS (WAFFLE_DIRECT_BLOCKS + WAFFLE_BLOCK_POINTERS)
#define WAFFLE_DINDIRECT_BLOCKS (WAFFLE_INDIRECT_BLOCKS + WAFFLE_BLOCK_POINTERS * WAFFLE_BLOCK_POINTERS)

#define WAFFLE_SIZE_FILE_SIZE 0xFFFFFFFF

struct waffle_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint16_t i_gid;
	uint16_t i_links;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	union {
		struct {
			uint32_t i_direct[WAFFLE_DIRECT_POINTERS];
			uint32_t i_indirect;
			uint32_t i_dindirect;
		};
		uint8_t i_inline[WAFFLE_INLINE_SIZE];
	};
};

#define WAFFLE_BLOCK_INODES (WAFFLE_BLOCK_SIZE / sizeof(struct waffle_inode))

struct waffle_snapshot {
	uint32_t sn_blocks;
	uint32_t sn_inodes;
	struct waffle_inode sn_block;
	struct waffle_inode sn_inode;
};

#define WAFFLE_FS_MAGIC 0x3AFF1EF5

struct waffle_super {
	uint32_t s_magic;
	uint32_t s_blocks;
	uint32_t s_inodes;
	struct waffle_snapshot s_active;
	struct waffle_snapshot s_checkpoint;
	struct waffle_snapshot s_snapshot;
};

#define WAFFLE_NAME_LEN 122

/* names must be null-terminated */
struct waffle_dentry {
	uint32_t d_inode;
	/* same as i_mode but the permission bits are ignored */
	uint16_t d_type;
	char d_name[WAFFLE_NAME_LEN];
};

#define WAFFLE_SUPER_BLOCK 1
#define WAFFLE_ROOT_INODE 1
#define WAFFLE_LINK_MAX 32000

#define WAFFLE_S_IFMT   0xF000
#define WAFFLE_S_IFSOCK 0xC000
#define WAFFLE_S_IFLNK  0xA000
#define WAFFLE_S_IFREG  0x8000
#define WAFFLE_S_IFBLK  0x6000
#define WAFFLE_S_IFDIR  0x4000
#define WAFFLE_S_IFCHR  0x2000
#define WAFFLE_S_IFIFO  0x1000

#define WAFFLE_S_ISUID  0x0800
#define WAFFLE_S_ISGID  0x0400
#define WAFFLE_S_ISVTX  0x0200 /* sticky */
#define WAFFLE_S_IRWXU  0x01C0 /* user mask */
#define WAFFLE_S_IRUSR  0x0100
#define WAFFLE_S_IWUSR  0x0080
#define WAFFLE_S_IXUSR  0x0040
#define WAFFLE_S_IRWXG  0x0038 /* group mask */
#define WAFFLE_S_IRGRP  0x0020
#define WAFFLE_S_IWGRP  0x0010
#define WAFFLE_S_IXGRP  0x0008
#define WAFFLE_S_IRWXO  0x0007 /* others mask */
#define WAFFLE_S_IROTH  0x0004
#define WAFFLE_S_IWOTH  0x0002
#define WAFFLE_S_IXOTH  0x0001

#define WAFFLE_TYPE_UNKNOWN  0
#define WAFFLE_TYPE_FILE     1
#define WAFFLE_TYPE_DIR      2
#define WAFFLE_TYPE_CHRDEV   3
#define WAFFLE_TYPE_BLKDEV   4
#define WAFFLE_TYPE_FIFO     5
#define WAFFLE_TYPE_SOCK     6
#define WAFFLE_TYPE_SYMLINK  7

#endif /* __FSTITCH_MODULES_WAFFLE_H */
