/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <lib/types.h>
#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <assert.h>

#include <kfs/modman.h>
#include <kfs/ufs_base.h>
#include <kfs/ufs_common.h>
#include <kfs/ufs_alloc_linear.h>

#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in __FILE__
#endif

#define UFS_BASE_DEBUG 0

#if UFS_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct open_ufsfile {
	ufs_fdesc_t * file;
	int count;
};
typedef struct open_ufsfile open_ufsfile_t;

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number);
static int get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file);
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ufs_set_metadata(LFS_t * object, ufs_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail);
static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail);
static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
//
int search_dirent(LFS_t * object, ufs_fdesc_t * parent, const char * name, inode_t * ino, int * offset);

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

// TODO do more checks, move printf statements elsewhere, mark fs as unclean
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t numblocks;
	int i, bs;

	// TODO better way of detecting fs block size
	/* make sure we have the block size we expect */
	bs = CALL(info->ubd, get_blocksize);
	if (bs != 2048) {
		printf("Block device size is not 2048! (%d)\n", bs);
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

// Find a free block and allocate all fragments in the block
static uint32_t allocate_wholeblock(LFS_t * object, int wipe, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	bool synthetic;
	uint32_t i, num;
	bdesc_t * block;
	chdesc_t * newtail;

	if (!head || !tail)
		return INVALID_BLOCK;

	num = CALL(info->allocator, find_free_block, file, 0);
	if (num == INVALID_BLOCK)
		return INVALID_BLOCK;

	// Mark the fragments as used
	for (i = num * info->super->fs_frag; i < (num + 1) * info->super->fs_frag; i++) {
		if (i == num * info->super->fs_frag)
			r = write_fragment_bitmap(info, i, UFS_USED, head, tail);
		else
			r = write_fragment_bitmap(info, i, UFS_USED, head, &newtail);
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
		f->f_inode.di_blocks += 32; // charge the fragments to the file
		r = write_inode(info, f->f_num, f->f_inode, head, &newtail);
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
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	uint32_t i;
	chdesc_t * newtail;

	if (!head || !tail || num == INVALID_BLOCK)
		return -E_INVAL;

	// Mark the fragments as used
	for (i = num * info->super->fs_frag; i < (num + 1) * info->super->fs_frag; i++) {
		if (i == num * info->super->fs_frag)
			r = write_fragment_bitmap(info, i, UFS_FREE, head, tail);
		else
			r = write_fragment_bitmap(info, i, UFS_FREE, head, &newtail);
		if (r < 0)
			return r;
		assert(r != 1); // This should not happen
	}

	if (file) {
		f->f_inode.di_blocks -= 32; // charge the fragments to the file
		r = write_inode(info, f->f_num, f->f_inode, head, &newtail);
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
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t newblock;
	chdesc_t * newtail;

	if (!file || !head || !tail || n < 0 || n >= UFS_NIADDR)
		return -E_INVAL;

	// Beware of the evil bit? ;)
	if (evil) {
		// Clears the indirect pointer...
		f->f_inode.di_ib[n] = 0;
		return write_inode(info, f->f_num, f->f_inode, head, &newtail);
	}
	else {
		// Allocates an indirect pointer block
		if (f->f_inode.di_ib[n])
			return -E_UNSPECIFIED;

		newblock = allocate_wholeblock(object, 1, file, head, tail);
		if (newblock == INVALID_BLOCK)
			return -E_NOT_FOUND;
		f->f_inode.di_ib[n] = newblock;
		return write_inode(info, f->f_num, f->f_inode, head, &newtail);
	}
}

// Write the block ptrs for a file, allocate indirect blocks as needed
// Offset is a byte offset
static int write_block_ptr(LFS_t * object, fdesc_t * file, uint32_t offset, uint32_t value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %x %d %d\n", __FUNCTION__, file, offset, value);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
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
		f->f_inode.di_db[blockno] = value;
		return write_inode(info, f->f_num, f->f_inode, head, tail);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		// Allocate single indirect block if needed
		if (!f->f_inode.di_ib[0]) {
			r = modify_indirect_ptr(object, file, 0, 0, head, newtail);
			if (r < 0)
				return r;
			newtail = &tmptail;
		}

		indirect[0] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[0] + frag_off[0]);
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
		if (!f->f_inode.di_ib[1]) {
			r = modify_indirect_ptr(object, file, 1, 0, head, newtail);
			if (r < 0)
				return r;
			newtail = &tmptail;
		}

		indirect[1] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[1] + frag_off[1]);
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
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
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
		f->f_inode.di_db[blockno] = 0;
		return write_inode(info, f->f_num, f->f_inode, head, tail);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;
		num[0] = f->f_inode.di_ib[0] / info->super->fs_frag;

		indirect[0] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[0] + frag_off[0]);
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
		num[1] = f->f_inode.di_ib[1] / info->super->fs_frag;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		indirect[1] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[1] + frag_off[1]);
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

