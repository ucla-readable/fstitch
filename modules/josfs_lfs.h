#ifndef __FSTITCH_FSCORE_JOSFS_BASE_H
#define __FSTITCH_FSCORE_JOSFS_BASE_H

#ifdef FSTITCHD
#include <fscore/bd.h>
#include <fscore/lfs.h>
#endif

/* Bytes per file system block */
#define JOSFS_BLKSIZE		4096
#define JOSFS_BLKBITSIZE	(JOSFS_BLKSIZE * 8)

/* Maximum size of a filename (a single path component), including null */
#define JOSFS_MAXNAMELEN	128

/* Maximum size of a complete pathname, including null */
#define JOSFS_MAXPATHLEN	1024

/* Number of block pointers in a JOSFS_File descriptor */
#define JOSFS_NDIRECT		10
/* Number of direct block pointers in an indirect block */
#define JOSFS_NINDIRECT		(JOSFS_BLKSIZE / 4)

#define JOSFS_MAXFILESIZE	(JOSFS_NINDIRECT * JOSFS_BLKSIZE)

/* File nodes (both in-memory and on-disk) */
typedef struct JOSFS_File JOSFS_File_t;

struct JOSFS_File {
	char f_name[JOSFS_MAXNAMELEN];	/* filename */
	int32_t f_size;			/* file size in bytes */
	uint32_t f_type;		/* file type */

	/* Block pointers. A block is allocated iff its value is != 0. */
	uint32_t f_direct[JOSFS_NDIRECT];	/* direct blocks */
	uint32_t f_indirect;		/* indirect block */

	uint32_t f_mtime;		/* file mtime */
	uint32_t f_atime;		/* file atime */

	/* Pad out to 256 bytes */
	uint8_t f_pad[256 - JOSFS_MAXNAMELEN - 8 - 4 * JOSFS_NDIRECT - 12];
};

#define JOSFS_TYPE_FILE		0
#define JOSFS_TYPE_DIR		1

#define JOSFS_FS_MAGIC		0x4A0530AE	  /* related vaguely to 'J\0S!' */
#define JOSFS_BLKFILES		(JOSFS_BLKSIZE / sizeof(struct JOSFS_File))

struct JOSFS_Super {
	uint32_t s_magic;		/* Magic number: JOSFS_FS_MAGIC */
	uint32_t s_nblocks;		/* Total number of blocks on disk */
	struct JOSFS_File s_root;	/* Root directory node */
};

#ifdef FSTITCHD
LFS_t * josfs_lfs(BD_t * block_device);
#endif

#endif /* __FSTITCH_FSCORE_JOSFS_BASE_H */
