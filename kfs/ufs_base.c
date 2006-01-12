/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <lib/types.h>
#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <assert.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/ufs_base.h>

#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in ufs_base.c
#endif

#define UFS_BASE_DEBUG 0

#if UFS_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	bdesc_t * csum_block;
	struct UFS_Super * super;
	struct UFS_csum * csum;
	// commonly used values
	uint16_t ipf; // inodes per fragment
	uint32_t * cylstart; // array of cylinder starting block numbers
	hash_map_t * filemap; // keep track of in-memory struct UFS_Files
};

struct open_ufsfile {
	UFS_File_t * file;
	int count;
};
typedef struct open_ufsfile open_ufsfile_t;

struct ufs_fdesc {
	uint32_t dir_inode; // Parent directory's inode number
	uint32_t dir_offset; // Byte offset of entry in parent directory
	char fullpath[UFS_MAXPATHLEN];
	char filename[UFS_MAXNAMELEN];
	UFS_File_t * file;
};

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number);
static int get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep, const bool skip);
static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file);
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int ufs_set_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail);
static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail);
static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);

static uint32_t read_btot(LFS_t * object, uint32_t num);
static uint16_t read_fbp(LFS_t * object, uint32_t num);
static int read_inode_bitmap(LFS_t * object, uint32_t blockno);
static int read_fragment_bitmap(LFS_t * object, uint32_t blockno);
static int read_block_bitmap(LFS_t * object, uint32_t blockno);
static int write_btot(LFS_t * object, uint32_t num, uint32_t value, chdesc_t ** head, chdesc_t ** tail);
static int write_inode_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);
static int write_fragment_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);
static int write_block_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);

static uint32_t calc_cylgrp_start(LFS_t * object, uint32_t i)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return info->super->fs_fpg * i
			+ info->super->fs_cgoffset * (i & ~info->super->fs_cgmask);
}

static void print_inode(struct UFS_dinode inode)
{
	int i;

	printf("mode: %x\n", inode.di_mode);
	printf("link count: %d\n", inode.di_nlink);
	printf("size: %d\n", inode.di_size);
	printf("owner: %d\n", inode.di_uid);
	printf("group: %d\n", inode.di_gid);
	printf("gen number: %d\n", inode.di_gen);
	printf("chflags: %d\n", inode.di_flags);

	printf("using blocks:"); 
	for (i = 0; i < UFS_NDADDR; i++)
		printf(" %d", inode.di_db[i]);
	printf("\nusing indirect blocks:"); 
	for (i = 0; i < UFS_NIADDR; i++)
		printf(" %d", inode.di_ib[i]);
	printf("\n");
}

// Assuming fixed number of inodes per cylinder group, so we don't have
// to read the cylinder group descriptor and confirm this every time.
// The last cylinder group may have less inodes?
static int read_inode(LFS_t * object, uint32_t num, struct UFS_dinode * inode)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int cg, cg_off, fragno, frag_off;
	struct UFS_dinode * wanted;
	bdesc_t * inode_table;

	if (!inode || num >= info->super->fs_ipg * info->super->fs_ncg)
		return -E_INVAL;

	cg = num / info->super->fs_ipg; // Cylinder group #
	cg_off = num % info->super->fs_ipg; // nth inode in cg
	fragno = cg_off / info->ipf; // inode is in nth fragment
	frag_off = cg_off % info->ipf; // inode is nth inode in fragment
	fragno += info->cylstart[cg] + info->super->fs_iblkno; // real fragno

	inode_table = CALL(info->ubd, read_block, fragno);
	if (!inode_table)
		return -E_NOT_FOUND;
	wanted = (struct UFS_dinode *) (inode_table->ddesc->data);
	wanted += frag_off;
	memcpy(inode, wanted, sizeof(struct UFS_dinode));

	// Not sure what chflags do, so raise a warning if any are set
	if (inode->di_flags)
		printf("Warning, inode %d has chflags set: %d\n", num);

	return 0;
}

static int write_inode(LFS_t * object, uint32_t num, struct UFS_dinode inode, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int cg, cg_off, fragno, frag_off, r, offset;
	bdesc_t * inode_table;

	if (num >= info->super->fs_ipg * info->super->fs_ncg)
		return -E_INVAL;

	cg = num / info->super->fs_ipg; // Cylinder group #
	cg_off = num % info->super->fs_ipg; // nth inode in cg
	fragno = cg_off / info->ipf; // inode is in nth fragment
	frag_off = cg_off % info->ipf; // inode is nth inode in fragment
	fragno += info->cylstart[cg] + info->super->fs_iblkno; // real fragno

	inode_table = CALL(info->ubd, read_block, fragno);
	if (!inode_table)
		return -E_NOT_FOUND;
	offset = sizeof(struct UFS_dinode) * frag_off;
	r = chdesc_create_byte(inode_table, info->ubd, offset, sizeof(struct UFS_dinode), &inode, head, tail);
	if (r < 0)
		return r;

	return CALL(info->ubd, write_block, inode_table);
}

static int read_cg(LFS_t * object, uint32_t num, struct UFS_cg * cg)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * b;

	if (num >= info->super->fs_ncg)
		return -E_INVAL;
	if (!cg)
		return -E_INVAL;

	b = CALL(info->ubd, read_block,
			info->cylstart[num] + info->super->fs_cblkno);
	if (!b)
		return -E_NOT_FOUND;

	memcpy(cg, b->ddesc->data, sizeof(struct UFS_cg));

	return 0;
}

// TODO do more checks, move printf statements elsewhere, mark fs as unclean
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t numblocks;
	int i;

	/* make sure we have the block size we expect */
	if (CALL(info->ubd, get_blocksize) != UFS_FRAGSIZE) {
		printf("Block device size is not UFS_FRAGSIZE!\n");
		return -1;
	}

	/* the superblock is in sector 16 */
	info->super_block = CALL(info->ubd, read_block, 4);
	if (!info->super_block)
	{
		printf("Unable to read superblock!\n");
		return -1;
	}

	info->super = (struct UFS_Super *) (info->super_block->ddesc->data);
	if (info->super->fs_magic != UFS_MAGIC) {
		printf("ufs_base: bad file system magic number\n");
		printf("%x\n", info->super->fs_magic);
		return -1;
	}
	if (info->super->fs_inodefmt != 2) {
		printf("ufs_base: unsupported inode format\n");
		return -1;
	}

	numblocks = CALL(info->ubd, get_numblocks);
	info->ipf = info->super->fs_inopb / info->super->fs_frag;
	info->cylstart = malloc(sizeof(uint32_t) * info->super->fs_ncg);
	if (!info->cylstart)
		return -E_NO_MEM;

	for (i = 0; i < info->super->fs_ncg; i++) {
		info->cylstart[i] = calc_cylgrp_start(object, i);
	}

	printf("Superblock size %d\n", info->super->fs_sbsize);
	printf("Superblock offset %d\n", info->super->fs_sblkno);
	printf("Number of blocks: %d, data blocks %d\n", info->super->fs_size, info->super->fs_dsize);
	printf("Block size: %d, fragment size %d, frags/block: %d\n", info->super->fs_bsize, info->super->fs_fsize, info->super->fs_frag);
	printf("Inodes per block: %d, sectors per fragment %d\n", info->super->fs_inopb, info->super->fs_nspf);
	printf("Inodes per group: %d, fragments per group %d\n", info->super->fs_ipg, info->super->fs_fpg);
	printf("Cylinder Groups: %d\n", info->super->fs_ncg);
	printf("Cylinder group offset %d, inode table offset %d\n", info->super->fs_cblkno, info->super->fs_iblkno);
	printf("cg_offset: %d, cgmask: 0x %x\n", info->super->fs_cgoffset, info->super->fs_cgmask);
	printf("internal symlink max length: %d\n", info->super->fs_maxsymlinklen);
	printf("Flags: fmod: %d, clean: %d, ronly: %d, flags: %d\n",
			info->super->fs_fmod, info->super->fs_clean, info->super->fs_ronly, info->super->fs_flags);
	printf("Superblock Cylinder Summary:\n\tDirectories: %d\n\tFree Blocks: %d\n\tFree Inodes: %d\n\tFree Frags: %d\n", info->super->fs_cstotal.cs_ndir,
			info->super->fs_cstotal.cs_nbfree, info->super->fs_cstotal.cs_nifree,
			info->super->fs_cstotal.cs_nffree);

	bdesc_retain(info->super_block);

	info->csum_block = CALL(info->ubd, read_block, info->super->fs_csaddr);
	if (!info->csum_block)
	{
		printf("Unable to read cylinder summary!\n");
		return -1;
	}

	info->csum = (struct UFS_csum *) (info->csum_block->ddesc->data);

	bdesc_retain(info->csum_block);

	return 0;
}