static int check_name(const char * p)
{
	int i;

	if (!p)
		return -1;

	if (strlen(p) < 1 || strlen(p) > UFS_MAXNAMELEN)
		return -2;

	for (i = 0 ; i < strlen(p); i++) {
		if (p[i] == '/')
			return 1;
	}

	return 0;
}

// file is the directory
static int read_dirent(LFS_t * object, fdesc_t * file, struct UFS_direct * entry, uint32_t * basep)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, *basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	struct UFS_direct * dirfile;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, offset;

	if (!entry)
		return -E_INVAL;

	// Make sure it's a directory and we can read from it
	if (f->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	if (*basep >= f->f_inode.di_size)
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

static int erase_dirent(LFS_t * object, fdesc_t * dirf, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct UFS_direct last_entry, entry;
	ufs_fdesc_t * f = (ufs_fdesc_t *) dirf;
	uint32_t basep, last_basep, p;
	int r, offset;

	if (!head || !tail || !f || check_name(name))
		return -E_INVAL;

	r = search_dirent(object, f, name, NULL, &offset);
	if (r < 0)
		return r;

	if (offset % 512 == 0) {
		// We are the first entry in the fragment
		p = offset;
		r = read_dirent(object, dirf, &entry, &p);
		if (r < 0)
			return r;

		entry.d_ino = 0;
		return write_dirent(object, dirf, entry, offset, head, tail);
	}

	// Find the entry in front of us
	basep = 0;
	do {
		last_basep = basep;
		r = read_dirent(object, dirf, &last_entry, &basep);
		if (r < 0)
			return r;
	} while (basep < offset);

	// we went past the entry somehow?
	if (basep != offset) {
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

// tries to find an empty entry for a filename of len, in directory dirf
static int find_free_dirent(LFS_t * object, fdesc_t * dirf, uint32_t len)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, len);
	struct UFS_direct entry;
	uint32_t basep = 0, last_basep, actual_len;
	int r;

	if (!dirf)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + len - UFS_MAXNAMELEN, 4);

	while (1) {
		last_basep = basep;
		r = read_dirent(object, dirf, &entry, &basep);
		if (r < 0 && r != -E_EOF)
			return r;
		if (r == -E_EOF) // EOF, return where next entry starts
			return basep;

		if (entry.d_ino) {
			// Check to see if entry has room leftover for our entry
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

static int insert_dirent(LFS_t * object, fdesc_t * dir_file, inode_t ino, uint8_t type, const char * name, int offset, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * df = (ufs_fdesc_t *) dir_file;
	struct UFS_direct entry, last_entry;
	chdesc_t * tmptail;
	chdesc_t ** newtail = tail;
	uint32_t len, last_len, blockno, newsize = offset + 512;
	int r, p = offset, alloc = 0;
	uint8_t fs_type;

	if (!head || !tail || !df || check_name(name) || offset < 0)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);

	switch(type)
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
	entry.d_ino = ino;
	entry.d_namlen = strlen(name);
	strcpy(entry.d_name, name);
	entry.d_name[entry.d_namlen] = 0;

	// Need to extend directory
	if (offset >= df->f_inode.di_size) {
		// Need to allocate/append fragment
		if (offset % info->super->fs_fsize == 0) {
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
		r = write_dirent(object, dir_file, entry, offset + last_len, head, newtail);
		if (r < 0)
			return r;
		newtail = &tmptail;
		last_entry.d_reclen = last_len;
		r = write_dirent(object, dir_file, last_entry, offset, head, newtail);
		return r;
	}
	else {
		if (alloc) // Writing to new fragment
			entry.d_reclen = 512;
		else // Overwriting blank entry
			entry.d_reclen = last_entry.d_reclen;
		return write_dirent(object, dir_file, entry, offset, head, newtail);
	}
}

static open_ufsfile_t * open_ufsfile_create(ufs_fdesc_t * file)
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

static open_ufsfile_t * get_ufsfile(hash_map_t * filemap, inode_t ino, int * exists)
{
	open_ufsfile_t * existing_file;
	ufs_fdesc_t * new_file;

	if (!filemap)
		return NULL;

	existing_file = hash_map_find_val(filemap, (void *) ino);
	if (existing_file) {
		existing_file->count++;
		*exists = 1;
		return existing_file;
	}

	*exists = 0;
	new_file = malloc(sizeof(ufs_fdesc_t));
	if (!new_file)
		return NULL;

	// If file struct is not in memory
	existing_file = open_ufsfile_create(new_file);
	if (!existing_file) {
		free(new_file);
		return NULL;
	}
	assert(hash_map_insert(filemap, (void *) ino, existing_file) == 0);
	return existing_file;
}

int search_dirent(LFS_t * object, ufs_fdesc_t * parent, const char * name, inode_t * ino, int * offset)
{
	uint32_t basep = 0, last_basep;
	struct dirent entry;
	int r = 0;

	if (!parent || check_name(name))
		return -E_INVAL;

	while (r >= 0) {
		last_basep = basep;
		r = get_dirent(object, (fdesc_t *) parent, &entry, sizeof(struct dirent), &basep);
		if (r < 0)
			return r;
		if (entry.d_fileno == 0) // Blank spot
			continue;
		if (!strcmp(entry.d_name, name)) {
			if (ino)
				*ino = entry.d_fileno;
			if (offset)
				*offset = last_basep;
			return 0;
		}
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
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t i, blockno, offset;
	int r, frags;
	bool synthetic = 0;
	chdesc_t * newtail;
	bdesc_t * block, * newblock;

	if (!head || !tail || !file)
		return INVALID_BLOCK;

	frags = f->f_numfrags % info->super->fs_frag;
	offset = (f->f_numfrags - frags) * info->super->fs_size;

	// Time to allocate a new block and copy the data there
	// FIXME handle failure case better?

	// find new block
	blockno = CALL(info->allocator, find_free_block, file, purpose);
	if (blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	blockno *= info->super->fs_frag;

	// allocate some fragments
	for (i = 0 ; i < frags; i++) {
		if (i == 0)
			r = write_fragment_bitmap(info, blockno, UFS_USED, head, tail);
		else
			r = write_fragment_bitmap(info, blockno + i, UFS_USED, head, &newtail);
		if (r != 0)
			return INVALID_BLOCK;
	}

	// read in fragments, and write to new location
	for (i = 0 ; i < frags; i++) {
		block = CALL(info->ubd, read_block, f->f_lastfrag - frags + i + 1);
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
		r = write_fragment_bitmap(info, f->f_lastfrag - frags + i + 1, UFS_FREE, head, &newtail);
		if (r != 0)
			return INVALID_BLOCK;
	}

	blockno = blockno + frags;
	f->f_lastfrag = blockno - 1;

	return blockno;
}

// Allocates fragments, really
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	bdesc_t * block;
	uint32_t blockno;
	bool synthetic = 0, use_newtail = 0;
	chdesc_t * newtail;
	int r;

	// FIXME require file to be non-null for now
	if (!head || !tail || !file)
		return INVALID_BLOCK;

	if (f->f_lastalloc != INVALID_BLOCK)
		// We already allocated a fragment, go use that first
		return INVALID_BLOCK;

	// File has no fragments
	if (f->f_numfrags == 0) {
		blockno = CALL(info->allocator, find_free_block, file, purpose);
		if (blockno == INVALID_BLOCK)
			return INVALID_BLOCK;
		blockno *= info->super->fs_frag;
	}
	// We're using indirect pointers, time to allocate whole blocks
	else if (f->f_numfrags >= UFS_NDADDR * info->super->fs_frag) {
		// Well, except we're still working with fragments here

		// Time to allocate a find a new block
		if (((f->f_lastfrag + 1) % info->super->fs_frag) == 0) {
			blockno = allocate_wholeblock(object, 0, file, head, tail);
			f->f_lastalloc = blockno;
			return blockno;
		}
		// Use the next fragment (everything was zeroed out already)
		else {
			blockno = f->f_lastfrag + 1;
			f->f_lastalloc = blockno;
			return blockno;
		}
	}
	// Time to allocate a find a new block
	else if (((f->f_lastfrag + 1) % info->super->fs_frag) == 0) {
		if (f->f_numfrags % info->super->fs_frag) {
			blockno = find_frags_new_home(object, file, purpose, head, tail);
			use_newtail = 1;
		}
		else {
			blockno = CALL(info->allocator, find_free_block, file, purpose);
			if (blockno == INVALID_BLOCK)
				return INVALID_BLOCK;
			blockno *= info->super->fs_frag;
		}
	}
	// Use the next fragment
	else {
		r = read_fragment_bitmap(info, f->f_lastfrag + 1);
		if (r < 0)
			return r;
		else if (r == UFS_FREE)
			blockno = f->f_lastfrag + 1; // UFS says we must use it
		else // Next fragment is taken, move elsewhere
		{
			blockno = find_frags_new_home(object, file, purpose, head, tail);
			use_newtail = 1;
		}
	}

	if (use_newtail)
		r = write_fragment_bitmap(info, blockno, UFS_USED, head, &newtail);
	else
		r = write_fragment_bitmap(info, blockno, UFS_USED, head, tail);
	if (r != 0)
		return INVALID_BLOCK;

	assert(read_fragment_bitmap(info, blockno) == UFS_USED);
	block = CALL(info->ubd, synthetic_read_block, blockno, &synthetic);
	if (!block)
		goto allocate_block_cleanup;

	f->f_inode.di_blocks += 4; // grr, di_blocks counts 512 byte blocks
	r = write_inode(info, f->f_num, f->f_inode, head, &newtail);
	if (r < 0)
		return INVALID_BLOCK;

	f->f_lastalloc = blockno;
	return blockno;

allocate_block_cleanup:
	r = write_fragment_bitmap(info, blockno, UFS_FREE, head, &newtail);
	assert(r == 0);
	return INVALID_BLOCK;
}

static fdesc_t * ufs_lookup_inode(LFS_t * object, inode_t ino)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	open_ufsfile_t * ef;
	int r, exists = -1;

	if (ino == 0)
		return NULL;

	ef = get_ufsfile(info->filemap, ino, &exists);
	if (!ef)
		return NULL;

	if (exists == 1)
		return (fdesc_t *) ef->file;
	else if (exists == 0) {
		r = read_inode(info, ino, &ef->file->f_inode);
		if (r < 0) {
			open_ufsfile_destroy(ef);
			return NULL;
		}
		ef->file->f_lastalloc = INVALID_BLOCK;
		ef->file->f_lastfrag = 0;
		ef->file->f_num = ino;
		ef->file->f_numfrags = ufs_get_file_numblocks(object, (fdesc_t *) ef->file);
		ef->file->f_type = ef->file->f_inode.di_mode >> 12;
		return (fdesc_t *) ef->file;
	}

	return NULL;
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

static int ufs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("UFSDEBUG: %s %d, %s\n", __FUNCTION__, parent, name);
	ufs_fdesc_t * pfile;

	if (!ino || check_name(name))
		return -E_INVAL;

	pfile = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pfile)
		return -E_NOT_FOUND;

	if (pfile->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	return search_dirent(object, pfile, name, ino, NULL);
}

static void ufs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("UFSDEBUG: %s %x\n", __FUNCTION__, fdesc);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) fdesc;
	open_ufsfile_t * uf;

	if (f) {
		uf = hash_map_find_val(info->filemap, (void *) f->f_num);
		if (uf->count < 2)
			hash_map_erase(info->filemap, (void *) f->f_num);
		open_ufsfile_destroy(uf);
	}
}

static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	Dprintf("UFSDEBUG: %s %x\n", __FUNCTION__, file);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t n;

	n = f->f_inode.di_size / info->super->fs_fsize;
	if (f->f_inode.di_size % info->super->fs_fsize)
		n++;

	return n;
}

