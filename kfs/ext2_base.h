#ifndef __KUDOS_KFS_EXT2FS_BASE_H
#define __KUDOS_KFS_EXT2FS_BASE_H

#include <lib/mmu.h>
#include <kfs/bd.h>
#include <kfs/lfs.h>

/* This file is derived from JOS' kfs/josfs_base.h */


/*
 * Constants relative to the data blocks
 */
#define EXT2_NDIRECT			12
#define EXT2_NINDIRECT			EXT2_NDIRECT
#define EXT2_DINDIRECT			(EXT2_NINDIRECT + 1)
#define EXT2_TINDIRECT			(EXT2_DINDIRECT + 1)
#define EXT2_N_BLOCKS			(EXT2_TINDIRECT + 1)

/*
 * Define EXT2_PREALLOCATE to preallocate data blocks for expanding files
 */
#define EXT2_PREALLOCATE		8
#define EXT2_DEFAULT_PREALLOC_BLOCKS    8

/*
 * Special inode numbers
 */
#define EXT2_BAD_INO             1      /* Bad blocks inode */
#define EXT2_ROOT_INO            2      /* Root inode */
#define EXT2_BOOT_LOADER_INO     5      /* Boot loader inode */
#define EXT2_UNDEL_DIR_INO       6      /* Undelete directory inode */

/* First non-reserved inode for old ext2 filesystems */
#define EXT2_GOOD_OLD_FIRST_INO 11


// Maximal count of links to a file
#define EXT2_LINK_MAX           32000

#define EXT2_MIN_BLOCK_SIZE             1024
#define EXT2_MAX_BLOCK_SIZE             4096
#define EXT2_MIN_BLOCK_LOG_SIZE           10

// Maximum size of a filename (a single path component), including null
#define EXT2_NAME_LEN 255


#ifndef __KERNEL__

#define S_IFMT   0xF000
#define S_IFSOCK 0xC000
#define S_IFLNK  0xA000
#define S_IFREG  0x8000
#define S_IFBLK  0x6000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000
#define S_ISUID  0x0800
#define S_ISGID  0x0400
#define S_ISVTX  0x0200

#endif

#define EXT2_S_ISUID	0x0800	//SUID
#define EXT2_S_ISGID	0x0400	//SGID
#define EXT2_S_ISVTX	0x0200	//sticky bit
#define EXT2_S_IRWXU	0x01C0	//user access rights mask
#define EXT2_S_IRUSR	0x0100	//read
#define EXT2_S_IWUSR	0x0080	//write
#define EXT2_S_IXUSR	0x0040	//execute
#define EXT2_S_IRWXG	0x0038	//group access rights mask
#define EXT2_S_IRGRP	0x0020	//read
#define EXT2_S_IWGRP	0x0010	//write
#define EXT2_S_IXGRP	0x0008	//execute
#define EXT2_S_IRWXO	0x0007	//others access rights mask
#define EXT2_S_IROTH	0x0004	//read
#define EXT2_S_IWOTH	0x0002	//write
#define EXT2_S_IXOTH	0x0001	//execute

#define EXT2_TYPE_UNKNOWN  0
#define EXT2_TYPE_FILE     1
#define EXT2_TYPE_DIR      2
#define EXT2_TYPE_CHRDEV   3
#define EXT2_TYPE_BLKDEV   4
#define EXT2_TYPE_FIFO     5
#define EXT2_TYPE_SOCK     6
#define EXT2_TYPE_SYMLINK  7

#define EXT2_FS_MAGIC	0xEF53	  
//FIXME this is quite made up
#define EXT2_MAX_FILE_SIZE 0xFFFFFFFF

/* for the bitmaps */
#define EXT2_FREE	1
#define EXT2_USED	0