// [ndir, ..., nffree] parameters are deltas
static int update_summary(LFS_t * object, int cyl, int ndir, int nbfree, int nifree, int nffree, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	struct UFS_csum * csum;
	uint32_t blockno;
	int r;
	bdesc_t * cgblock;
	chdesc_t * newtail;

	if (!head || !tail || cyl < 0 || cyl >= info->super->fs_ncg)
		return -E_INVAL;

	r = read_cg(object, cyl, &cg);
	if (r < 0)
		return r;

	// Update cylinder group
	blockno = info->cylstart[cyl] + info->super->fs_cblkno;
	cgblock = CALL(info->ubd, read_block, blockno);
	if (!cgblock)
		return -E_NOT_FOUND;

	cg.cg_cs.cs_ndir += ndir;
	cg.cg_cs.cs_nbfree += nbfree;
	cg.cg_cs.cs_nifree += nifree;
	cg.cg_cs.cs_nffree += nffree;

	r = chdesc_create_byte(cgblock, info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_cs,
			sizeof(cg.cg_cs), &cg.cg_cs, head, tail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;

	// Update cylinder summary area
	csum = info->csum + cyl;
	csum->cs_ndir += ndir;
	csum->cs_nbfree += nbfree;
	csum->cs_nifree += nifree;
	csum->cs_nffree += nffree;

	r = chdesc_create_byte(info->csum_block, info->ubd,
			cyl * sizeof(struct UFS_csum), sizeof(struct UFS_csum),
			csum, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->csum_block);
	if (r < 0)
		return r;

	// Update superblock
	info->super->fs_cstotal.cs_ndir += ndir;
	info->super->fs_cstotal.cs_nbfree += nbfree;
	info->super->fs_cstotal.cs_nifree += nifree;
	info->super->fs_cstotal.cs_nffree += nffree;

	r = chdesc_create_byte(info->super_block, info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal,
			sizeof(info->super->fs_cstotal),
			&info->super->fs_cstotal, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->super_block);
	if (r < 0)
		return r;

	return 0;
}

static uint32_t read_btot(LFS_t * object, uint32_t num)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(object, num / info->super->fs_fpg, &cg);
	if (r < 0)
		return INVALID_BLOCK;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return INVALID_BLOCK;

	offset = cg.cg_btotoff + offset / 256;
	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return INVALID_BLOCK;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	return *ptr;
}

static uint16_t read_fbp(LFS_t * object, uint32_t num)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(object, num / info->super->fs_fpg, &cg);
	if (r < 0)
		return UFS_INVALID16;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return UFS_INVALID16;

	offset = cg.cg_boff + offset / 512;
	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return UFS_INVALID16;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if ((num / 1024) % 2)
		return ((*ptr >> 16) & 0xFFFF);
	return (*ptr & 0xFFFF);
}