// Offset is a byte offset
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("UFSDEBUG: %s %x %d\n", __FUNCTION__, file, offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t fragno, blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];

	if (offset % info->super->fs_fsize || offset >= f->f_inode.di_size)
		return INVALID_BLOCK;

	nindirb = info->super->fs_nindir;
	nindirf = nindirb / info->super->fs_frag;
	blockno = offset / info->super->fs_bsize;
	fragno = (offset / info->super->fs_fsize) % info->super->fs_frag;

	if (blockno < UFS_NDADDR) {
		return f->f_inode.di_db[blockno] + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		indirect[0] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[0] + frag_off[0]);
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
				f->f_inode.di_ib[1] + frag_off[1]);
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

		// FIXME write some tedious code
	}

	return -E_UNSPECIFIED;
}

static int get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_direct dirfile;
	struct UFS_dinode inode;
	uint32_t actual_len;
	int r;

	if (!entry)
		return -E_INVAL;

	r = read_dirent(object, file, &dirfile, basep);
	if (r < 0)
		return r;

	actual_len = sizeof(struct dirent) + dirfile.d_namlen - DIRENT_MAXNAMELEN;
	if (size < actual_len)
		return -E_INVAL;

	r = read_inode(info, dirfile.d_ino, &inode); 
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

	return 0;
}

