#ifndef __KUDOS_KFS_JOSFS_BASE_H
#define __KUDOS_KFS_JOSFS_BASE_H

/* This file is derived from JOS' inc/fs.h */

// File nodes (both in-memory and on-disk)

// Bytes per file system block - same as page size
#define JOSFS_BLKSIZE	PGSIZE
#define JOSFS_BLKBITSIZE	(JOSFS_BLKSIZE * 8)

// Maximum size of a filename (a single path component), including null
#define JOSFS_MAXNAMELEN	128

// Maximum size of a complete pathname, including null
#define JOSFS_MAXPATHLEN	1024

// Number of block pointers in a File descriptor
#define JOSFS_NDIRECT	10
// Number of direct block pointers in an indirect block
#define JOSFS_NINDIRECT	(JOSFS_BLKSIZE / 4)

#define JOSFS_MAXFILESIZE	(JOSFS_NINDIRECT * JOSFS_BLKSIZE)

struct JOSFS_File {
	char f_name[JOSFS_MAXNAMELEN];	// filename
	off_t f_size;			// file size in bytes
	uint32_t f_type;		// file type

	// Block pointers.
	// A block is allocated iff its value is != 0.
	uint32_t f_direct[JOSFS_NDIRECT];	// direct blocks
	uint32_t f_indirect;		// indirect block

	// Points to the directory in which this file lives.
	// Meaningful only in memory; the value on disk can be garbage.
	// dir_lookup() sets the value when required.
	struct JOSFS_File* f_dir;

	// Pad out to 256 bytes; must do arithmetic in case we're compiling
	// fsformat on a 64-bit machine.
	uint8_t f_pad[256 - JOSFS_MAXNAMELEN - 8 - 4*JOSFS_NDIRECT - 4 - sizeof(struct JOSFS_File*)];
};

#define JOSFS_FS_MAGIC	0x4A0530AE	  // related vaguely to 'J\0S!'
#define JOSFS_BLKFILES       (JOSFS_BLKSIZE / sizeof(struct JOSFS_File))

struct JOSFS_Super {
	uint32_t s_magic;		// Magic number: FS_MAGIC
	uint32_t s_nblocks;		// Total number of blocks on disk
	struct JOSFS_File s_root;		// Root directory node
};

static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep);
static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);

#endif /* __KUDOS_KFS_JOSFS_BASE_H */