static int read_inode_bitmap(LFS_t * object, uint32_t num)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(object, num / info->super->fs_ipg, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_ipg;
	if (offset >= cg.cg_niblk)
		return -E_INVAL;

	offset = cg.cg_iusedoff + offset / 8;
	blockno = info->cylstart[num / info->super->fs_ipg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_USED;
	return UFS_FREE;
}

static int read_fragment_bitmap(LFS_t * object, uint32_t num)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(object, num / info->super->fs_fpg, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return -E_INVAL;

	offset = cg.cg_freeoff + offset / 8;
	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_FREE;
	return UFS_USED;
}

static int read_block_bitmap(LFS_t * object, uint32_t num)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, blocknum;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	blocknum = num * info->super->fs_frag;
	r = read_cg(object, blocknum / info->super->fs_fpg, &cg);
	if (r < 0)
		return r;

	offset = num % (info->super->fs_fpg / info->super->fs_frag);
	if (offset >= cg.cg_nclusterblks)
		return -E_INVAL;

	offset = cg.cg_clusteroff + offset / 8;
	blockno = info->cylstart[blocknum / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_FREE;
	return UFS_USED;
}

static int write_btot(LFS_t * object, uint32_t num, uint32_t value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d %d\n", __FUNCTION__, num, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;

	if (!head || !tail || value > 128)
		return -E_INVAL;

	r = read_cg(object, num / info->super->fs_fpg, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return -E_INVAL;

	offset = cg.cg_btotoff + offset / 256;
	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, ROUNDDOWN32(offset, 4), 4, &value, head, tail);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	return 0;
}

static int write_fbp(LFS_t * object, uint32_t num, uint16_t value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d %d\n", __FUNCTION__, num, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;

	if (!head || !tail || value > 128)
		return -E_INVAL;

	r = read_cg(object, num / info->super->fs_fpg, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return -E_INVAL;

	offset = cg.cg_boff + offset / 512;
	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, ROUNDDOWN32(offset,2), 2, &value, head, tail);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	return 0;
}

static int write_inode_bitmap(LFS_t * object, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, * ptr;
	int r, cyl = num / info->super->fs_ipg;
	bdesc_t * block;
	chdesc_t * newtail, * ch;

	if (!head || !tail)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 1;
	else
		value = 0;

	r = read_cg(object, cyl, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_ipg;
	if (offset >= cg.cg_niblk)
		return -E_INVAL;

	offset = cg.cg_iusedoff + offset / 8;
	blockno = info->cylstart[cyl]
		+ info->super->fs_cblkno + offset / info->super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	ch = chdesc_create_bit(block, info->ubd,
			(offset % info->super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*tail = ch;
	*head = ch;

	if (value)
		r = update_summary(object, cyl, 0, 0, -1, 0, head, &newtail);
	else
		r = update_summary(object, cyl, 0, 0, 1, 0, head, &newtail);

	return r;
}

static int write_fragment_bitmap(LFS_t * object, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d %d\n", __FUNCTION__, num, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, * ptr;
	int r, nf, cyl = num / info->super->fs_fpg;
	int nfrags_before, nfrags_after, unused_before = 0, unused_after = 0;
	bdesc_t * block, * cgblock;
	chdesc_t * newtail, * ch;

	// Counting bits set in a byte via lookup table...
	// anyone know a faster/better way?
	const unsigned char BitsSetTable256[] = 
	{
		0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 
		1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
		1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
		1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
		2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
		3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
		3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
		4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
	};

	if (!head || !tail)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 0;
	else
		value = 1;

	r = read_cg(object, cyl, &cg);
	if (r < 0)
		return r;

	offset = num % info->super->fs_fpg;
	if (offset >= cg.cg_ndblk)
		return -E_INVAL;

	offset = cg.cg_freeoff + offset / 8;
	blockno = info->cylstart[cyl] + info->super->fs_cblkno
		+ offset / info->super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	nfrags_before = BitsSetTable256[(*ptr >> ROUNDDOWN32(num % 32, 8)) & 0xFF];
	if (nfrags_before == 8)
		unused_before = 1;

	blockno = info->cylstart[cyl] + info->super->fs_cblkno;
	cgblock = CALL(info->ubd, read_block, blockno);
	if (!cgblock)
		return -E_NOT_FOUND;

	ch = chdesc_create_bit(block, info->ubd,
			(offset % info->super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*tail = ch;
	*head = ch;

	nfrags_after = BitsSetTable256[(*ptr >> ROUNDDOWN32(num % 32, 8)) & 0xFF];
	if (nfrags_after == 8)
		unused_after = 1;

	if (value) { // Marked fragment as free
		if (unused_after) { // Mark the whole block as free
			r = write_block_bitmap(object, num / info->super->fs_frag,
					UFS_FREE, head, &newtail);
			if (r < 0)
				return r;
			nf = 1 - info->super->fs_frag;
		}
		else
			nf = 1;
	}
	else { // Marked fragment as used
		if (unused_before) { // Mark the whole block as used
			r = write_block_bitmap(object, num / info->super->fs_frag,
					UFS_USED, head, &newtail);
			if (r < 0)
				return r;
			nf = info->super->fs_frag - 1;
		}
		else
			nf = -1;
	}

	if (nfrags_before > 0 && nfrags_before < 8) {
		offset = (uint32_t) &((struct UFS_cg *) NULL)->cg_frsum[nfrags_before];
		cg.cg_frsum[nfrags_before]--;
		r = chdesc_create_byte(cgblock, info->ubd, (uint16_t) offset,
				sizeof(cg.cg_frsum[nfrags_before]),
				&cg.cg_frsum[nfrags_before], head, &newtail);
		if (r < 0)
			return r;

	}
	if (nfrags_after > 0 && nfrags_after < 8) {
		offset = (uint32_t) &((struct UFS_cg *) NULL)->cg_frsum[nfrags_after];
		cg.cg_frsum[nfrags_after]++;
		r = chdesc_create_byte(cgblock, info->ubd, (uint16_t) offset,
				sizeof(cg.cg_frsum[nfrags_after]),
				&cg.cg_frsum[nfrags_after], head, &newtail);
		if (r < 0)
			return r;
	}

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;

	return update_summary(object, cyl, 0, 0, 0, nf, head, &newtail);
}

// This is the 'raw' function to write the block bitmap
// You probably want allocate_wholeblock()
static int write_block_bitmap(LFS_t * object, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d %d\n", __FUNCTION__, num, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blocknum, blockno, offset, * ptr, btot;
	uint16_t fbp;
	int r, cyl;
	bdesc_t * block;
	chdesc_t * newtail, * ch;

	if (!head || !tail)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 0;
	else
		value = 1;

	blocknum = num * info->super->fs_frag;
	cyl = blocknum / info->super->fs_fpg;
	r = read_cg(object, cyl, &cg);
	if (r < 0)
		return r;

	offset = num % (info->super->fs_fpg / info->super->fs_frag);
	if (offset >= cg.cg_nclusterblks)
		return -E_INVAL;

	offset = cg.cg_clusteroff + offset / 8;
	blockno = info->cylstart[cyl] + info->super->fs_cblkno
		+ offset / info->super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	ch = chdesc_create_bit(block, info->ubd,
			(offset % info->super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*tail = ch;
	*head = ch;

	if (value) {
		btot = read_btot(object, blocknum) + 1;
		fbp = read_fbp(object, blocknum) + 1;
	}
	else {
		btot = read_btot(object, blocknum) - 1;
		fbp = read_fbp(object, blocknum) - 1;
	}
	r = write_btot(object, blocknum, btot, head, &newtail);
	if (r < 0)
		return r;
	r = write_fbp(object, blocknum, fbp, head, &newtail);
	if (r < 0)
		return r;


	if (value)
		r = update_summary(object, cyl, 0, 1, 0, 0, head, &newtail);
	else
		r = update_summary(object, cyl, 0, -1, 0, 0, head, &newtail);

	return r;
}

// FIXME this is a fairly inefficient way to scan for free blocks
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t find_free_block_linear(LFS_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free block
	for (num = 0; num < info->super->fs_size / info->super->fs_frag; num++) {
		r = read_block_bitmap(object, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num; // returns a block number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free fragments
// we should take advantage of cylinder group summaries
// and possibly even file and purpose.
static uint32_t find_free_frag_linear(LFS_t * object, fdesc_t * file, int purpose)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free fragment
	for (num = 0; num < info->super->fs_size; num++) {
		r = read_fragment_bitmap(object, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
	return num; // returns a fragment number
	}

	return INVALID_BLOCK;
}

// FIXME this is a fairly inefficient way to scan for free inodes
static uint32_t find_free_inode_linear(LFS_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t num;
	int r;

	// Find free inode
	for (num = 0; num < info->super->fs_ipg * info->super->fs_ncg; num++) {
		r = read_inode_bitmap(object, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
			return num; // returns a inode number
	}

	return INVALID_BLOCK;
}

// Find a free block and allocate all fragments in the block
static uint32_t allocate_wholeblock(LFS_t * object, int wipe, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	int r;
	bool synthetic;
	uint32_t i, num;
	bdesc_t * block;
	chdesc_t * newtail;

	if (!head || !tail)
		return INVALID_BLOCK;

	num = find_free_block_linear(object, file, 0);
	if (num == INVALID_BLOCK)
		return INVALID_BLOCK;

	// Mark the fragments as used
	for (i = num * info->super->fs_frag; i < (num + 1) * info->super->fs_frag; i++) {
		if (i == num * info->super->fs_frag)
			r = write_fragment_bitmap(object, i, UFS_USED, head, tail);
		else
			r = write_fragment_bitmap(object, i, UFS_USED, head, &newtail);
		if (r < 0)
			return INVALID_BLOCK;
		assert(r != 1); // This should not happen

		if (wipe) {
			block = CALL(info->ubd, synthetic_read_block, i, &synthetic);
			// FIXME revert all previously allocated blocks?
			if (!block)
				return INVALID_BLOCK;
			r = chdesc_create_init(block, info->ubd, head, &newtail);
			if (r >= 0)
				r = CALL(info->ubd, write_block, block);
			if (r < 0)
				return INVALID_BLOCK;
		}
	}

	if (file) {
		f->file->f_inode.di_blocks += 32; // charge the fragments to the file
		r = write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
		if (r < 0)
			return INVALID_BLOCK;
	}

	return num * info->super->fs_frag;
}

// Deallocate an entire block
static int erase_wholeblock(LFS_t * object, uint32_t num, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	int r;
	uint32_t i;
	chdesc_t * newtail;

	if (!head || !tail || num == INVALID_BLOCK)
		return -E_INVAL;

	// Mark the fragments as used
	for (i = num * info->super->fs_frag; i < (num + 1) * info->super->fs_frag; i++) {
		if (i == num * info->super->fs_frag)
			r = write_fragment_bitmap(object, i, UFS_FREE, head, tail);
		else
			r = write_fragment_bitmap(object, i, UFS_FREE, head, &newtail);
		if (r < 0)
			return r;
		assert(r != 1); // This should not happen
	}

	if (file) {
		f->file->f_inode.di_blocks -= 32; // charge the fragments to the file
		r = write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
		if (r < 0)
			return r;
	}

	return 0;
}

// Update a ptr in an indirect ptr block
static int update_indirect_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t n, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int r;

	r = chdesc_create_byte(block, info->ubd, offset * sizeof(n), sizeof(n), &n, head, tail);
	if (r < 0)
		return r;
	return CALL(info->ubd, write_block, block);
}

// Update file's inode with an nth indirect ptr
static int modify_indirect_ptr(LFS_t * object, fdesc_t * file, int n, bool evil, chdesc_t ** head, chdesc_t ** tail)
{
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t newblock;
	chdesc_t * newtail;

	if (!file || !head || !tail || n < 0 || n >= UFS_NIADDR)
		return -E_INVAL;

	// Beware of the evil bit? ;)
	if (evil) {
		// Clears the indirect pointer...
		f->file->f_inode.di_ib[n] = 0;
		return write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
	}
	else {
		// Allocates an indirect pointer block
		if (f->file->f_inode.di_ib[n])
			return -E_UNSPECIFIED;

		newblock = allocate_wholeblock(object, 1, file, head, tail);
		if (newblock == INVALID_BLOCK)
			return -E_NOT_FOUND;
		f->file->f_inode.di_ib[n] = newblock;
		return write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
	}
}

// Write the block ptrs for a file, allocate indirect blocks as needed
// Offset is a byte offset
static int write_block_ptr(LFS_t * object, fdesc_t * file, uint32_t offset, uint32_t value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %x %d %d\n", __FUNCTION__, file, offset, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	int r;
	uint32_t blockno, nindirb, nindirf, newblock;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];
	chdesc_t * tmptail;
	chdesc_t ** newtail = tail;

	if (!head || !tail || !file || offset % info->super->fs_bsize)
		return -E_INVAL;

	nindirb = info->super->fs_nindir;
	nindirf = nindirb / info->super->fs_frag;
	blockno = offset / info->super->fs_bsize;

	if (blockno < UFS_NDADDR) {
		f->file->f_inode.di_db[blockno] = value;
		return write_inode(object, f->file->f_num, f->file->f_inode, head, tail);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		// Allocate single indirect block if needed
		if (!f->file->f_inode.di_ib[0]) {
			r = modify_indirect_ptr(object, file, 0, 0, head, newtail);
			if (r < 0)
				return r;
			newtail = &tmptail;
		}

		indirect[0] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		return update_indirect_block(object, indirect[0], pt_off[0], value, head, newtail);
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		// Allocate double indirect block if needed
		if (!f->file->f_inode.di_ib[1]) {
			r = modify_indirect_ptr(object, file, 1, 0, head, newtail);
			if (r < 0)
				return r;
			newtail = &tmptail;
		}

		indirect[1] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[1] + frag_off[1]);
		if (!indirect[1])
			return -E_NOT_FOUND;

		block_off[0] = *((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1]);

		// Allocate single indirect block if needed
		if (!block_off[0]) {
			newblock = allocate_wholeblock(object, 1, file, head, newtail);
			if (newblock == INVALID_BLOCK)
				return -E_NOT_FOUND;
			newtail = &tmptail;
			r = update_indirect_block(object, indirect[1], pt_off[1], newblock, head, newtail);
			if (r < 0)
				return r;
		}

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		return update_indirect_block(object, indirect[0], pt_off[0], value, head, newtail);
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		// We'll only need triple indirect ptrs when the filesize is:
		//  4 KB Blocksize: > 4GB
		//  8 KB Blocksize: > 32GB
		// 16 KB Blocksize: > 256GB

		// FIXME write some tedious code
	}

	return -E_UNSPECIFIED;
}

// Erase the block ptrs for a file, deallocate indirect blocks as needed
// Offset is a byte offset
static int erase_block_ptr(LFS_t * object, fdesc_t * file, uint32_t offset, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %x %d\n", __FUNCTION__, file, offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	int r;
	uint32_t blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	uint32_t num[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];
	chdesc_t * tmptail;
	chdesc_t ** newtail = tail;

	if (!head || !tail || !file || offset % info->super->fs_bsize)
		return -E_INVAL;

	nindirb = info->super->fs_nindir;
	nindirf = nindirb / info->super->fs_frag;
	blockno = offset / info->super->fs_bsize;

	if (blockno < UFS_NDADDR) {
		f->file->f_inode.di_db[blockno] = 0;
		return write_inode(object, f->file->f_num, f->file->f_inode, head, tail);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;
		num[0] = f->file->f_inode.di_ib[0] / info->super->fs_frag;

		indirect[0] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		r = update_indirect_block(object, indirect[0], pt_off[0], 0, head, newtail);
		// Deallocate indirect block if necessary
		if (blockno == UFS_NDADDR && r >= 0) {
			newtail = &tmptail;
			r = modify_indirect_ptr(object, file, 0, 1, head, newtail);
			if (r >= 0)
				r = erase_wholeblock(object, num[0], file, head, newtail);
		}
		return r;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;
		num[1] = f->file->f_inode.di_ib[1] / info->super->fs_frag;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		indirect[1] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[1] + frag_off[1]);
		if (!indirect[1])
			return -E_NOT_FOUND;

		block_off[0] = *((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1]);
		num[0] = block_off[0] / info->super->fs_frag;

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		r = update_indirect_block(object, indirect[0], pt_off[0], 0, head, newtail);
		newtail = &tmptail;

		// Deallocate indirect block if necessary
		if ((block_off[1] % nindirb == 0) && r >= 0) {
			r = update_indirect_block(object, indirect[1], pt_off[1], 0, head, newtail);
			if (r >= 0)
				r = erase_wholeblock(object, num[0], file, head, newtail);
		}

		// Deallocate double indirect block if necessary
		if (blockno == UFS_NDADDR + nindirb && r >= 0) {
			r = modify_indirect_ptr(object, file, 1, 1, head, newtail);
			if (r >= 0)
				r = erase_wholeblock(object, num[1], file, head, newtail);
		}

		return r;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		// We'll only need triple indirect ptrs when the filesize is:
		//  4 KB Blocksize: > 4GB
		//  8 KB Blocksize: > 32GB
		// 16 KB Blocksize: > 256GB

		// FIXME write some tedious code
	}

	return -E_UNSPECIFIED;
}

static uint32_t count_free_space(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	return info->super->fs_cstotal.cs_nbfree * info->super->fs_frag
		+ info->super->fs_cstotal.cs_nffree;
}

// Skip over slashes.
static inline const char* skip_slash(const char* p)
{
	while (*p == '/')
		p++;
	return p;
}

// file is the directory
static int read_dirent(LFS_t * object, fdesc_t * file, struct UFS_direct * entry, uint32_t * basep)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, *basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	struct UFS_direct * dirfile;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, offset;

	if (!entry)
		return -E_INVAL;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	if (*basep >= f->file->f_inode.di_size)
		return -E_EOF;

	blockno = ufs_get_file_block(object, file, ROUNDDOWN32(*basep, info->super->fs_fsize));
	if (blockno != INVALID_BLOCK)
		dirblock = ufs_lookup_block(object, blockno);
	if (!dirblock)
		return -E_NOT_FOUND;

	offset = *basep % info->super->fs_fsize;
	dirfile = (struct UFS_direct *) (dirblock->ddesc->data + offset);

	if (offset + dirfile->d_reclen > info->super->fs_fsize
			|| dirfile->d_reclen < dirfile->d_namlen)
		return -E_UNSPECIFIED;

	entry->d_ino = dirfile->d_ino;
	entry->d_reclen = dirfile->d_reclen;
	entry->d_type = dirfile->d_type;
	entry->d_namlen = dirfile->d_namlen;
	strncpy(entry->d_name, dirfile->d_name, dirfile->d_namlen);
	entry->d_name[dirfile->d_namlen] = 0;

	*basep += dirfile->d_reclen;
	return 0;
}

// Writes a directory entry, does not check for free space
static int write_dirent(LFS_t * object, fdesc_t * file, struct UFS_direct entry, uint32_t basep, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * block;
	uint32_t foffset, blockno;
	uint16_t offset, actual_len;
	int r;

	if (!head || !tail || !file)
		return -E_INVAL;

	actual_len = sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN;

	offset = basep % info->super->fs_fsize;
	foffset = basep - offset;
	blockno = ufs_get_file_block(object, file, foffset);
	if (blockno == INVALID_BLOCK)
		return -E_NOT_FOUND;
	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, offset, actual_len,
			&entry, head, tail);
	if (r < 0)
		return r;

	return CALL(info->ubd, write_block, block);
}

// file is the one we are removing
static int erase_dirent(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct UFS_direct last_entry, entry;
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	struct ufs_fdesc dirfdesc;
	struct UFS_File dirfile;
	fdesc_t * dirf = (fdesc_t *) &dirfdesc;
	uint32_t basep = 0, last_basep, p;
	int r;

	if (!head || !tail || !file)
		return -E_INVAL;

	// FIXME perhaps we should read in the full fdesc?
	// instead of using this synthetic version?
	dirfdesc.file = &dirfile;
	dirfile.f_type = TYPE_DIR;
	r = read_inode(object, f->dir_inode, &dirfile.f_inode);
	if (r < 0)
		return r;

	if (f->dir_offset % 512 == 0) {
		// We are the first entry in the fragment
		p = f->dir_offset;
		r = read_dirent(object, dirf, &entry, &p);
		if (r < 0)
			return r;

		entry.d_ino = 0;
		return write_dirent(object, dirf, entry, f->dir_offset, head, tail);
	}

	// Find the entry in front of us
	do {
		last_basep = basep;
		r = read_dirent(object, dirf, &last_entry, &basep);
		if (r < 0)
			return r;
	} while (basep < f->dir_offset);

	// we went past the entry somehow?
	if (basep != f->dir_offset) {
		printf("%s: went past the directory entry\n", __FUNCTION__);
		return -E_UNSPECIFIED;
	}

	// Get our entry
	p = basep;
	r = read_dirent(object, dirf, &entry, &p);
	if (r < 0)
		return r;

	last_entry.d_reclen += entry.d_reclen;

	return write_dirent(object, dirf, last_entry, last_basep, head, tail);
}

static int search_dirent(LFS_t * object, fdesc_t * file, uint32_t len)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, len);
	struct UFS_direct entry;
	uint32_t basep = 0, last_basep, actual_len;
	int r;

	if (!file)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + len - UFS_MAXNAMELEN, 4);

	while (1) {
		last_basep = basep;
		r = read_dirent(object, file, &entry, &basep);
		if (r < 0 && r != -E_EOF)
			return r;
		if (r == -E_EOF) // EOF, return where next entry starts
			return basep;

		if (entry.d_ino) {
			actual_len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);
			if (entry.d_reclen - actual_len >= len)
				return last_basep; // return entry to insert after
		}
		else {
			if (entry.d_reclen >= len)
				return last_basep; // return blank entry location
		}
	}
}

static int insert_dirent(LFS_t * object, fdesc_t * dir_file, fdesc_t * new_file, int * offset, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, *offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * df = (struct ufs_fdesc *) dir_file;
	struct ufs_fdesc * nf = (struct ufs_fdesc *) new_file;
	struct UFS_direct entry, last_entry;
	chdesc_t * tmptail;
	chdesc_t ** newtail = tail;
	uint32_t len, last_len, blockno, newsize = *offset + 512;
	int r, p = *offset, alloc = 0;
	uint8_t fs_type;

	if (!head || !tail || !df || !nf || *offset < 0)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);

	switch(nf->file->f_type)
	{
		case TYPE_FILE:
			fs_type = UFS_DT_REG;
			break;
		case TYPE_DIR:
			fs_type = UFS_DT_DIR;
			break;
		case TYPE_SYMLINK:
			fs_type = UFS_DT_LNK;
			break;
			// case TYPE_DEVICE: ambiguous
		default:
			return -E_INVAL;
	}

	entry.d_type = fs_type;
	entry.d_ino = nf->file->f_num;
	entry.d_namlen = strlen(nf->filename);
	strcpy(entry.d_name, nf->filename);
	entry.d_name[entry.d_namlen] = 0;

	// Need to extend directory
	if (*offset >= df->file->f_inode.di_size) {
		// Need to allocate/append fragment
		if (*offset % info->super->fs_fsize == 0) {
			blockno = ufs_allocate_block(object, dir_file, 0, head, newtail);
			if (blockno == INVALID_BLOCK)
				return -E_UNSPECIFIED;
			newtail = &tmptail;
			r = ufs_append_file_block(object, dir_file, blockno, head, newtail);
			if (r < 0)
				return r;
		}

		// Set directory size
		r = ufs_set_metadata(object, df, KFS_feature_size.id, sizeof(uint32_t), &newsize, head, newtail);
		if (r < 0)
			return r;
		alloc = 1;
	}

	r = read_dirent(object, dir_file, &last_entry, &p);
	if (r < 0)
		return r;

	// Inserting after existing entry
	if (!alloc && last_entry.d_ino) {
		last_len = ROUNDUP32(sizeof(struct UFS_direct) + last_entry.d_namlen - UFS_MAXNAMELEN, 4);
		entry.d_reclen = last_entry.d_reclen - last_len;
		r = write_dirent(object, dir_file, entry, *offset + last_len, head, newtail);
		if (r < 0)
			return r;
		newtail = &tmptail;
		last_entry.d_reclen = last_len;
		r = write_dirent(object, dir_file, last_entry, *offset, head, newtail);
		*offset += last_len;
		return r;
	}
	else {
		if (alloc) // Writing to new fragment
			entry.d_reclen = 512;
		else // Overwriting blank entry
			entry.d_reclen = last_entry.d_reclen;
		return write_dirent(object, dir_file, entry, *offset, head, newtail);
	}
}

static open_ufsfile_t * open_ufsfile_create(UFS_File_t * file)
{
	open_ufsfile_t * uf;
	if (file) {
		uf = malloc(sizeof(open_ufsfile_t));
		if (uf) {
			uf->file = file;
			uf->count = 1;
			return uf;
		}
	}
	return NULL;
};

static void open_ufsfile_destroy(open_ufsfile_t * uf)
{
	if (uf) {
		if (uf->count < 2) {
			if (uf->count < 1)
				printf("%s: warning, count below 1 (%d)\n", __FUNCTION__, uf->count);
			free(uf->file);
			uf->count = 0;
			uf->file = NULL;
			free(uf);
		}
		else
			uf->count--;
	}
}

static void get_parent_path(const char * path, char * parent)
{
	int i;
	int len = strlen(path);
	strcpy(parent, path);

	while (parent[len-1] == '/') {
		parent[len-1] = 0;
		len--;
	}

	for (i = len - 1; i >= 0; i--)
		if (parent[i] == '/')
			break;

	while (parent[i] == '/')
		parent[i--] = 0;

}

static int dir_lookup(LFS_t * object, struct UFS_File dir, const char * name, struct ufs_fdesc * new_fdesc)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t basep = 0, last_basep;
	struct dirent entry;
	struct ufs_fdesc temp_fdesc;
	open_ufsfile_t * existing_file;
	int r = 0;

	if (!new_fdesc || !name)
		return -E_INVAL;

	temp_fdesc.file = &dir;
	while (r >= 0) {
		last_basep = basep;
		r = get_dirent(object, (fdesc_t *) &temp_fdesc, &entry, sizeof(struct dirent), &basep, 0);
		if (r < 0)
			return r;
		if (entry.d_fileno == 0) // Blank spot
			continue;
		if (!strcmp(entry.d_name, name)) {
			new_fdesc->dir_inode = dir.f_num;
			new_fdesc->dir_offset = last_basep;

			// Look up existing file struct
			existing_file = hash_map_find_val(info->filemap, (void *) entry.d_fileno);
			if (existing_file) {
				new_fdesc->file = existing_file->file;
				existing_file->count++;
				return 0;
			}

			new_fdesc->file = malloc(sizeof(UFS_File_t));
			if (!new_fdesc->file)
				return -E_NO_MEM;

			// If file struct is not in memory
			existing_file = open_ufsfile_create(new_fdesc->file);
			if (!existing_file)
				return -E_NO_MEM;

			strcpy(new_fdesc->filename, name);
			new_fdesc->file->f_type = entry.d_type;
			new_fdesc->file->f_num = entry.d_fileno;
			r = read_inode(object, entry.d_fileno, &new_fdesc->file->f_inode);
			if (r < 0)
				return r;
			new_fdesc->file->f_lastalloc = INVALID_BLOCK;
			new_fdesc->file->f_numfrags = ufs_get_file_numblocks(object, (fdesc_t *) new_fdesc);
			if (new_fdesc->file->f_numfrags)
				new_fdesc->file->f_lastfrag = ufs_get_file_block(object, (fdesc_t *) new_fdesc, (new_fdesc->file->f_numfrags - 1) * info->super->fs_fsize);
			else
				new_fdesc->file->f_lastfrag = 0;
			if (new_fdesc->file->f_lastfrag == INVALID_BLOCK)
				return -E_UNSPECIFIED;
			assert(hash_map_insert(info->filemap, (void *) entry.d_fileno, existing_file) == 0);
			return 0;
		}
	}

	return 0;
}