static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	int r;

	do {
		r = get_dirent(object, file, entry, size, basep);
		if (r < 0)
			return r;
	} while (entry->d_fileno == 0);

	return r;
}

static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t offset;
	int r;

	if (!head || !tail || !f || block == INVALID_BLOCK)
		return -E_INVAL;

	if (block != f->f_lastalloc)
		// hmm, that's not the right block
		return -E_UNSPECIFIED;

	if (f->f_numfrags % info->super->fs_frag) {
		// not appending to a new block,
		// the fragment has been attached implicitly
		f->f_numfrags++;
		f->f_lastfrag = block;
		f->f_lastalloc = INVALID_BLOCK;

		return 0;
	}

	offset = f->f_numfrags * info->super->fs_fsize;
	r = write_block_ptr(object, file, offset, block, head, tail);
	if (r < 0)
		return r;

	f->f_numfrags++;
	f->f_lastfrag = block;
	f->f_lastalloc = INVALID_BLOCK;

	return 0;
}

// FIXME free fdescs
static fdesc_t * ufs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, inode_t * newino, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * nf;
	ufs_fdesc_t * pf;
	open_ufsfile_t * open_file;
	ufs_fdesc_t * ln = (ufs_fdesc_t *) link;
	uint32_t inum = 0;
	int r, offset, notdot = 1, ex;
	uint16_t mode;
	chdesc_t * newtail;

	if (!head || !tail || check_name(name))
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
	if (ln && type != ln->f_type)
		return NULL;

