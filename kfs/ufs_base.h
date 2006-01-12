#ifndef __KUDOS_KFS_UFS_BASE_H
#define __KUDOS_KFS_UFS_BASE_H

#include <lib/mmu.h>
#include <kfs/bd.h>
#include <kfs/lfs.h>

// FreeBSD upped the default block size to 16k
#define UFS_BLKSIZE 16384
#define UFS_FRAGSIZE 2048
#define UFS_BLKBITSIZE	(UFS_BLKSIZE * 8)

// Maximum size of a filename (a single path component), including null
#define UFS_MAXNAMELEN	255

// Maximum size of a complete pathname, including null
#define UFS_MAXPATHLEN	1024

#define UFS_MAXFRAG	8 /* Maximum number of fragments per block */
#define UFS_NDADDR	12 /* Direct addresses in inode. */
#define UFS_NIADDR	3  /* Indirect addresses in inode. */

#define UFS_MAXFILESIZE 0xFFFFFFFF
#define UFS_INVALID16 0xFFFF

// Sigh, on disk, used inodes are marked 1, but used frags are marked 0
#define UFS_USED 0
#define UFS_FREE 1

/*---------------------------------------------------------------------*/

// TODO Insert BSD Copyright legalese

#define UFS_BBSIZE 8192
#define UFS_SBSIZE 8192
#define UFS_BBOFF 0
#define UFS_SBOFF BBOFF + BBSIZE
#define UFS_BBLOCK 0
#define UFS_SBLOCK BBLOCK + BBSIZE / DEV_BSIZE
#define UFS_MINBSIZE 4096
#define UFS_MAXMNTLEN 512
#define UFS_NOCSPTRS ((128 / sizeof(void *)) - 3)
#define UFS_ROOT_INODE 2

struct UFS_csum {
	int32_t cs_ndir;	/* number of directories */
	int32_t cs_nbfree;	/* number of free blocks */
	int32_t cs_nifree;	/* number of free inodes */
	int32_t cs_nffree;	/* number of free frags */
};

/*
 * Super block for an FFS file system.
 */