struct EXT2_Super {
	uint32_t s_inodes_count;	/* Inodes count */
	uint32_t s_blocks_count;	/* Blocks count */
	uint32_t s_r_blocks_count;	/* Reserved blocks count */
	uint32_t s_free_blocks_count;	/* Free blocks count */
	uint32_t s_free_inodes_count;	/* Free inodes count */
	uint32_t s_first_data_block;	/* First Data Block */
	uint32_t s_log_block_size;	/* Block size */
	uint32_t s_log_frag_size;	/* Fragment size */
	uint32_t s_blocks_per_group;	/* # Blocks per group */
	uint32_t s_frags_per_group;	/* # Fragments per group */
	uint32_t s_inodes_per_group;	/* # Inodes per group */
	uint32_t s_mtime;		/* Mount time */
	uint32_t s_wtime;		/* Write time */
	uint16_t s_mnt_count;		/* Mount count */
	uint16_t s_max_mnt_count;	/* Maximal mount count */
	uint16_t s_magic;		/* Magic signature */
	uint16_t s_state;		/* File system state */
	uint16_t s_errors;		/* Behaviour when detecting errors */
	uint16_t s_minor_rev_level; 	/* minor revision level */
	uint32_t s_lastcheck;		/* time of last check */
	uint32_t s_checkinterval;	/* max. time between checks */
	uint32_t s_creator_os;		/* OS */
	uint32_t s_rev_level;		/* Revision level */
	uint16_t s_def_resuid;		/* Default uid for reserved blocks */
	uint16_t s_def_resgid;		/* Default gid for reserved blocks */
	/*
	 * These fields are for EXT2_DYNAMIC_REV superblocks only.
	 *
	 * Note: the difference between the compatible feature set and
	 * the incompatible feature set is that if there is a bit set
	 * in the incompatible feature set that the kernel doesn't
	 * know about, it should refuse to mount the filesystem.
	 * 
	 * e2fsck's requirements are more strict; if it doesn't know
	 * about a feature in either the compatible or incompatible
	 * feature set, it must abort and not try to meddle with
	 * things it doesn't understand...
	 */
	uint32_t s_first_ino; 		/* First non-reserved inode */
	uint16_t s_inode_size; 		/* size of inode structure */
	uint16_t s_block_group_nr; 	/* block group # of this superblock */
	uint32_t s_feature_compat; 	/* compatible feature set */
	uint32_t s_feature_incompat; 	/* incompatible feature set */
	uint32_t s_feature_ro_compat; 	/* readonly-compatible feature set */
	uint8_t	 s_uuid[16];		/* 128-bit uuid for volume */
	char     s_volume_name[16]; 	/* volume name */
	char     s_last_mounted[64]; 	/* directory where last mounted */
	uint32_t s_algorithm_usage_bitmap; /* For compression */
	/*
	 * Performance hints.  Directory preallocation should only
	 * happen if the EXT2_COMPAT_PREALLOC flag is on.
	 */
	uint8_t  s_prealloc_blocks;	/* Nr of blocks to try to preallocate*/
	uint8_t	 s_prealloc_dir_blocks;	/* Nr to preallocate for dirs */
	uint16_t s_padding1;
	/*
	 * Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
	 */
	uint8_t	 s_journal_uuid[16];	/* uuid of journal superblock */
	uint32_t s_journal_inum;		/* inode number of journal file */
	uint32_t s_journal_dev;		/* device number of journal file */
	uint32_t s_last_orphan;		/* start of list of inodes to delete */
	uint32_t s_hash_seed[4];		/* HTREE hash seed */
	uint8_t	 s_def_hash_version;	/* Default hash version to use */
	uint8_t	 s_reserved_char_pad;
	uint16_t s_reserved_word_pad;
	uint32_t s_default_mount_opts;
 	uint32_t s_first_meta_bg; 	/* First metablock block group */
	uint32_t s_reserved[190];	/* Padding to the end of the block */
};

/*
 * Structure of a block's group descriptor
 */