static int walk_path(LFS_t * object, const char * path, struct ufs_fdesc * new_fdesc)
{
	printf("UFSDEBUG: %s %s\n", __FUNCTION__, path);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	open_ufsfile_t * existing_file;
	struct UFS_File dir;
	const char* p;
	char name[UFS_MAXNAMELEN];
	int r;

	if (!new_fdesc || !path)
		return -E_INVAL;

	strncpy(new_fdesc->fullpath, path, UFS_MAXPATHLEN);
	dir.f_num = UFS_ROOT_INODE;
	dir.f_type = TYPE_DIR;
	r = read_inode(object, UFS_ROOT_INODE, &dir.f_inode);
	if (r < 0)
		return r;

	path = skip_slash(path);
	name[0] = 0;

	// Special case of root
	if (path[0] == 0) {
		new_fdesc->dir_inode = 0;
		new_fdesc->dir_offset = 0;
		new_fdesc->fullpath[0] = 0;
		new_fdesc->filename[0] = 0;

		// Look up existing file struct
		existing_file = hash_map_find_val(info->filemap, (void *) UFS_ROOT_INODE);
		if (existing_file) {
			new_fdesc->file = existing_file->file;
			existing_file->count++;
			return 0;
		}

		new_fdesc->file = malloc(sizeof(UFS_File_t));
		if (!new_fdesc->file)
			return -E_NO_MEM;

		// If file struct is not in memory
		existing_file = open_ufsfile_create(new_fdesc->file);
		if (!existing_file)
			return -E_NO_MEM;

		new_fdesc->file->f_type = TYPE_DIR;
		new_fdesc->file->f_num = UFS_ROOT_INODE;
		memcpy(&new_fdesc->file->f_inode, &dir.f_inode, sizeof(struct UFS_dinode));
		new_fdesc->file->f_lastalloc = INVALID_BLOCK;
		new_fdesc->file->f_numfrags = ufs_get_file_numblocks(object, (fdesc_t *) new_fdesc);
		if (new_fdesc->file->f_numfrags)
			new_fdesc->file->f_lastfrag = ufs_get_file_block(object, (fdesc_t *) new_fdesc, (new_fdesc->file->f_numfrags - 1) * info->super->fs_fsize);
		else
			new_fdesc->file->f_lastfrag = 0;
		if (new_fdesc->file->f_lastfrag == INVALID_BLOCK)
			return -E_UNSPECIFIED;
		assert(hash_map_insert(info->filemap, (void *) UFS_ROOT_INODE, existing_file) == 0);
		return 0;
	}

	while (*path != 0) {
		p = path;
		while (*path != '/' && *path != '\0')
			path++;
		if (path - p >= UFS_MAXNAMELEN)
			return -E_BAD_PATH;
		memcpy(name, p, path - p);
		name[path - p] = 0;
		path = skip_slash(path);

		if (dir.f_type != TYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(object, dir, name, new_fdesc)) < 0)
			return r;
		memcpy(&dir, new_fdesc->file, sizeof(struct UFS_File));
	}

	return 0;
}