#warning need to prevent users from creating . and ..
	// Don't create directory hard links, except for . and ..
	if (!strcmp(name, "."))
		notdot = 0;
	else if (!strcmp(name, ".."))
		notdot = 0;

	if (ln && notdot && type == TYPE_DIR)
		return NULL;

	pf = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pf)
		return NULL;

	r = search_dirent(object, pf, name, NULL, NULL);
	if (r >= 0) { // File exists already
		return NULL;
	}

	// Find an empty slot to write into
	offset = find_free_dirent(object, (fdesc_t *) pf, strlen(name) + 1);
	if (offset < 0)
		goto ufs_allocate_name_exit;

	if (!ln) {
		// Allocate new inode
		inum = CALL(info->allocator, find_free_inode, (fdesc_t *) pf);
		if (inum == INVALID_BLOCK)
			goto ufs_allocate_name_exit;

		open_file = get_ufsfile(info->filemap, inum, &ex);
		if (!open_file)
			return NULL;
		assert(ex == 0);

		nf = open_file->file;

		nf->f_numfrags = 0;
		nf->f_lastfrag = 0;
		nf->f_lastalloc = INVALID_BLOCK;

		nf->f_num = inum;
		nf->f_type = type;

		memset(&nf->f_inode, 0, sizeof(struct UFS_dinode));
		nf->f_inode.di_mode = mode | UFS_IREAD; // FIXME set permissions
		nf->f_inode.di_nlink = 1;
		nf->f_inode.di_gen = 0; // FIXME use random number?

		// Write new inode to disk and allocate it
		r = write_inode(info, inum, nf->f_inode, head, tail);
		if (r < 0)
			goto ufs_allocate_name_exit2;

		r = write_inode_bitmap(info, inum, UFS_USED, head, &newtail);
		if (r != 0)
			goto ufs_allocate_name_exit2;
	}
	else {
		open_file = get_ufsfile(info->filemap, ln->f_num,  &ex);
		if (!open_file)
			return NULL;
		assert(ex == 1);
		nf = open_file->file;
	}

	// Create directory entry
	r = insert_dirent(object, (fdesc_t *) pf, nf->f_num, nf->f_type, name, offset, head, &newtail);
	if (r < 0)
		goto ufs_allocate_name_exit3;

	// Increase link count
	if (ln) {
		nf->f_inode.di_nlink++;
		r = write_inode(info, nf->f_num, nf->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_allocate_name_exit3;
	}

	// Create . and ..
	if (type == TYPE_DIR && notdot) {
		fdesc_t * cfdesc;
		inode_t newino;

		cfdesc = ufs_allocate_name(object, nf->f_num, ".", TYPE_DIR, (fdesc_t *) nf, &newino, head, &newtail);
		if (!cfdesc)
			goto ufs_allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);

		cfdesc = ufs_allocate_name(object, nf->f_num, "..", TYPE_DIR, (fdesc_t *) pf, &newino, head, &newtail);
		if (!cfdesc)
			goto ufs_allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);

		r = update_summary(info, inum / info->super->fs_ipg, 1, 0, 0, 0, head, &newtail);
		if (r < 0)
			goto ufs_allocate_name_exit2;
	}

	return (fdesc_t *) nf;