struct EXT2_group_desc
{
        uint32_t  bg_block_bitmap;        /* Blocks bitmap block */
        uint32_t  bg_inode_bitmap;        /* Inodes bitmap block */
        uint32_t  bg_inode_table;         /* Inodes table block */
        uint16_t  bg_free_blocks_count;   /* Free blocks count */
        uint16_t  bg_free_inodes_count;   /* Free inodes count */
        uint16_t  bg_used_dirs_count;     /* Directories count */
        uint16_t  bg_pad;
        uint32_t  bg_reserved[3];
};
typedef struct EXT2_group_desc EXT2_group_desc_t;

/*
 * Structure of an inode on the disk
 */
struct EXT2_inode {
	uint16_t	i_mode;		/* File mode */
	uint16_t	i_uid;		/* Low 16 bits of Owner Uid */
	uint32_t	i_size;		/* Size in bytes */
	uint32_t	i_atime;	/* Access time */
	uint32_t	i_ctime;	/* Creation time */
	uint32_t	i_mtime;	/* Modification time */
	uint32_t	i_dtime;	/* Deletion Time */
	uint16_t	i_gid;		/* Low 16 bits of Group Id */
	uint16_t	i_links_count;	/* Links count */
	uint32_t	i_blocks;	/* Blocks count */
	uint32_t	i_flags;	/* File flags */
	union {
		struct {
			uint32_t  l_i_reserved1;
		} linux1;
		struct {
			uint32_t  h_i_translator;
		} hurd1;
		struct {
			uint32_t  m_i_reserved1;
		} masix1;
	} osd1;				/* OS dependent 1 */
	uint32_t	i_block[EXT2_N_BLOCKS];/* Pointers to blocks */
	uint32_t	i_generation;	/* File version (for NFS) */
	uint32_t	i_file_acl;	/* File ACL */
	uint32_t	i_dir_acl;	/* Directory ACL */
	uint32_t	i_faddr;	/* Fragment address */
	union {
		struct {
			uint8_t	l_i_frag;	/* Fragment number */
			uint8_t	l_i_fsize;	/* Fragment size */
			uint16_t	i_pad1;
			uint16_t	l_i_uid_high;	/* these 2 fields    */
			uint16_t	l_i_gid_high;	/* were reserved2[0] */
			uint32_t	l_i_reserved2;
		} linux2;
		struct {
			uint8_t	h_i_frag;	/* Fragment number */
			uint8_t	h_i_fsize;	/* Fragment size */
			uint16_t	h_i_mode_high;
			uint16_t	h_i_uid_high;
			uint16_t	h_i_gid_high;
			uint32_t	h_i_author;
		} hurd2;
		struct {
			uint8_t	m_i_frag;	/* Fragment number */
			uint8_t	m_i_fsize;	/* Fragment size */
			uint16_t	m_pad1;
			uint32_t	m_i_reserved2[2];
		} masix2;
	} osd2;				/* OS dependent 2 */
};
typedef struct EXT2_inode EXT2_inode_t;

// File Type

struct EXT2_File {
	struct fdesc_common * common;
	struct fdesc_common base;
	
	EXT2_inode_t f_inode;		 //inode
	uint8_t f_type;		 //file type
	uint8_t	f_prealloc_count; //Number of preallocated blocks remaining
	inode_t	f_ino;		 //inode number
	uint32_t f_nopen;
	uint32_t f_prealloc_block[EXT2_PREALLOCATE]; //block numbers of preallocated blocks

};
typedef struct EXT2_File EXT2_File_t;
typedef struct EXT2_File ext2_fdesc_t;

/*
 * The new version of the directory entry.  Since EXT2 structures are
 * stored in intel byte order, and the name_len field could never be
 * bigger than 255 chars, it's safe to reclaim the extra byte for the
 * file_type field.
 */
struct EXT2_Dir_entry {
        uint32_t inode;                  /* Inode number */
        uint16_t rec_len;                /* Directory entry length */
        uint8_t  name_len;               /* Name length */
        uint8_t  file_type;
        char     name[EXT2_NAME_LEN];    /* File name */
};
typedef struct EXT2_Dir_entry EXT2_Dir_entry_t;

LFS_t * ext2(BD_t * block_device);

#endif /* __KUDOS_KFS_EXT2FS_BASE_H */
