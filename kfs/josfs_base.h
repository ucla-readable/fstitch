#ifndef __KUDOS_KFS_JOSFS_BASE_H
#define __KUDOS_KFS_JOSFS_BASE_H

/* This file is derived from JOS' inc/fs.h */

// File nodes (both in-memory and on-disk)

// Bytes per file system block - same as page size
#define JOS_BLKSIZE	PGSIZE
#define JOS_BLKBITSIZE	(JOS_BLKSIZE * 8)

// Maximum size of a filename (a single path component), including null
#define JOS_MAXNAMELEN	128

// Maximum size of a complete pathname, including null
#define JOS_MAXPATHLEN	1024

// Number of block pointers in a File descriptor
#define JOS_NDIRECT	10
// Number of direct block pointers in an indirect block
#define JOS_NINDIRECT	(JOS_BLKSIZE / 4)

#define JOS_MAXFILESIZE	(JOS_NINDIRECT * JOS_BLKSIZE)

struct JOS_File {
	char f_name[JOS_MAXNAMELEN];	// filename
	off_t f_size;			// file size in bytes
	uint32_t f_type;		// file type

	// Block pointers.
	// A block is allocated iff its value is != 0.
	uint32_t f_direct[JOS_NDIRECT];	// direct blocks
	uint32_t f_indirect;		// indirect block

	// Points to the directory in which this file lives.
	// Meaningful only in memory; the value on disk can be garbage.
	// dir_lookup() sets the value when required.
	struct JOS_File* f_dir;

	// Pad out to 256 bytes; must do arithmetic in case we're compiling
	// fsformat on a 64-bit machine.
	uint8_t f_pad[256 - JOS_MAXNAMELEN - 8 - 4*JOS_NDIRECT - 4 - sizeof(struct JOS_File*)];
};

#define JOS_FS_MAGIC	0x4A0530AE	  // related vaguely to 'J\0S!'

struct JOS_Super {
	uint32_t s_magic;		// Magic number: FS_MAGIC
	uint32_t s_nblocks;		// Total number of blocks on disk
	struct JOS_File s_root;		// Root directory node
};

#endif /* __KUDOS_KFS_JOSFS_BASE_H */