static int ufs_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != UFS_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int ufs_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != UFS_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static uint32_t ufs_get_blocksize(LFS_t * object)
{
	return ((struct lfs_info *) OBJLOCAL(object))->super->fs_fsize;
}

static BD_t * ufs_get_blockdev(LFS_t * object)
{
	return ((struct lfs_info *) OBJLOCAL(object))->ubd;
}

static uint32_t find_frags_new_home(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t i, blockno, offset;
	int r, frags;
	bool synthetic = 0;
	chdesc_t * newtail;
	bdesc_t * block, * newblock;

	if (!head || !tail || !file)
		return INVALID_BLOCK;

	frags = f->file->f_numfrags % info->super->fs_frag;
	offset = (f->file->f_numfrags - frags) * info->super->fs_size;

	// Time to allocate a new block and copy the data there
	// FIXME handle failure case better?

	// find new block
	blockno = find_free_block_linear(object, file, purpose);
	if (blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	blockno *= info->super->fs_frag;

	// allocate some fragments
	for (i = 0 ; i < frags; i++) {
		if (i == 0)
			r = write_fragment_bitmap(object, blockno, UFS_USED, head, tail);
		else
			r = write_fragment_bitmap(object, blockno + i, UFS_USED, head, &newtail);
		if (r != 0)
			return INVALID_BLOCK;
	}

	// read in fragments, and write to new location
	for (i = 0 ; i < frags; i++) {
		block = CALL(info->ubd, read_block, f->file->f_lastfrag - frags + i + 1);
		if (!block)
			return INVALID_BLOCK;
		newblock = CALL(info->ubd, synthetic_read_block, blockno + i, &synthetic);
		if (!newblock)
			return INVALID_BLOCK;

		r = chdesc_create_full(newblock, info->ubd, block->ddesc->data, head, &newtail);
		if (r >= 0)
			r = CALL(info->ubd, write_block, newblock);
		if (r < 0)
			return INVALID_BLOCK;
	}

	// update block ptr
	r = write_block_ptr(object, file, offset, blockno, head, &newtail);

	// free old fragments
	for (i = 0 ; i < frags; i++) {
		r = write_fragment_bitmap(object, f->file->f_lastfrag - frags + i + 1, UFS_FREE, head, &newtail);
		if (r != 0)
			return INVALID_BLOCK;
	}

	blockno = blockno + frags;
	f->file->f_lastfrag = blockno - 1;

	return blockno;
}

// Allocates fragments, really
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	bdesc_t * block;
	uint32_t blockno;
	bool synthetic = 0, use_newtail = 0;
	chdesc_t * newtail;
	int r;

	// FIXME require file to be non-null for now
	if (!head || !tail || !file)
		return INVALID_BLOCK;

	if (f->file->f_lastalloc != INVALID_BLOCK)
		// We already allocated a fragment, go use that first
		return INVALID_BLOCK;

	// File has no fragments
	if (f->file->f_numfrags == 0) {
		blockno = find_free_block_linear(object, file, purpose);
		if (blockno == INVALID_BLOCK)
			return INVALID_BLOCK;
		blockno *= info->super->fs_frag;
	}
	// We're using indirect pointers, time to allocate whole blocks
	else if (f->file->f_numfrags >= UFS_NDADDR * info->super->fs_frag) {
		// Well, except we're still working with fragments here

		// Time to allocate a find a new block
		if (((f->file->f_lastfrag + 1) % info->super->fs_frag) == 0) {
			blockno = allocate_wholeblock(object, 1, file, head, tail);
			f->file->f_lastalloc = blockno;
			return blockno;
		}
		// Use the next fragment (everything was zeroed out already)
		else {
			blockno = f->file->f_lastfrag + 1;
			f->file->f_lastalloc = blockno;
			return blockno;
		}
	}
	// Time to allocate a find a new block
	else if (((f->file->f_lastfrag + 1) % info->super->fs_frag) == 0) {
		if (f->file->f_numfrags % info->super->fs_frag) {
			blockno = find_frags_new_home(object, file, purpose, head, tail);
			use_newtail = 1;
		}
		else {
			blockno = find_free_block_linear(object, file, purpose);
			if (blockno == INVALID_BLOCK)
				return INVALID_BLOCK;
			blockno *= info->super->fs_frag;
		}
	}
	// Use the next fragment
	else {
		r = read_fragment_bitmap(object, f->file->f_lastfrag + 1);
		if (r < 0)
			return r;
		else if (r == UFS_FREE)
			blockno = f->file->f_lastfrag + 1; // UFS says we must use it
		else // Next fragment is taken, move elsewhere
		{
			blockno = find_frags_new_home(object, file, purpose, head, tail);
			use_newtail = 1;
		}
	}

	if (use_newtail)
		r = write_fragment_bitmap(object, blockno, UFS_USED, head, &newtail);
	else
		r = write_fragment_bitmap(object, blockno, UFS_USED, head, tail);
	if (r != 0)
		return INVALID_BLOCK;

	assert(read_fragment_bitmap(object, blockno) == UFS_USED);
	block = CALL(info->ubd, synthetic_read_block, blockno, &synthetic);
	if (!block)
		goto allocate_block_cleanup;

	r = chdesc_create_init(block, info->ubd, head, &newtail);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		goto allocate_block_cleanup;
	
	f->file->f_inode.di_blocks += 4; // grr, di_blocks counts 512 byte blocks
	r = write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
	if (r < 0)
		return INVALID_BLOCK;

	f->file->f_lastalloc = blockno;
	return blockno;

allocate_block_cleanup:
	r = write_fragment_bitmap(object, blockno, UFS_FREE, head, &newtail);
	assert(r == 0);
	return INVALID_BLOCK;
}

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, read_block, number);
}