ufs_allocate_name_exit3:
	if (!ln)
		write_inode_bitmap(info, inum, UFS_FREE, head, &newtail);
ufs_allocate_name_exit2:
	ufs_free_fdesc(object, (fdesc_t *) nf);
ufs_allocate_name_exit:
	ufs_free_fdesc(object, (fdesc_t *) pf);
	return NULL;
}

#warning free fdescs
static int ufs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %s %s\n", __FUNCTION__, oldname, newname);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * old_pfdesc;
	ufs_fdesc_t * new_pfdesc;
	ufs_fdesc_t * oldf;
	ufs_fdesc_t * newf;
	ufs_fdesc_t deadf;
	ufs_fdesc_t dirfdesc;
	struct UFS_direct entry;
	chdesc_t * newtail;
	uint32_t p;
	int r, existing = 0, dir_offset;
	inode_t ino, newino;

	if (!head || !tail || check_name(oldname) || check_name(newname))
		return -E_INVAL;

	if (!strcmp(oldname, newname)) // Umm, ok
		return 0;

	old_pfdesc = (ufs_fdesc_t *) ufs_lookup_inode(object, oldparent);
	if (!old_pfdesc)
		return -E_NOT_FOUND;

	r = search_dirent(object, old_pfdesc, oldname, &ino, NULL);
	if (r < 0)
		return r;

	oldf = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	if (!oldf)
		return -E_NOT_FOUND;

	new_pfdesc = (ufs_fdesc_t *) ufs_lookup_inode(object, newparent);
	if (!new_pfdesc)
		return -E_NOT_FOUND;

	r = search_dirent(object, new_pfdesc, newname, &ino, &dir_offset);
	if (r < 0)
		return r;

	if (dir_offset >= 0)
		newf = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	else
		newf = NULL;

	if (newf) {
		// Overwriting a directory makes little sense
		if (newf->f_type == TYPE_DIR) {
			r = -E_NOT_EMPTY;
			goto ufs_rename_exit2;
		}

		// File already exists
		existing = 1;

		// Save old info
		memcpy(&deadf, newf, sizeof(ufs_fdesc_t));

		p = dir_offset;
		r = read_dirent(object, (fdesc_t *) newparent, &entry, &p);
		if (r < 0)
			goto ufs_rename_exit2;

		entry.d_ino = oldf->f_num;
		r = write_dirent(object, (fdesc_t *) &dirfdesc, entry, dir_offset, head, tail);
		if (r < 0)
			goto ufs_rename_exit2;

		oldf->f_inode.di_nlink++;
		r = write_inode(info, oldf->f_num, oldf->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;
	}
	else {
		// Link files together
		newf = (ufs_fdesc_t *) ufs_allocate_name(object, newparent,  newname, oldf->f_type, (fdesc_t *) oldf, &newino, head, tail);
		if (!newf) {
			r = -E_UNSPECIFIED;
			goto ufs_rename_exit;
		}
		assert(ino == newino);
	}

	newf->f_inode.di_nlink--;
	r = write_inode(info, newf->f_num, newf->f_inode, head, &newtail);
	if (r < 0)
		goto ufs_rename_exit2;

	r = erase_dirent(object, (fdesc_t *) old_pfdesc, oldname, head, &newtail);
	if (r < 0)
		goto ufs_rename_exit2;

	if (existing) {
		uint32_t block, i, n = deadf.f_numfrags;
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

		memset(&deadf.f_inode, 0, sizeof(struct UFS_dinode));
		r = write_inode(info, deadf.f_num, deadf.f_inode, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;

		r = write_inode_bitmap(info, deadf.f_num, UFS_FREE, head, &newtail);
		if (r < 0)
			goto ufs_rename_exit2;
	}

	r = 0;

ufs_rename_exit2:
	ufs_free_fdesc(object, (fdesc_t *) newf);
ufs_rename_exit:
	ufs_free_fdesc(object, (fdesc_t *) oldf);
	return r;
}

static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t offset, blockno, truncated;
	int r;
	chdesc_t * newtail;

	if (!head || !tail || !f || f->f_numfrags == 0)
		return INVALID_BLOCK;

	truncated = f->f_lastfrag;

	if ((f->f_numfrags - 1) % info->super->fs_frag) {

		// not truncating the entire block
		// the fragment has been attached implicitly
		f->f_numfrags--;
		f->f_lastfrag--;

		return truncated;
	}

	offset = (f->f_numfrags - 1) * info->super->fs_fsize;
	r = erase_block_ptr(object, file, offset, head, &newtail);
	if (r < 0)
		return INVALID_BLOCK;

	if (offset != 0) {
		offset -= info->super->fs_bsize;
		blockno = ufs_get_file_block(object, file, offset);
		assert(blockno != INVALID_BLOCK); // FIXME handle better
		f->f_lastfrag = blockno + info->super->fs_frag - 1;
	}
	else
		f->f_lastfrag = 0;

	f->f_numfrags--;

	return truncated;
}