struct UFS_Super {
	int32_t  fs_firstfield;	/* 0: historic file system linked list, */
	int32_t  fs_unused_1;	/* 4: used for incore super blocks */
	int32_t  fs_sblkno;	/* 8: addr of super-block in filesys */ 
	int32_t  fs_cblkno;	/* 12: offset of cyl-block in filesys */
	int32_t  fs_iblkno;	/* 16: offset of inode-blocks in filesys */
	int32_t  fs_dblkno;	/* 20: offset of first data after cg */
	int32_t  fs_cgoffset;	/* 24: cylinder group offset in cylinder */
	int32_t  fs_cgmask;	/* 28: used to calc mod fs_ntrak */
	int32_t  fs_time;	/* 32: last time written */
	int32_t  fs_size;	/* 36: number of blocks in fs */
	int32_t  fs_dsize;	/* 40: number of data blocks in fs */
	int32_t  fs_ncg;	/* 44: number of cylinder groups */
	int32_t  fs_bsize;	/* 48: size of basic blocks in fs */
	int32_t  fs_fsize;	/* 52: size of frag blocks in fs */
	int32_t  fs_frag;	/* 56: number of frags in a block in fs */
/* these are configuration parameters */
	int32_t  fs_minfree;	/* 60: minimum percentage of free blocks */
	int32_t  fs_rotdelay;  	/* 64: num of ms for optimal next block */
	int32_t  fs_rps;	/* 68: disk revolutions per second */
/* these fields can be computed from the others */
	int32_t  fs_bmask;	/* 72: 'blkoff' calc of blk offsets */
	int32_t  fs_fmask;	/* 76: 'fragoff' calc of frag offsets */
	int32_t  fs_bshift;	/* 80: 'lblkno' calc of logical blkno */
	int32_t  fs_fshift;	/* 84: 'numfrags' calc number of frags */
/* these are configuration parameters */
	int32_t  fs_maxcontig; 	/* 88: max number of contiguous blks */
	int32_t  fs_maxbpg;	/* 92: max number of blks per cyl group */
/* these fields can be computed from the others */
	int32_t  fs_fragshift; 	/* 96: block to frag shift */
	int32_t  fs_fsbtodb;	/* 100: fsbtodb and dbtofsb shift constant */
	int32_t  fs_sbsize;	/* 104: actual size of super block */
	int32_t  fs_csmask;	/* 108: csum block offset */
	int32_t  fs_csshift;	/* 112: csum block number */
	int32_t  fs_nindir;	/* 116: value of NINDIR */
	int32_t  fs_inopb;	/* 120: value of INOPB */
	int32_t  fs_nspf;	/* 124: value of NSPF */
/* yet another configuration parameter */
	int32_t  fs_optim;	/* 128: optimization preference, see below */
/* these fields are derived from the hardware */
	int32_t  fs_npsect;	/* 132: # sectors/track including spares */
	int32_t  fs_interleave;	/* 136: hardware sector interleave */
	int32_t  fs_trackskew;	/* 140: sector 0 skew, per track */
/* fs_id takes the space of the unused fs_headswitch and fs_trkseek fields */
	int32_t fs_id[2];	/* 144: unique filesystem id*/
/* sizes determined by number of cylinder groups and their sizes */
	int32_t  fs_csaddr;	/* 152: blk addr of cyl grp summary area */
	int32_t  fs_cssize;	/* 156: size of cyl grp summary area */
	int32_t  fs_cgsize;	/* 160: cylinder group size */
/* these fields are derived from the hardware */
	int32_t  fs_ntrak;	/* 164: tracks per cylinder */
	int32_t  fs_nsect;	/* 168: sectors per track */
	int32_t  fs_spc;	/* 172: sectors per cylinder */
/* this comes from the disk driver partitioning */
	int32_t  fs_ncyl;	/* 176: cylinders in file system */
/* these fields can be computed from the others */
	int32_t  fs_cpg;	/* 180: cylinders per group */
	int32_t  fs_ipg;	/* 184: inodes per group */
	int32_t  fs_fpg;	/* 188: blocks per group * fs_frag */
/* this data must be re-computed after crashes */
	struct UFS_csum fs_cstotal; /* 192: cylinder summary information */
/* these fields are cleared at mount time */
	int8_t   fs_fmod;	/* 208: super block modified flag */
	int8_t   fs_clean;	/* 209: file system is clean flag */
	int8_t   fs_ronly;	/* 210: mounted read-only flag */
	int8_t   fs_flags;	/* 211: currently unused flag */
	uint8_t  fs_fsmnt[UFS_MAXMNTLEN];	/* 212: name mounted on */
/* these fields retain the current block allocation info */
	int32_t  fs_cgrotor;	/* 724: last cg searched */
	void	*fs_ocsp[UFS_NOCSPTRS];	/* 728: padding; was list of fs_cs buffers */
	uint8_t *fs_contigdirs;	/* 844: # of contiguously allocated dirs */
	struct UFS_csum *fs_csp;	/* 848: cg summary info buffer for fs_cs */
	int32_t *fs_maxcluster;	/* 852: max cluster in each cyl group */
	int32_t  fs_cpc;	/* 856: cyl per cycle in postbl */
	int16_t  fs_opostbl[16][8];	/* 860: old rotation block list head */
	int32_t  fs_sparecon[50];	/* 1116: reserved for future constants */
	int32_t  fs_contigsumsize;	/* 1316: size of cluster summary array */
	int32_t  fs_maxsymlinklen;	/* 1320: max length of an internal symlink */
	int32_t  fs_inodefmt;	/* 1324: format of on-disk inodes */
	uint64_t fs_maxfilesize;	/* 1328: maximum representable file size */
	int64_t  fs_qbmask;	/* 1336: ~fs_bmask for use with 64-bit size */
	int64_t  fs_qfmask;	/* 1344: ~fs_fmask for use with 64-bit size */
	int32_t  fs_state;	/* 1352: validate fs_clean field */
	int32_t  fs_postblformat; /* 1356: format of positional layout tables */
	int32_t  fs_nrpos;	/* 1360: number of rotational positions */
	int32_t  fs_postbloff;	/* 1364: (u_int16) rotation block list head */
	int32_t  fs_rotbloff;	/* 1368: (u_int8) blocks for each rotation */
	int32_t  fs_magic;	/* 1372: magic number */
	uint8_t  fs_space[1];	/* 1376: list of blocks for each rotation */
/* actually longer */
};

struct UFS_cg {
	int32_t  cg_firstfield;	/* 0: historic cyl groups linked list */
	int32_t  cg_magic;	/* 4: magic number */
	int32_t  cg_time;	/* 8: time last written */
	int32_t  cg_cgx;	/* 12: we are the cgx'th cylinder group */
	int16_t  cg_ncyl;	/* 16: number of cyl's this cg */
	int16_t  cg_niblk;	/* 18: number of inode blocks this cg */
	int32_t  cg_ndblk;	/* 20: number of data blocks this cg */
	struct UFS_csum cg_cs;	/* 24: cylinder summary information */
	int32_t  cg_rotor;	/* 40: position of last used block */
	int32_t  cg_frotor;	/* 44: position of last used frag */
	int32_t  cg_irotor;	/* 48: position of last used inode */
	int32_t  cg_frsum[UFS_MAXFRAG];	/* 52: counts of available frags */
	int32_t  cg_btotoff;	/* 84: (int32) block totals per cylinder */
	int32_t  cg_boff;	/* 88: (u_int16) free block positions */
	int32_t  cg_iusedoff;	/* 92: (u_int8) used inode map */
	int32_t  cg_freeoff;	/* 96: (u_int8) free block map */
	int32_t  cg_nextfreeoff;	/* 100: (u_int8) next available space */
	int32_t  cg_clustersumoff;	/* 104: (u_int32) counts of avail clusters */
	int32_t  cg_clusteroff;	/* 108: (u_int8) free cluster map */
	int32_t  cg_nclusterblks;	/* 112: number of clusters this cg */
	int32_t  cg_sparecon[13];	/* 116: reserved for future use */
	uint8_t  cg_space[1];	/* space for cylinder group maps */
/* actually longer */
};