static bdesc_t * ufs_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, synthetic_read_block, number, synthetic);
}

static int ufs_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, cancel_block, number);
}

static fdesc_t * ufs_lookup_name(LFS_t * object, const char * name)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	struct ufs_fdesc * temp_fdesc = malloc(sizeof(struct ufs_fdesc));
	if (!temp_fdesc)
		return NULL;

	if (walk_path(object, name, temp_fdesc) == 0)
		return (fdesc_t *) temp_fdesc;

	free(temp_fdesc);
	return NULL;
}

static void ufs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("UFSDEBUG: %s %x\n", __FUNCTION__, fdesc);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) fdesc;
	open_ufsfile_t * uf;

	if (f) {
		if (f->file) {
			uf = hash_map_find_val(info->filemap, (void *) f->file->f_num);
			if (uf->count < 2)
				hash_map_erase(info->filemap, (void *) f->file->f_num);
			open_ufsfile_destroy(uf);
		}
		free(f);
	}
}

static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	Dprintf("UFSDEBUG: %s %x\n", __FUNCTION__, file);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t n;

	n = f->file->f_inode.di_size / info->super->fs_fsize;
	if (f->file->f_inode.di_size % info->super->fs_fsize)
		n++;

	return n;
}

// Offset is a byte offset
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("UFSDEBUG: %s %x %d\n", __FUNCTION__, file, offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t fragno, blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];

	if (offset % info->super->fs_fsize || offset >= f->file->f_inode.di_size)
		return INVALID_BLOCK;

	nindirb = info->super->fs_nindir;
	nindirf = nindirb / info->super->fs_frag;
	blockno = offset / info->super->fs_bsize;
	fragno = (offset / info->super->fs_fsize) % info->super->fs_frag;

	if (blockno < UFS_NDADDR) {
		return f->file->f_inode.di_db[blockno] + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		indirect[0] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		return (*((uint32_t *) (indirect[0]->ddesc->data) + pt_off[0])) + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		indirect[1] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[1] + frag_off[1]);
		if (!indirect[1])
			return -E_NOT_FOUND;

		block_off[0] = *((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1]);

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;

		return (*((uint32_t *) (indirect[0]->ddesc->data) + pt_off[0])) + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		// We'll only need triple indirect ptrs when the filesize is:
		//  4 KB Blocksize: > 4GB
		//  8 KB Blocksize: > 32GB
		// 16 KB Blocksize: > 256GB

		/* although I think this will work...
		block_off[2] = blockno - UFS_NDADDR - nindirb * nindirb;
		frag_off[2] = block_off[2] / nindirf / nindirb / nindirb;
		pt_off[2] = (block_off[2] / nindirb / nindirb) % nindirf;

		frag_off[1] = block_off[2] / nindirf / nindirb;
		pt_off[1] = (block_off[2] / nindirb) % nindirf;

		frag_off[0] = (block_off[2] % nindirb) / nindirf;
		pt_off[0] = block_off[2] % nindirf;

		indirect[2] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[2] + frag_off[2]);
		if (!indirect[2])
			return -E_NOT_FOUND;

		block_off[1] = *((uint32_t *) (indirect[2]->ddesc->data) + pt_off[2]);

		indirect[1] = CALL(info->ubd, read_block, block_off[1] + frag_off[1]);
		if (!indirect[1])
			return -E_NOT_FOUND;

		block_off[0] = *((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1]);

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_NOT_FOUND;
		return (*((uint32_t *) (indirect[0]->ddesc->data) + pt_off[0])) + fragno;
		*/
	}

	return -E_UNSPECIFIED;
}

static int get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep, const bool skip)
{
	struct UFS_direct dirfile;
	struct UFS_dinode inode;
	uint32_t actual_len, p;
	int r;

	if (!entry)
		return -E_INVAL;

	p = *basep;
	do {
		r = read_dirent(object, file, &dirfile, &p);
		if (r < 0)
			return r;
	} while (skip && dirfile.d_ino == 0);

	actual_len = sizeof(struct dirent) + dirfile.d_namlen - DIRENT_MAXNAMELEN;
	if (size < actual_len)
		return -E_INVAL;

	r = read_inode(object, dirfile.d_ino, &inode); 
	if (r < 0)
		return r;

	if (inode.di_size > UFS_MAXFILESIZE) {
		printf("%s: file too big?\n", __FUNCTION__);
		inode.di_size &= UFS_MAXFILESIZE;
	}
	entry->d_filesize = inode.di_size;

	switch(dirfile.d_type)
	{
		case UFS_DT_REG:
			entry->d_type = TYPE_FILE;
			break;
		case UFS_DT_DIR:
			entry->d_type = TYPE_DIR;
			break;
		case UFS_DT_LNK:
			entry->d_type = TYPE_SYMLINK;
			break;
		case UFS_DT_CHR:
		case UFS_DT_BLK:
			entry->d_type = TYPE_DEVICE;
			break;
		default:
			entry->d_type = TYPE_INVAL;
	}
	entry->d_fileno = dirfile.d_ino;
	entry->d_reclen = actual_len;
	entry->d_namelen = dirfile.d_namlen;
	strncpy(entry->d_name, dirfile.d_name, dirfile.d_namlen);
	entry->d_name[dirfile.d_namlen] = 0;

	*basep = p;
	return 0;
}

static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return get_dirent(object, file, entry, size, basep, 1);
}

static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t offset;
	int r;

	if (!head || !tail || !f || block == INVALID_BLOCK)
		return -E_INVAL;

	if (block != f->file->f_lastalloc)
		// hmm, that's not the right block
		return -E_UNSPECIFIED;

	if (f->file->f_numfrags % info->super->fs_frag) {
		// not appending to a new block,
		// the fragment has been attached implicitly
		f->file->f_numfrags++;
		f->file->f_lastfrag = block;
		f->file->f_lastalloc = INVALID_BLOCK;

		return 0;
	}

	offset = f->file->f_numfrags * info->super->fs_fsize;
	r = write_block_ptr(object, file, offset, block, head, tail);
	if (r < 0)
		return r;

	f->file->f_numfrags++;
	f->file->f_lastfrag = block;
	f->file->f_lastalloc = INVALID_BLOCK;

	return 0;
}

