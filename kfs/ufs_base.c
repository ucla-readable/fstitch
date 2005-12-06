/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <lib/types.h>
#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/hash_set.h>
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
	struct UFS_Super * super;
	// commonly used values
	uint16_t ipf; // inodes per fragment
	uint32_t * cylstart; // array of cylinder starting block numbers
};

struct ufs_fdesc {
	uint32_t dir_inode; // Parent directory's inode number
	uint32_t dir_offset; // Byte offset of entry in parent directory
	uint32_t numfrags; // Number of fragments
	uint32_t lastfrag; // Last fragment in the file
	uint32_t lastalloc; // Last fragment we allocated
	char fullpath[UFS_MAXPATHLEN];
	UFS_File_t * file;
};

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number);
static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file);
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int ufs_set_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);

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
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, * ptr;
	int r;
	bdesc_t * block;

	// FIXME I think value <= 128
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

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	r = chdesc_create_byte(block, info->ubd, ROUNDDOWN32(offset, 4), 4, &value, head, tail);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	return 0;
}

static int write_fbp(LFS_t * object, uint32_t num, uint16_t value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, * ptr;
	int r;
	bdesc_t * block;

	// FIXME I think value <= 128
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

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % info->super->fs_fsize) / 4;

	if ((num / 1024) % 2)
		offset += 2;

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
	int r;
	bdesc_t * block, * cgblock;
	chdesc_t * newtail, * ch;

	if (!head || !tail)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 1;
	else
		value = 0;

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

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	blockno = info->cylstart[num / info->super->fs_ipg]
		+ info->super->fs_cblkno;
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

	if (value)
		cg.cg_cs.cs_nifree--;
	else
		cg.cg_cs.cs_nifree++;
	r = chdesc_create_byte(cgblock, info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_cs.cs_nifree,
			sizeof(cg.cg_cs.cs_nifree), &cg.cg_cs.cs_nifree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;

	if (value)
		info->super->fs_cstotal.cs_nifree--;
	else
		info->super->fs_cstotal.cs_nifree++;
	r = chdesc_create_byte(info->super_block, info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal.cs_nifree,
			sizeof(info->super->fs_cstotal.cs_nifree),
			&info->super->fs_cstotal.cs_nifree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->super_block);
	if (r < 0)
		return r;

	return 0;
}