struct UFS_dinode {
	uint16_t di_mode;	/*   0: IFMT, permissions; see below. */
	int16_t  di_nlink;	/*   2: File link count. */
	union {
		uint16_t oldids[2];	/*   4: Ffs: old user and group ids. */
		int32_t	 inumber;	/*   4: Lfs: inode number. */
	} di_u;
	uint64_t di_size;	/*   8: File byte count. */
	int32_t  di_atime;	/*  16: Last access time. */
	int32_t  di_atimensec;	/*  20: Last access time. */
	int32_t  di_mtime;	/*  24: Last modified time. */
	int32_t  di_mtimensec;	/*  28: Last modified time. */
	int32_t  di_ctime;	/*  32: Last inode change time. */
	int32_t  di_ctimensec;	/*  36: Last inode change time. */
	uint32_t di_db[UFS_NDADDR];	/*  40: Direct disk blocks. */
	uint32_t di_ib[UFS_NIADDR];	/*  88: Indirect disk blocks. */
	uint32_t di_flags;	/* 100: Status flags (chflags). */
	int32_t  di_blocks;	/* 104: Blocks actually held. */
	int32_t  di_gen;	/* 108: Generation number. */
	uint32_t di_uid;	/* 112: File owner. */
	uint32_t di_gid;	/* 116: File group. */
	int32_t  di_spare[2];	/* 120: Reserved; currently unused */
};

struct  UFS_direct {
	uint32_t d_ino;	/* inode number of entry */
	uint16_t d_reclen;	/* length of this record */
	uint8_t  d_type;	/* file type, see below */
	uint8_t  d_namlen;	/* length of string in d_name */
	char     d_name[UFS_MAXNAMELEN + 1]; /* name with length <= MAXNAMLEN */
};

/*
 * Filesystem identification
 */
#define UFS_CG_MAGIC	0x090255
#define UFS_MAGIC	0x011954  	/* the fast filesystem magic number */
#define UFS_OKAY	0x7c269d38	/* superblock checksum */
#define UFS_42INODEFMT 	-1	/* 4.2BSD inode format */
#define UFS_44INODEFMT 	2	/* 4.4BSD inode format */

/*
 * Filesystem flags.
 */
#define UFS_UNCLEAN	0x01	/* filesystem not clean at mount */
#define UFS_DOSOFTDEP	0x02	/* filesystem using soft dependencies */

/*
 * File types
 */

#define UFS_DT_UNKNOWN	0
#define UFS_DT_FIFO	1
#define UFS_DT_CHR	2
#define UFS_DT_DIR	4
#define UFS_DT_BLK	6
#define UFS_DT_REG	8
#define UFS_DT_LNK	10
#define UFS_DT_SOCK	12
#define UFS_DT_WHT	14

/* File permissions. */
#define UFS_IEXEC	0000100	/* Executable. */
#define UFS_IWRITE	0000200	/* Writeable. */
#define UFS_IREAD	0000400	/* Readable. */
#define UFS_ISVTX	0001000	/* Sticky bit. */
#define UFS_ISGID	0002000	/* Set-gid. */
#define UFS_ISUID	0004000	/* Set-uid. */

/* File types. */
#define UFS_IFMT	0170000	/* Mask of file type. */
#define UFS_IFIFO	0010000	/* Named pipe (fifo). */
#define UFS_IFCHR	0020000	/* Character device. */
#define UFS_IFDIR	0040000	/* Directory file. */
#define UFS_IFBLK	0060000	/* Block device. */
#define UFS_IFREG	0100000	/* Regular file. */
#define UFS_IFLNK	0120000	/* Symbolic link. */
#define UFS_IFSOCK	0140000	/* UNIX domain socket. */
#define UFS_IFWHT	0160000	/* Whiteout. */

/*---------------------------------------------------------------------*/

typedef struct UFS_File UFS_File_t;

struct UFS_File {
	struct UFS_dinode f_inode;
	uint32_t f_num; // Inode number
	uint32_t f_numfrags; // Number of fragments
	uint32_t f_lastfrag; // Last fragment in the file
	uint32_t f_lastalloc; // Last fragment we allocated
	uint8_t f_type;
};


LFS_t * ufs(BD_t * block_device);

#endif /* __KUDOS_KFS_UFS_BASE_H */