static fdesc_t * ufs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * fdesc, * new_fdesc;
	struct ufs_fdesc *f, * nf;
	open_ufsfile_t * new_file;
	struct ufs_fdesc * ln = (struct ufs_fdesc *) link;
	const char * filename;
	char pname[UFS_MAXPATHLEN];
	uint32_t inum = 0;
	int r, offset, notdot = 1;
	uint16_t mode;
	chdesc_t * newtail;

	if (!head || !tail || !name)
		return NULL;
	switch (type)
	{
		case TYPE_FILE:
			mode = UFS_IFREG;
			break;
		case TYPE_DIR:
			mode = UFS_IFDIR;
			break;
		default:
			return NULL;
	}

	// Don't link files of different types
	if (ln && type != ln->file->f_type)
		return NULL;

	get_parent_path(name, pname);
	filename = skip_slash(name + strlen(pname));

	// Don't create directory hard links, except for . and ..
	if (!strcmp(filename, "."))
		notdot = 0;
	else if (!strcmp(filename, ".."))
		notdot = 0;

	if (ln && notdot && type == TYPE_DIR)
		return NULL;

	fdesc = ufs_lookup_name(object, name);
	if (fdesc) { // File exists already
		ufs_free_fdesc(object, fdesc);
		return NULL;
	}

	// Get parent directory
	fdesc = ufs_lookup_name(object, pname);
	f = (struct ufs_fdesc *) fdesc;
	if (!fdesc)
		return NULL;

	// Find an empty slot to write into
	offset = search_dirent(object, fdesc, strlen(filename) + 1);
	if (offset < 0)
		goto ufs_allocate_name_exit;

	if (!ln) {
		// Allocate new inode
		inum = find_free_inode_linear(object, fdesc);
		if (inum == INVALID_BLOCK)
			goto ufs_allocate_name_exit;
	}

	// Create data structures, fill in fields
	new_fdesc = malloc(sizeof(struct ufs_fdesc));
	nf = (struct ufs_fdesc *) new_fdesc;
	if (!new_fdesc)
		goto ufs_allocate_name_exit;

	if (!ln) {
		nf->file = malloc(sizeof(UFS_File_t));
		if (!nf->file)
			goto ufs_allocate_name_exit2;

		new_file = open_ufsfile_create(nf->file);
		if (!new_file)
			goto ufs_allocate_name_exit2;

		nf->dir_inode = f->file->f_num;
		nf->file->f_numfrags = 0;
		nf->file->f_lastfrag = 0;
		nf->file->f_lastalloc = INVALID_BLOCK;
		strcpy(nf->fullpath, name);

		nf->file->f_num = inum;
		nf->file->f_type = type;
		strcpy(nf->filename, filename);

		memset(&nf->file->f_inode, 0, sizeof(struct UFS_dinode));
		nf->file->f_inode.di_mode = mode | UFS_IREAD; // FIXME set permissions
		nf->file->f_inode.di_nlink = 1;
		nf->file->f_inode.di_gen = 0; // FIXME use random number?

		// Write new inode to disk and allocate it
		r = write_inode(object, inum, nf->file->f_inode, head, tail);
		if (r < 0)
			goto ufs_allocate_name_exit2;

		r = write_inode_bitmap(object, inum, UFS_USED, head, &newtail);
		if (r != 0)
			goto ufs_allocate_name_exit2;

		assert(hash_map_insert(info->filemap, (void *) inum, new_file) == 0);
	}
	else {
		new_file = hash_map_find_val(info->filemap, (void *) ln->file->f_num);
		assert(new_file); // FIXME handle better?
		new_file->count++;
		nf->dir_inode = ln->dir_inode;
		nf->file = new_file->file;
		strcpy(nf->fullpath, name);
		strcpy(nf->filename, filename);
	}

	// Create directory entry
	r = insert_dirent(object, fdesc, new_fdesc, &offset, head, &newtail);
	if (r < 0)
		goto ufs_allocate_name_exit3;
	nf->dir_offset = offset;

	// Increase link count
	if (ln) {
		// Need to re-read the inode from disk
		if (!strcmp(filename, ".")) {
			r = read_inode(object, nf->file->f_num, &nf->file->f_inode);
			if (r < 0)
				goto ufs_allocate_name_exit2;
			r = read_inode(object, ln->file->f_num, &ln->file->f_inode);
			if (r < 0)
				goto ufs_allocate_name_exit2;
		}
		nf->file->f_inode.di_nlink++;
		r = write_inode(object, nf->file->f_num, nf->file->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_allocate_name_exit3;
	}

	// Create . and ..
	if (type == TYPE_DIR && notdot) {
		fdesc_t * cfdesc;
		char cname[UFS_MAXPATHLEN];
		int len = strlen(name);

		strcpy(cname, name);
		strcpy(cname + len, "/.");
		cfdesc = ufs_allocate_name(object, cname, TYPE_DIR, new_fdesc, head, &newtail);
		if (!cfdesc)
			goto ufs_allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);
		strcpy(cname + len, "/..");
		cfdesc = ufs_allocate_name(object, cname, TYPE_DIR, fdesc, head, &newtail);
		if (!cfdesc)
			goto ufs_allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);

		r = update_summary(object, inum / info->super->fs_ipg, 1, 0, 0, 0, head, &newtail);
		if (r < 0)
			goto ufs_allocate_name_exit2;
	}

	ufs_free_fdesc(object, fdesc);

	return new_fdesc;

ufs_allocate_name_exit3:
	if (!ln)
		write_inode_bitmap(object, inum, UFS_FREE, head, &newtail);
ufs_allocate_name_exit2:
	ufs_free_fdesc(object, new_fdesc);
ufs_allocate_name_exit:
	ufs_free_fdesc(object, fdesc);
	return NULL;
}

static int ufs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %s %s\n", __FUNCTION__, oldname, newname);
	fdesc_t * old_fdesc;
	fdesc_t * new_fdesc;
	struct ufs_fdesc * oldf;
	struct ufs_fdesc * newf;
	struct ufs_fdesc deadf;
	struct UFS_File deadfile;
	struct ufs_fdesc dirfdesc;
	struct UFS_File dirfile;
	struct UFS_direct entry;
	chdesc_t * newtail;
	uint32_t p;
	int r, existing = 0;

	if (!head || !tail )
		return -E_INVAL;

	if (!strcmp(oldname, newname)) // Umm, ok
		return 0;

	old_fdesc = ufs_lookup_name(object, oldname);
	if (!old_fdesc)
		return -E_NOT_FOUND;

	oldf = (struct ufs_fdesc *) old_fdesc;

	new_fdesc = ufs_lookup_name(object, newname);
	newf = (struct ufs_fdesc *) new_fdesc;
	if (new_fdesc) {
		// Overwriting a directory makes little sense
		if (newf->file->f_type == TYPE_DIR) {
			r = -E_NOT_EMPTY;
			goto ufs_rename_exit2;
		}

		// File already exists
		existing = 1;

		// Save old info
		memcpy(&deadf, newf, sizeof(struct ufs_fdesc));
		memcpy(&deadfile, newf->file, sizeof(struct UFS_File));
		deadf.file = &deadfile;

		// FIXME perhaps we should read in the full fdesc?
		// instead of using this synthetic version?
		dirfdesc.file = &dirfile;
		dirfile.f_type = TYPE_DIR;
		r = read_inode(object, newf->dir_inode, &dirfile.f_inode);
		if (r < 0)
			goto ufs_rename_exit2;

		p = newf->dir_offset;
		r = read_dirent(object, (fdesc_t *) &dirfdesc, &entry, &p);
		if (r < 0)
			goto ufs_rename_exit2;

		entry.d_ino = oldf->file->f_num;
		r = write_dirent(object, (fdesc_t *) &dirfdesc, entry, newf->dir_offset, head, tail);
		if (r < 0)
			goto ufs_rename_exit2;

		oldf->file->f_inode.di_nlink++;
		r = write_inode(object, oldf->file->f_num, oldf->file->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;
	}
	else {
		// Link files together
		new_fdesc = ufs_allocate_name(object, newname, oldf->file->f_type, old_fdesc, head, tail);
		newf = (struct ufs_fdesc *) new_fdesc;
		if (!new_fdesc) {
			r = -E_UNSPECIFIED;
			goto ufs_rename_exit;
		}
	}

	newf->file->f_inode.di_nlink--;
	r = write_inode(object, newf->file->f_num, newf->file->f_inode, head, &newtail);
	if (r < 0)
		goto ufs_rename_exit2;

	r = erase_dirent(object, old_fdesc, head, &newtail);
	if (r < 0)
		goto ufs_rename_exit2;

	if (existing) {
		uint32_t block, i, n = deadf.file->f_numfrags;
		for (i = 0; i < n; i++) {
			block = ufs_truncate_file_block(object, (fdesc_t *) &deadf, head, &newtail);
			if (block == INVALID_BLOCK) {
				r = -E_UNSPECIFIED;
				goto ufs_rename_exit2;
			}
			r = ufs_free_block(object, (fdesc_t *) &deadf, block, head, &newtail);
			if (r < 0)
				goto ufs_rename_exit2;
		}

		memset(&deadf.file->f_inode, 0, sizeof(struct UFS_dinode));
		r = write_inode(object, deadf.file->f_num, deadf.file->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;

		r = write_inode_bitmap(object, deadf.file->f_num, UFS_FREE, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;
	}

	r = 0;

ufs_rename_exit2:
	ufs_free_fdesc(object, new_fdesc);
ufs_rename_exit:
	ufs_free_fdesc(object, old_fdesc);
	return r;
}

static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t offset, blockno, truncated;
	int r;
	chdesc_t * newtail;

	if (!head || !tail || !f || f->file->f_numfrags == 0)
		return INVALID_BLOCK;

	truncated = f->file->f_lastfrag;

	if ((f->file->f_numfrags - 1) % info->super->fs_frag) {

		// not truncating the entire block
		// the fragment has been attached implicitly
		f->file->f_numfrags--;
		f->file->f_lastfrag--;

		return truncated;
	}

	offset = (f->file->f_numfrags - 1) * info->super->fs_fsize;
	r = erase_block_ptr(object, file, offset, head, &newtail);
	if (r < 0)
		return INVALID_BLOCK;

	if (offset != 0) {
		offset -= info->super->fs_bsize;
		blockno = ufs_get_file_block(object, file, offset);
		assert(blockno != INVALID_BLOCK); // FIXME handle better
		f->file->f_lastfrag = blockno + info->super->fs_frag - 1;
	}
	else
		f->file->f_lastfrag = 0;

	f->file->f_numfrags--;

	return truncated;
}

static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	chdesc_t * newtail;
	int r;

	if (!head || !tail)
		return -E_INVAL;

	if (file) {
		// Whole block time
		if (f->file->f_numfrags >= UFS_NDADDR * info->super->fs_frag) {
			if (f->file->f_numfrags % info->super->fs_frag == 0) {
				assert(block % info->super->fs_frag == 0);

				// free the entire block
				assert(block % info->super->fs_frag == 0);
				return erase_wholeblock(object, block / info->super->fs_frag, file, head, tail);
			}
			else {
				// Do nothing
				*tail = NULL;
				return 0;
			}
		}
		else {
			f->file->f_inode.di_blocks -= 4;
			r = write_inode(object, f->file->f_num, f->file->f_inode, head, tail);
			if (r < 0)
				return r;
			return write_fragment_bitmap(object, block, UFS_FREE, head, &newtail);
		}
	}

	// Free the fragment, no questions asked
	return write_fragment_bitmap(object, block, UFS_FREE, head, tail);
}