static int write_fragment_bitmap(LFS_t * object, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blockno, offset, * ptr;
	int r, nfrags_before, nfrags_after, unused_before = 0, unused_after = 0;
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

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	nfrags_before = BitsSetTable256[(*ptr >> ROUNDDOWN32(num % 32, 8)) & 0xFF];
	if (nfrags_before == 8)
		unused_before = 1;

	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno;
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
			cg.cg_cs.cs_nffree -= (info->super->fs_frag - 1);
			info->super->fs_cstotal.cs_nffree -= (info->super->fs_frag - 1);
		}
		else
		{
			cg.cg_cs.cs_nffree++;
			info->super->fs_cstotal.cs_nffree++;
		}
	}
	else { // Marked fragment as used
		if (unused_before) { // Mark the whole block as used
			r = write_block_bitmap(object, num / info->super->fs_frag,
					UFS_USED, head, &newtail);
			if (r < 0)
				return r;
			cg.cg_cs.cs_nffree += (info->super->fs_frag - 1);
			info->super->fs_cstotal.cs_nffree += (info->super->fs_frag - 1);
		}
		else
		{
			cg.cg_cs.cs_nffree--;
			info->super->fs_cstotal.cs_nffree--;
		}
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

	r = chdesc_create_byte(cgblock, info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_cs.cs_nffree,
			sizeof(cg.cg_cs.cs_nffree), &cg.cg_cs.cs_nffree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;

	r = chdesc_create_byte(info->super_block, info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal.cs_nffree,
			sizeof(info->super->fs_cstotal.cs_nffree),
			&info->super->fs_cstotal.cs_nffree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->super_block);
	if (r < 0)
		return r;

	return 0;
}

// This is the 'raw' function to write the block bitmap
// You probably want allocate_wholeblock()
static int write_block_bitmap(LFS_t * object, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_cg cg;
	uint32_t blocknum, blockno, offset, * ptr, btot;
	uint16_t fbp;
	int r;
	bdesc_t * block, * cgblock;
	chdesc_t * newtail, * ch;

	if (!head || !tail)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 0;
	else
		value = 1;

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

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	blockno = info->cylstart[num / info->super->fs_fpg]
		+ info->super->fs_cblkno;
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

	if (value)
		cg.cg_cs.cs_nbfree++;
	else
		cg.cg_cs.cs_nbfree--;
	r = chdesc_create_byte(cgblock, info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_cs.cs_nbfree,
			sizeof(cg.cg_cs.cs_nbfree), &cg.cg_cs.cs_nbfree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;

	if (value) {
		btot = read_btot(object, blocknum) + 1;
		r = write_btot(object, blocknum, btot, head, &newtail);
		if (r < 0)
			return r;
		fbp = read_fbp(object, blocknum) + 1;
		r = write_fbp(object, blocknum, fbp, head, &newtail);
		if (r < 0)
			return r;
	}
	else {
		btot = read_btot(object, blocknum) - 1;
		r = write_btot(object, blocknum, btot, head, &newtail);
		if (r < 0)
			return r;
		fbp = read_fbp(object, blocknum) - 1;
		r = write_fbp(object, blocknum, fbp, head, &newtail);
		if (r < 0)
			return r;
	}

	if (value)
		info->super->fs_cstotal.cs_nbfree++;
	else
		info->super->fs_cstotal.cs_nbfree--;
	r = chdesc_create_byte(info->super_block, info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal.cs_nbfree,
			sizeof(info->super->fs_cstotal.cs_nbfree),
			&info->super->fs_cstotal.cs_nbfree, head, &newtail);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->super_block);
	if (r < 0)
		return r;

	return 0;
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

	// Find free block
	for (num = 0; num < info->super->fs_size; num++) {
		r = read_fragment_bitmap(object, num);
		if (r < 0)
			return INVALID_BLOCK;
		if (r == UFS_FREE)
	return num; // returns a fragment number
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
		f->file->f_inode.di_ib[n] = 0;
		return write_inode(object, f->file->f_num, f->file->f_inode, head, &newtail);
	}
	else {
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
			r = modify_indirect_ptr(object, file, 0, 1, head, newtail);
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

static int read_dirent(LFS_t * object, fdesc_t * file, struct UFS_direct * entry, uint32_t * basep)
{
	Dprintf("UFSDEBUG: %s %x, %d\n", __FUNCTION__, basep, *basep);
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
		return -E_INVAL;

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

// TODO handle crossing fragment boundaries
static int erase_dirent(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_direct last_entry, entry;
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t basep = 0, last_basep;
	int r;

	if (!head || !tail || !file)
		return -E_INVAL;

	if (f->dir_offset < 12) {
		printf("%s: trying to remove . or ..\n", __FUNCTION__);
		return -E_NOT_EMPTY;
	}

	// Find the entry in front of us
	do {
		last_basep = basep;
		r = read_dirent(object, file, &last_entry, &basep);
		if (r < 0)
			return r;
	} while (basep < f->dir_offset);

	// we went past the entry somehow?
	if (basep != f->dir_offset) {
		printf("%s: went past the directory entry\n", __FUNCTION__);
		return -E_UNSPECIFIED;
	}

	// Get our entry
	r = read_dirent(object, file, &entry, &basep);
	if (r < 0)
		return r;

	last_entry.d_reclen += entry.d_reclen;

	return write_dirent(object, file, last_entry, last_basep, head, tail);
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
	uint32_t basep = 0;
	struct dirent entry;
	struct ufs_fdesc temp_fdesc;
	int r = 0;

	if (!new_fdesc || !name)
		return -E_INVAL;

	temp_fdesc.file = &dir;
	while (r >= 0) {
		r = ufs_get_dirent(object, (fdesc_t *) &temp_fdesc, &entry, sizeof(struct dirent), &basep);
		if (r < 0)
			return r;
		if (!strcmp(entry.d_name, name)) {
			new_fdesc->file = malloc(sizeof(UFS_File_t));
			if (!new_fdesc->file)
				return -E_NO_MEM;

			new_fdesc->dir_inode = dir.f_num;
			new_fdesc->dir_offset = basep;
			strcpy(new_fdesc->file->f_name, name);
			new_fdesc->file->f_type = entry.d_type;
			new_fdesc->file->f_num = entry.d_fileno;
			r = read_inode(object, entry.d_fileno, &new_fdesc->file->f_inode);
			if (r < 0)
				return r;
			new_fdesc->lastalloc = INVALID_BLOCK;
			new_fdesc->numfrags = ufs_get_file_numblocks(object, (fdesc_t *) new_fdesc);
			if (new_fdesc->numfrags)
				new_fdesc->lastfrag = ufs_get_file_block(object, (fdesc_t *) new_fdesc, (new_fdesc->numfrags - 1) * info->super->fs_fsize);
			else
				new_fdesc->lastfrag = 0;
			if (new_fdesc->lastfrag == INVALID_BLOCK)
				return -E_UNSPECIFIED;
			return 0;
		}
	}

	return 0;
}

static int walk_path(LFS_t * object, const char * path, struct ufs_fdesc * new_fdesc)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
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
		new_fdesc->file = malloc(sizeof(UFS_File_t));
		if (!new_fdesc->file)
			return -E_NO_MEM;

		new_fdesc->file->f_name[0] = 0;
		new_fdesc->file->f_type = TYPE_DIR;
		memcpy(&new_fdesc->file->f_inode, &dir.f_inode, sizeof(struct UFS_dinode));
		new_fdesc->lastalloc = INVALID_BLOCK;
		new_fdesc->numfrags = ufs_get_file_numblocks(object, (fdesc_t *) new_fdesc);
		if (new_fdesc->numfrags)
			new_fdesc->lastfrag = ufs_get_file_block(object, (fdesc_t *) new_fdesc, (new_fdesc->numfrags - 1) * info->super->fs_fsize);
		else
			new_fdesc->lastfrag = 0;
		if (new_fdesc->lastfrag == INVALID_BLOCK)
			return -E_UNSPECIFIED;
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

	frags = f->numfrags % info->super->fs_frag;
	offset = (f->numfrags - frags) * info->super->fs_size;

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
		block = CALL(info->ubd, read_block, f->lastfrag - frags + i + 1);
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
		r = write_fragment_bitmap(object, f->lastfrag - frags + i + 1, UFS_FREE, head, &newtail);
		if (r != 0)
			return INVALID_BLOCK;
	}

	blockno = blockno + frags;
	f->lastfrag = blockno - 1;

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

	if (f->lastalloc != INVALID_BLOCK)
		// We already allocated a fragment, go use that first
		return INVALID_BLOCK;

	// File has no fragments
	if (f->numfrags == 0) {
		blockno = find_free_block_linear(object, file, purpose);
		if (blockno == INVALID_BLOCK)
			return INVALID_BLOCK;
		blockno *= info->super->fs_frag;
	}
	// We're using indirect pointers, time to allocate whole blocks
	else if (f->numfrags >= UFS_NDADDR * info->super->fs_frag) {
		// Well, except we're still working with fragments here

		// Time to allocate a find a new block
		if (((f->lastfrag + 1) % info->super->fs_frag) == 0) {
			blockno = allocate_wholeblock(object, 1, file, head, tail);
			f->lastalloc = blockno;
			return blockno;
		}
		// Use the next fragment (everything was zeroed out already)
		else {
			blockno = f->lastfrag + 1;
			f->lastalloc = blockno;
			return blockno;
		}
	}
	// Time to allocate a find a new block
	else if (((f->lastfrag + 1) % info->super->fs_frag) == 0) {
		if (f->numfrags % info->super->fs_frag) {
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
		r = read_fragment_bitmap(object, f->lastfrag + 1);
		if (r < 0)
			return r;
		else if (r == UFS_FREE)
			blockno = f->lastfrag + 1; // UFS says we must use it
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

	f->lastalloc = blockno;
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
	struct ufs_fdesc * f = (struct ufs_fdesc *) fdesc;

	if (f) {
		if (f->file)
			free(f->file);
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

static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("UFSDEBUG: %s %x, %d\n", __FUNCTION__, basep, *basep);
	struct UFS_direct dirfile;
	struct UFS_dinode inode;
	uint32_t actual_len, p;
	int r;

	if (!entry)
		return -E_INVAL;

	p = *basep;
	r = read_dirent(object, file, &dirfile, &p);
	if (r < 0)
		return r;

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

static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t offset;
	int r;

	if (!head || !tail || !f || block == INVALID_BLOCK)
		return -E_INVAL;

	if (block != f->lastalloc)
		// hmm, that's not the right block
		return -E_UNSPECIFIED;

	if (f->numfrags % info->super->fs_frag) {
		// not appending to a new block,
		// the fragment has been attached implicitly
		f->numfrags++;
		f->lastfrag = block;
		f->lastalloc = INVALID_BLOCK;

		return 0;
	}

	offset = f->numfrags * info->super->fs_fsize;
	r = write_block_ptr(object, file, offset, block, head, tail);
	if (r < 0)
		return r;

	f->numfrags++;
	f->lastfrag = block;
	f->lastalloc = INVALID_BLOCK;

	return 0;
}

// TODO
static fdesc_t * ufs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	return NULL;
}

// TODO
static int ufs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
}

static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	uint32_t offset, blockno, truncated;
	int r;
	chdesc_t * newtail;

	if (!head || !tail || !f || f->numfrags == 0)
		return INVALID_BLOCK;

	truncated = f->lastfrag;

	if ((f->numfrags - 1) % info->super->fs_frag) {

		// not truncating the entire block
		// the fragment has been attached implicitly
		f->numfrags--;
		f->lastfrag--;

		return truncated;
	}

	offset = (f->numfrags - 1) * info->super->fs_fsize;
	r = erase_block_ptr(object, file, offset, head, &newtail);
	if (r < 0)
		return INVALID_BLOCK;

	if (offset != 0) {
		offset -= info->super->fs_bsize;
		blockno = ufs_get_file_block(object, file, offset);
		assert(blockno != INVALID_BLOCK); // FIXME handle better
		f->lastfrag = blockno + info->super->fs_frag - 1;
	}
	else
		f->lastfrag = 0;

	f->numfrags--;

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
		if (f->numfrags >= UFS_NDADDR * info->super->fs_frag) {
			if (f->numfrags % info->super->fs_frag == 0) {
				assert(block % info->super->fs_frag == 0);

				f->file->f_inode.di_blocks -= 32;
				r = write_inode(object, f->file->f_num, f->file->f_inode, head, tail);
				if (r < 0)
					return r;
				// free the entire block
				assert(block % info->super->fs_frag == 0);
				return erase_wholeblock(object, block / info->super->fs_frag, file, head, &newtail);
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

// TODO
static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
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

static const feature_t * ufs_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_nlinks, &KFS_feature_file_lfs, &KFS_feature_file_lfs_name};

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
			return r;

	if (strcmp(name, "/") == 0)
		return 0;

	parent = malloc(UFS_MAXPATHLEN);
	get_parent_path(name, parent);
	if (strlen(parent) == 0)
		strcpy(parent, "/");
	r = ufs_sync(object, parent);
	free(parent);
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