static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	chdesc_t * newtail;
	int r;

	if (!head || !tail)
		return -E_INVAL;

	if (file) {
		// Whole block time
		if (f->f_numfrags >= UFS_NDADDR * info->super->fs_frag) {
			if (f->f_numfrags % info->super->fs_frag == 0) {
				assert(block % info->super->fs_frag == 0);

				// free the entire block
				return erase_wholeblock(object, block / info->super->fs_frag, file, head, tail);
			}
			else {
				// Do nothing
				*tail = NULL;
				return 0;
			}
		}
		else {
			f->f_inode.di_blocks -= 4;
			r = write_inode(info, f->f_num, f->f_inode, head, tail);
			if (r < 0)
				return r;
			return write_fragment_bitmap(info, block, UFS_FREE, head, &newtail);
		}
	}

	// Free the fragment, no questions asked
	return write_fragment_bitmap(info, block, UFS_FREE, head, tail);
}

static int ufs_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d %s\n", __FUNCTION__, parent, name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ufs_fdesc_t * pfile;
	ufs_fdesc_t * f;
	inode_t filenum;
	int r, minlinks = 1;
	chdesc_t * newtail;

	if (!head || !tail || check_name(name))
		return -E_INVAL;

	pfile = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pfile)
		return -E_NOT_FOUND;

	if (pfile->f_type != TYPE_DIR) {
		r = -E_NOT_DIR;
		goto ufs_remove_name_error2;
	}

	r = search_dirent(object, pfile, name, &filenum, NULL);
	if (r < 0)
		goto ufs_remove_name_error2;

	f = (ufs_fdesc_t *) ufs_lookup_inode(object, filenum);
	if (!f) {
		r = -E_NOT_FOUND;
		goto ufs_remove_name_error2;
	}

	if (f->f_type == TYPE_DIR) {
		if (f->f_inode.di_nlink > 2) {
			r = -E_NOT_EMPTY;
			goto ufs_remove_name_error;
		}
		else if (f->f_inode.di_nlink < 2) {
			printf("%s warning, directory with %d links\n", __FUNCTION__, f->f_inode.di_nlink);
			minlinks = f->f_inode.di_nlink;
		}
		else
			minlinks = 2;
	}

	// Remove directory entry
	r = erase_dirent(object, (fdesc_t *) pfile, name, head, tail);
	if (r < 0)
		goto ufs_remove_name_error;

	// Update / free inode
	assert (f->f_inode.di_nlink >= minlinks);
	if (f->f_inode.di_nlink == minlinks) {
		// Truncate the directory
		if (f->f_type == TYPE_DIR) {
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
		memset(&f->f_inode, 0, sizeof(struct UFS_dinode));
		r = write_inode(info, f->f_num, f->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;

		r = write_inode_bitmap(info, f->f_num, UFS_FREE, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}
	else {
		f->f_inode.di_nlink--;
		r = write_inode(info, f->f_num, f->f_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}

	if (f->f_type == TYPE_DIR) {
		// Decrement parent directory's link count
		struct UFS_dinode dir_inode;
		struct UFS_cg cg;
		int cyl = f->f_num / info->super->fs_ipg;

		r = read_inode(info, pfile->f_num, &dir_inode);
		if (r < 0)
			goto ufs_remove_name_error;
		dir_inode.di_nlink--;
		r = write_inode(info, pfile->f_num, dir_inode, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;

		// Update group summary
		r = read_cg(info, cyl, &cg);
		if (r < 0)
			goto ufs_remove_name_error;

		r = update_summary(info, cyl, -1, 0, 0, 0, head, &newtail);
		if (r < 0)
			goto ufs_remove_name_error;
	}

	r = 0;

ufs_remove_name_error:
	ufs_free_fdesc(object, (fdesc_t *) f);
ufs_remove_name_error2:
	ufs_free_fdesc(object, (fdesc_t *) pfile);
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

static size_t ufs_get_num_features(LFS_t * object, inode_t ino)
{
	return sizeof(ufs_features) / sizeof(ufs_features[0]);
}

static const feature_t * ufs_get_feature(LFS_t * object, inode_t ino, size_t num)
{
	if(num < 0 || num >= sizeof(ufs_features) / sizeof(ufs_features[0]))
		return NULL;
	return ufs_features[num];
}

// TODO (permission feature, etc)
static int ufs_get_metadata(LFS_t * object, const ufs_fdesc_t * f, uint32_t id, size_t * size, void ** data)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (id == KFS_feature_size.id) {
		*data = malloc(sizeof(int32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(int32_t);
		memcpy(*data, &(f->f_inode.di_size), sizeof(int32_t));
	}
	else if (id == KFS_feature_filetype.id) {
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		*((uint32_t *) *data) = f->f_type;
	}
	else if (id == KFS_feature_nlinks.id) {
		*data = malloc(sizeof(int16_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(int16_t);
		memcpy(*data, &(f->f_inode.di_nlink), sizeof(int16_t));
	}
	else if (id == KFS_feature_freespace.id) {
		int free_space;
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		free_space = count_free_space(object) * info->super->fs_fsize / 1024;
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

static int ufs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, ino);
	int r;
	const ufs_fdesc_t * f = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	if (!f)
		return -E_NOT_FOUND;

	r = ufs_get_metadata(object, f, id, size, data);

	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	const ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	return ufs_get_metadata(object, f, id, size, data);
}

// TODO (permission feature, etc)
static int ufs_set_metadata(LFS_t * object, ufs_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	
	if (!head || !tail || !f || !data)
		return -E_INVAL;

	if (id == KFS_feature_size.id) {
		if (sizeof(uint32_t) != size || *((uint32_t *) data) >= UFS_MAXFILESIZE)
			return -E_INVAL;

		f->f_inode.di_size = *((uint32_t *) data);
		return write_inode(info, f->f_num, f->f_inode, head, tail);
	}
	else if (id == KFS_feature_filetype.id) {
		uint8_t fs_type;

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

		if (fs_type == (f->f_inode.di_mode >> 12))
			return 0;
		return -E_INVAL;
	}

	return -E_INVAL;
}

static int ufs_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	int r;
	ufs_fdesc_t * f = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	if (!f)
		return -E_INVAL;
	r = ufs_set_metadata(object, f, id, size, data, head, tail);
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	return ufs_set_metadata(object, f, id, size, data, head, tail);
}

#if 0
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
#endif

static int ufs_get_root(LFS_t * lfs, inode_t * ino)
{
	*ino = UFS_ROOT_INODE;
	return UFS_ROOT_INODE;
}

static int ufs_destroy(LFS_t * lfs)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(lfs);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);

	DESTROY(info->allocator);
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
	info->allocator = ufs_alloc_linear(info);
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