static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f;
	int r, minlinks = 1;
	chdesc_t * newtail;

	if (!head || !tail || !name || !strcmp(name, "") || !strcmp(name, "/"))
		return -E_INVAL;

	if (strlen(name) > 1)
		if (!strcmp(name + strlen(name) - 2, "/."))
			return -E_INVAL;

	f = (struct ufs_fdesc *) ufs_lookup_name(object, name);
	if (!f)
		return -E_NOT_FOUND;

	if (f->file->f_type == TYPE_DIR) {
		if (f->file->f_inode.di_nlink > 2) {
			r = -E_NOT_EMPTY;
			goto ufs_remove_name_error;
		}
		else if (f->file->f_inode.di_nlink < 2) {
			printf("%s warning, directory with %d links\n", __FUNCTION__, f->file->f_inode.di_nlink);
			minlinks = f->file->f_inode.di_nlink;
		}
		else
			minlinks = 2;
	}

	// Remove directory entry
	r = erase_dirent(object, (fdesc_t *) f, head, tail);
	if (r < 0)
		goto ufs_remove_name_error;

	// Update / free inode
	assert (f->file->f_inode.di_nlink >= minlinks);
	if (f->file->f_inode.di_nlink == minlinks) {
		// Truncate the directory
		if (f->file->f_type == TYPE_DIR) {
			uint32_t number, nblocks, j;
			nblocks = ufs_get_file_numblocks(object, (fdesc_t *) f);

			for (j = 0; j < nblocks; j++) {
				number = ufs_truncate_file_block(object, (fdesc_t *) f, head, &newtail);
				if (number == INVALID_BLOCK) {
					r = -E_INVAL;
					goto ufs_remove_name_error;
				}

				r = ufs_free_block(object, (fdesc_t *) f, number, head, &newtail);
				if (r < 0)
					goto ufs_remove_name_error;
			}
		}

		// Clear inode
		memset(&f->file->f_inode, 0, sizeof(struct UFS_dinode));
		r = write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;

		r = write_inode_bitmap(object, f->file->f_num, UFS_FREE, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}
	else {
		f->file->f_inode.di_nlink--;
		r = write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}

	if (f->file->f_type == TYPE_DIR) {
		// Decrement parent directory's link count
		struct UFS_dinode dir_inode;
		struct UFS_cg cg;
		int cyl = f->file->f_num / info->super->fs_ipg;

		r = read_inode(object, f->dir_inode, &dir_inode);
		if (r < 0)
			goto ufs_remove_name_error;
		dir_inode.di_nlink--;
		r = write_inode(object, f->dir_inode, dir_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;

		// Update group summary
		r = read_cg(object, cyl, &cg);
		if (r < 0)
			goto ufs_remove_name_error;

		r = update_summary(object, cyl, -1, 0, 0, 0, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}

	ufs_free_fdesc(object, (fdesc_t *) f);
	return 0;

ufs_remove_name_error:
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (!head || !tail)
		return -E_INVAL;

	*tail = NULL;

	return CALL(info->ubd, write_block, block);
}

static const feature_t * ufs_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_nlinks, &KFS_feature_file_lfs, &KFS_feature_file_lfs_name, &KFS_feature_unixdir};

static size_t ufs_get_num_features(LFS_t * object, const char * name)
{
	return sizeof(ufs_features) / sizeof(ufs_features[0]);
}

static const feature_t * ufs_get_feature(LFS_t * object, const char * name, size_t num)
{
	if(num < 0 || num >= sizeof(ufs_features) / sizeof(ufs_features[0]))
		return NULL;
	return ufs_features[num];
}

// TODO (permission feature, etc)
static int ufs_get_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t * size, void ** data)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	if (id == KFS_feature_size.id) {
		*data = malloc(sizeof(off_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(off_t);
		memcpy(*data, &(f->file->f_inode.di_size), sizeof(off_t));
	}
	else if (id == KFS_feature_filetype.id) {
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		*((uint32_t *) *data) = f->file->f_type;
	}
	else if (id == KFS_feature_nlinks.id) {
		*data = malloc(sizeof(int16_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(int16_t);
		memcpy(*data, &(f->file->f_inode.di_nlink), sizeof(int16_t));
	}
	else if (id == KFS_feature_freespace.id) {
		int free_space;
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		free_space = count_free_space(object) * UFS_FRAGSIZE / 1024;
		memcpy(*data, &free_space, sizeof(uint32_t));
	}
	else if (id == KFS_feature_file_lfs.id) {
		*data = malloc(sizeof(object));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(object);
		memcpy(*data, &object, sizeof(object));
	}
	else
		return -E_INVAL;

	return 0;
}

static int ufs_get_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	int r;
	const struct ufs_fdesc * f = (struct ufs_fdesc *) ufs_lookup_name(object, name);
	if (!f)
		return -E_NOT_FOUND;

	if (id == KFS_feature_file_lfs_name.id) {
		// Implement KFS_feature_file_lfs_name here because we need name
		*data = strdup(name);
		if (!*data) {
			r = -E_NO_MEM;
			goto ufs_get_metadata_name_exit;
		}

		*size = strlen(*data);
		r = 0;
	}
	else
		r = ufs_get_metadata(object, f, id, size, data);

ufs_get_metadata_name_exit:
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	const struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	return ufs_get_metadata(object, f, id, size, data);
}

// TODO (permission feature, etc)
static int ufs_set_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	int r;
	
	if (!head || !tail || !f || !data)
		return -E_INVAL;

	if (id == KFS_feature_size.id) {
		if (sizeof(uint32_t) != size || *((uint32_t *) data) >= UFS_MAXFILESIZE)
			return -E_INVAL;

		f->file->f_inode.di_size = *((uint32_t *) data);
		return write_inode(object, f->file->f_num, f->file->f_inode, head, tail);
	}
	else if (id == KFS_feature_filetype.id) {
		uint8_t fs_type;
		uint32_t p;
		struct UFS_direct entry;
		struct ufs_fdesc dirfdesc; 
		struct UFS_File dirfile;

		if (sizeof(uint32_t) != size)
			return -E_INVAL;
		switch(*((uint32_t *) data))
		{
			case TYPE_FILE:
				fs_type = UFS_DT_REG;
				break;
			case TYPE_DIR:
				fs_type = UFS_DT_DIR;
				break;
			case TYPE_SYMLINK:
				fs_type = UFS_DT_LNK;
				break;
			// case TYPE_DEVICE: ambiguous
			default:
				return -E_INVAL;
		}

		// FIXME perhaps we should read in the full fdesc?
		// instead of using this synthetic version?
		dirfdesc.file = &dirfile;
		dirfile.f_type = TYPE_DIR;
		r = read_inode(object, f->dir_inode, &dirfile.f_inode);
		if (r < 0)
			return r;

		p = f->dir_offset;
		r = read_dirent(object, (fdesc_t *) &dirfdesc, &entry, &p);
		if (r < 0)
			return r;

		entry.d_type = fs_type;
		return write_dirent(object, (fdesc_t *) &dirfdesc, entry, f->dir_offset, head, tail);
	}

	return -E_INVAL;
}

static int ufs_set_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	int r;
	const struct ufs_fdesc * f = (struct ufs_fdesc *) ufs_lookup_name(object, name);
	if (!f)
		return -E_INVAL;
	r = ufs_set_metadata(object, f, id, size, data, head, tail);
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_set_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	const struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	return ufs_set_metadata(object, f, id, size, data, head, tail);
}

// TODO sync metadata
static int ufs_sync(LFS_t * object, const char * name)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * f;
	uint32_t i, nblocks;
	int r;
	char * parent;

	if (!name || !name[0])
		return CALL(info->ubd, sync, SYNC_FULL_DEVICE, NULL);

	f = ufs_lookup_name(object, name);
	if (!f)
		return -E_NOT_FOUND;

	nblocks = ufs_get_file_numblocks(object, f);
	for (i = 0 ; i < nblocks; i++)
		if ((r = CALL(info->ubd, sync, ufs_get_file_block(object, f, i * info->super->fs_fsize), NULL)) < 0)
			goto ufs_sync_error;

	if (strcmp(name, "/") == 0) {
		ufs_free_fdesc(object, (fdesc_t *) f);
		return 0;
	}

	parent = malloc(UFS_MAXPATHLEN);
	get_parent_path(name, parent);
	if (strlen(parent) == 0)
		strcpy(parent, "/");
	r = ufs_sync(object, parent);
	ufs_free_fdesc(object, (fdesc_t *) f);
	free(parent);
	return r;

ufs_sync_error:
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_destroy(LFS_t * lfs)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(lfs);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);

	bdesc_release(&info->super_block);
	bdesc_release(&info->csum_block);
	free(info->cylstart);
	hash_map_destroy(info->filemap);

	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);

	return 0;
}

LFS_t * ufs(BD_t * block_device)
{
	struct lfs_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));

	if (!lfs)
		return NULL;

	info = malloc(sizeof(*info));
	if (!info) {
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, ufs, info);
	OBJMAGIC(lfs) = UFS_MAGIC;

	info->ubd = block_device;
	info->filemap = hash_map_create();
	if (!info->filemap) {
		free(info);
		free(lfs);
		return NULL;
	}

	if (check_super(lfs)) {
		free(info);
		free(lfs);
		return NULL;
	}

	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_bd(block_device, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}

	return lfs;
}
