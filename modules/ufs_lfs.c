/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/hash_map.h>

#include <fscore/debug.h>
#include <fscore/modman.h>

#include <modules/ufs_lfs.h>
#include <modules/ufs_common.h>
#include <modules/ufs_alloc_lastpos.h>
#include <modules/ufs_alloc_linear.h>
#include <modules/ufs_dirent_linear.h>
#include <modules/ufs_cg_wb.h>
#include <modules/ufs_super_wb.h>

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

static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file);
static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head);
static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head);
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ufs_set_metadata2(LFS_t * object, ufs_fdesc_t * f, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head);

#if 0
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
#endif

// TODO do more checks, move printf statements elsewhere, mark fs as unclean
static int check_super(LFS_t * object)
{
	struct ufs_info * info = (struct ufs_info *) object;
	uint32_t numblocks;
	int bs;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	// TODO better way of detecting fs block size
	/* make sure we have the block size we expect */
	bs = info->ubd->blocksize;
	if (bs != 2048) {
		printf("Block device size is not 2048! (%d)\n", bs);
		return -1;
	}

	if (super->fs_magic != UFS_MAGIC) {
		printf("ufs_base: bad file system magic number\n");
		printf("%x\n", super->fs_magic);
		return -1;
	}
	if (super->fs_inodefmt != 2) {
		printf("ufs_base: unsupported inode format\n");
		return -1;
	}

	numblocks = info->ubd->numblocks;
	info->ipf = super->fs_inopb / super->fs_frag;

	printf("Superblock size %d\n", super->fs_sbsize);
	printf("Superblock offset %d\n", super->fs_sblkno);
	printf("Number of blocks: %d, data blocks %d\n", super->fs_size, super->fs_dsize);
	printf("Block size: %d, fragment size %d, frags/block: %d\n", super->fs_bsize, super->fs_fsize, super->fs_frag);
	printf("Inodes per block: %d, sectors per fragment %d\n", super->fs_inopb, super->fs_nspf);
	printf("Inodes per group: %d, fragments per group %d\n", super->fs_ipg, super->fs_fpg);
	printf("Cylinder Groups: %d\n", super->fs_ncg);
	printf("Cylinder group offset %d, inode table offset %d\n", super->fs_cblkno, super->fs_iblkno);
	printf("cg_offset: %d, cgmask: 0x%x\n", super->fs_cgoffset, super->fs_cgmask);
	printf("internal symlink max length: %d\n", super->fs_maxsymlinklen);
	printf("Flags: fmod: %d, clean: %d, ronly: %d, flags: %d\n",
			super->fs_fmod, super->fs_clean, super->fs_ronly, super->fs_flags);
	printf("Superblock Cylinder Summary:\n\tDirectories: %d\n\tFree Blocks: %d\n\tFree Inodes: %d\n\tFree Frags: %d\n", super->fs_cstotal.cs_ndir,
			super->fs_cstotal.cs_nbfree, super->fs_cstotal.cs_nifree,
			super->fs_cstotal.cs_nffree);
	object->blocksize = super->fs_fsize;

	info->csum_block = CALL(info->ubd, read_block, super->fs_csaddr, 1, NULL);
	if (!info->csum_block)
	{
		printf("Unable to read cylinder summary!\n");
		return -1;
	}

	info->csums = smalloc(sizeof(struct UFS_csum) * super->fs_ncg);
	if (!info->csums)
		return -1;
	memcpy(info->csums, bdesc_data(info->csum_block),
			sizeof(struct UFS_csum) * super->fs_ncg);
	bdesc_retain(info->csum_block);

	return 0;
}

// Find a free block and allocate all fragments in the block
static uint32_t allocate_wholeblock(LFS_t * object, int wipe, fdesc_t * file, patch_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	uint32_t i, num;
	bdesc_t * block;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	assert(!file || f->f_type != TYPE_SYMLINK);

	if (!head)
		return INVALID_BLOCK;

	num = CALL(info->parts.p_allocator, find_free_block, file, 0);
	if (num == INVALID_BLOCK)
		return INVALID_BLOCK;

	// Mark the fragments as used
	for (i = num * super->fs_frag; i < (num + 1) * super->fs_frag; i++) {
		r = ufs_write_fragment_bitmap(info, i, UFS_USED, head);
		if (r < 0)
			return INVALID_BLOCK;
		assert(r != 1); // This should not happen

		if (wipe) {
			// TODO: lookup page_t* for this block?
			block = CALL(info->ubd, synthetic_read_block, i, 1, NULL);
			// FIXME revert all previously allocated blocks?
			if (!block)
				return INVALID_BLOCK;
			r = patch_create_init(block, info->ubd, head);
			if (r >= 0)
			{
				FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "wipe block");
				r = CALL(info->ubd, write_block, block, i);
			}
			if (r < 0)
				return INVALID_BLOCK;
		}
	}

	if (file) {
		f->f_inode.di_blocks += 32; // charge the fragments to the file
		r = ufs_write_inode(info, f->f_num, f->f_inode, head);
		if (r < 0)
			return INVALID_BLOCK;
	}

	return num * super->fs_frag;
}

// Deallocate an entire block
static int erase_wholeblock(LFS_t * object, uint32_t num, fdesc_t * file, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, num);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	uint32_t i;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	assert(!file || f->f_type != TYPE_SYMLINK);

	if (!head || num == INVALID_BLOCK)
		return -EINVAL;

	// Mark the fragments as used
	for (i = num * super->fs_frag; i < (num + 1) * super->fs_frag; i++) {
		r = ufs_write_fragment_bitmap(info, i, UFS_FREE, head);
		if (r < 0)
			return r;
		assert(r != 1); // This should not happen
	}

	if (file) {
		f->f_inode.di_blocks -= 32; // charge the fragments to the file
		r = ufs_write_inode(info, f->f_num, f->f_inode, head);
		if (r < 0)
			return r;
	}

	return 0;
}

// Update a ptr in an indirect ptr block
static inline int update_indirect_block(struct ufs_info * info, bdesc_t * block, uint32_t block_number, uint32_t offset, uint32_t n, patch_t ** head)
{
	int r = patch_create_byte(block, info->ubd, offset * sizeof(n), sizeof(n), &n, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "indirect pointer");
	return CALL(info->ubd, write_block, block, block_number);
}

// Update file's inode with an nth indirect ptr
static int modify_indirect_ptr(LFS_t * object, fdesc_t * file, int n, bool evil, patch_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t newblock;

	if (!file || !head || n < 0 || n >= UFS_NIADDR)
		return -EINVAL;

	// Beware of the evil bit? ;)
	if (evil) {
		// Clears the indirect pointer...
		f->f_inode.di_ib[n] = 0;
		return ufs_write_inode(info, f->f_num, f->f_inode, head);
	}
	else {
		// Allocates an indirect pointer block
		if (f->f_inode.di_ib[n])
			return -1;

		newblock = allocate_wholeblock(object, 1, file, head);
		if (newblock == INVALID_BLOCK)
			return -ENOENT;
		f->f_inode.di_ib[n] = newblock;
		return ufs_write_inode(info, f->f_num, f->f_inode, head);
	}
}

// Write the block ptrs for a file, allocate indirect blocks as needed
// Offset is a byte offset
static int write_block_ptr(LFS_t * object, fdesc_t * file, uint32_t offset, uint32_t value, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %p %d %d\n", __FUNCTION__, file, offset, value);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	uint32_t blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR], indirect_number[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !file || offset % super->fs_bsize)
		return -EINVAL;
	assert(f->f_type != TYPE_SYMLINK);

	nindirb = super->fs_nindir;
	nindirf = nindirb / super->fs_frag;
	blockno = offset / super->fs_bsize;

	if (blockno < UFS_NDADDR) {
		f->f_inode.di_db[blockno] = value;
		return ufs_write_inode(info, f->f_num, f->f_inode, head);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		// Allocate single indirect block if needed
		if (!f->f_inode.di_ib[0]) {
			r = modify_indirect_ptr(object, file, 0, 0, head);
			if (r < 0)
				return r;
		}

		indirect_number[0] = f->f_inode.di_ib[0] + frag_off[0];
		indirect[0] = CALL(info->ubd, read_block,
				indirect_number[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		return update_indirect_block(info, indirect[0], indirect_number[0], pt_off[0], value, head);
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		// Allocate double indirect block if needed
		if (!f->f_inode.di_ib[1]) {
			r = modify_indirect_ptr(object, file, 1, 0, head);
			if (r < 0)
				return r;
		}

		indirect_number[1] = f->f_inode.di_ib[1] + frag_off[1];
		indirect[1] = CALL(info->ubd, read_block,
				indirect_number[1], 1, NULL);
		if (!indirect[1])
			return -ENOENT;

		block_off[0] = *((uint32_t *) (bdesc_data(indirect[1])) + pt_off[1]);

		// Allocate single indirect block if needed
		if (!block_off[0]) {
			block_off[0] = allocate_wholeblock(object, 1, file, head);
			if (block_off[0] == INVALID_BLOCK)
				return -ENOENT;
			r = update_indirect_block(info, indirect[1], indirect_number[1], pt_off[1], block_off[0], head);
			if (r < 0)
				return r;
		}

		indirect_number[0] = block_off[0] + frag_off[0];
		indirect[0] = CALL(info->ubd, read_block, indirect_number[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		return update_indirect_block(info, indirect[0], indirect_number[0], pt_off[0], value, head);
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		// We'll only need triple indirect ptrs when the filesize is:
		//  4 KB Blocksize: > 4GB
		//  8 KB Blocksize: > 32GB
		// 16 KB Blocksize: > 256GB

		// FIXME write some tedious code
	}

	return -1;
}

// Erase the block ptrs for a file, deallocate indirect blocks as needed
// Offset is a byte offset
static int erase_block_ptr(LFS_t * object, fdesc_t * file, uint32_t offset, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	uint32_t blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	uint32_t num[UFS_NIADDR], indirect_number[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !file || offset % super->fs_bsize)
		return -EINVAL;
	assert(f->f_type != TYPE_SYMLINK);

	nindirb = super->fs_nindir;
	nindirf = nindirb / super->fs_frag;
	blockno = offset / super->fs_bsize;

	if (blockno < UFS_NDADDR) {
		f->f_inode.di_db[blockno] = 0;
		return ufs_write_inode(info, f->f_num, f->f_inode, head);
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;
		num[0] = f->f_inode.di_ib[0] / super->fs_frag;

		indirect_number[0] = f->f_inode.di_ib[0] + frag_off[0];
		indirect[0] = CALL(info->ubd, read_block,
				indirect_number[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		r = update_indirect_block(info, indirect[0], indirect_number[0], pt_off[0], 0, head);
		// Deallocate indirect block if necessary
		if (blockno == UFS_NDADDR && r >= 0) {
			r = modify_indirect_ptr(object, file, 0, 1, head);
			if (r >= 0)
				r = erase_wholeblock(object, num[0], file, head);
		}
		return r;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;
		num[1] = f->f_inode.di_ib[1] / super->fs_frag;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		indirect_number[1] = f->f_inode.di_ib[1] + frag_off[1];
		indirect[1] = CALL(info->ubd, read_block,
				indirect_number[1], 1, NULL);
		if (!indirect[1])
			return -ENOENT;

		block_off[0] = *((uint32_t *) bdesc_data(indirect[1]) + pt_off[1]);
		num[0] = block_off[0] / super->fs_frag;

		indirect_number[0] = block_off[0] + frag_off[0];
		indirect[0] = CALL(info->ubd, read_block, indirect_number[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		r = update_indirect_block(info, indirect[0], indirect_number[0], pt_off[0], 0, head);

		// Deallocate indirect block if necessary
		if ((block_off[1] % nindirb == 0) && r >= 0) {
			r = update_indirect_block(info, indirect[1], indirect_number[1], pt_off[1], 0, head);
			if (r >= 0)
				r = erase_wholeblock(object, num[0], file, head);
		}

		// Deallocate double indirect block if necessary
		if (blockno == UFS_NDADDR + nindirb && r >= 0) {
			r = modify_indirect_ptr(object, file, 1, 1, head);
			if (r >= 0)
				r = erase_wholeblock(object, num[1], file, head);
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

	return -1;
}

static inline uint32_t count_free_space(struct ufs_info * info)
{
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	return super->fs_cstotal.cs_nbfree * super->fs_frag
		+ super->fs_cstotal.cs_nffree;
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
			assert(uf->count >= 1);
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
	int r;

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
	new_file->common = &new_file->base;
	new_file->base.parent = INODE_NONE;

	// If file struct is not in memory
	existing_file = open_ufsfile_create(new_file);
	if (!existing_file) {
		free(new_file);
		return NULL;
	}
	r = hash_map_insert(filemap, (void *) ino, existing_file);
	assert(r == 0);
	return existing_file;
}

static uint32_t find_frags_new_home(LFS_t * object, fdesc_t * file, int purpose, patch_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t i, blockno, offset;
	int r, frags;
	bdesc_t * block, * newblock;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !file)
		return INVALID_BLOCK;
	assert(f->f_type != TYPE_SYMLINK);

	frags = f->f_numfrags % super->fs_frag;
	offset = (f->f_numfrags - frags) * super->fs_size;

	// Time to allocate a new block and copy the data there
	// FIXME handle failure case better?

	// find new block
	blockno = CALL(info->parts.p_allocator, find_free_block, file, purpose);
	if (blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	blockno *= super->fs_frag;

	// allocate some fragments
	for (i = 0 ; i < frags; i++) {
		r = ufs_write_fragment_bitmap(info, blockno + i, UFS_USED, head);
		if (r != 0)
			return INVALID_BLOCK;
	}

	// read in fragments, and write to new location
	for (i = 0 ; i < frags; i++) {
		// TODO: lookup page_t* for this block?
		block = CALL(info->ubd, read_block, f->f_lastfrag - frags + i + 1, 1, NULL);
		if (!block)
			return INVALID_BLOCK;
		bdesc_retain(block);
		// TODO: lookup page_t* for this block?
		newblock = CALL(info->ubd, synthetic_read_block, blockno + i, 1, NULL);
		if (!newblock)
			goto find_frags_new_home_failed;

		r = patch_create_full(newblock, info->ubd, bdesc_data(block), head);
		if (r < 0)
			goto find_frags_new_home_failed;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "move fragment");

		bdesc_release(&block);
		r = CALL(info->ubd, write_block, newblock, blockno + i);
		if (r < 0)
			return INVALID_BLOCK;
	}

	// update block ptr
	r = write_block_ptr(object, file, offset, blockno, head);

	// free old fragments
	for (i = 0 ; i < frags; i++) {
		r = ufs_write_fragment_bitmap(info, f->f_lastfrag - frags + i + 1, UFS_FREE, head);
		if (r != 0)
			return INVALID_BLOCK;
	}

	blockno = blockno + frags;
	f->f_lastfrag = blockno - 1;

	return blockno;

find_frags_new_home_failed:
	bdesc_release(&block);
	return INVALID_BLOCK;
}

// Allocates fragments, really
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t blockno;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (f->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;

	// FIXME require file to be non-null for now
	if (!head || !file)
		return INVALID_BLOCK;

	if (f->f_lastalloc != INVALID_BLOCK)
		// We already allocated a fragment, go use that first
		return INVALID_BLOCK;

	// File has no fragments
	if (f->f_numfrags == 0) {
		blockno = CALL(info->parts.p_allocator, find_free_frag, file, purpose);
		if (blockno == INVALID_BLOCK)
			return INVALID_BLOCK;
	}
	// We're using indirect pointers, time to allocate whole blocks
	else if (f->f_numfrags >= UFS_NDADDR * super->fs_frag) {
		// Well, except we're still working with fragments here

		// Time to allocate a new block
		if (((f->f_lastfrag + 1) % super->fs_frag) == 0) {
			blockno = allocate_wholeblock(object, 0, file, head);
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
	else if (((f->f_lastfrag + 1) % super->fs_frag) == 0) {
		if (f->f_numfrags % super->fs_frag) {
			blockno = find_frags_new_home(object, file, purpose, head);
		}
		else {
			blockno = CALL(info->parts.p_allocator, find_free_block, file, purpose);
			if (blockno == INVALID_BLOCK)
				return INVALID_BLOCK;
			blockno *= super->fs_frag;
		}
	}
	// Use the next fragment
	else {
		r = ufs_read_fragment_bitmap(info, f->f_lastfrag + 1);
		if (r < 0)
			return r;
		else if (r == UFS_FREE)
			blockno = f->f_lastfrag + 1; // UFS says we must use it
		else // Next fragment is taken, move elsewhere
			blockno = find_frags_new_home(object, file, purpose, head);
	}
	if(blockno == INVALID_BLOCK)
		return INVALID_BLOCK;

	r = ufs_write_fragment_bitmap(info, blockno, UFS_USED, head);
	if (r != 0)
		return INVALID_BLOCK;

	r = ufs_read_fragment_bitmap(info, blockno);
	assert(r == UFS_USED);

	f->f_inode.di_blocks += 4; // grr, di_blocks counts 512 byte blocks
	r = ufs_write_inode(info, f->f_num, f->f_inode, head);
	if (r < 0)
		goto allocate_block_cleanup;

	f->f_lastalloc = blockno;
	return blockno;

allocate_block_cleanup:
	r = ufs_write_fragment_bitmap(info, blockno, UFS_FREE, head);
	assert(r == 0);
	return INVALID_BLOCK;
}

static fdesc_t * ufs_lookup_inode(LFS_t * object, inode_t ino)
{
	struct ufs_info * info = (struct ufs_info *) object;
	open_ufsfile_t * ef;
	int r, exists = -1;
	uint8_t type;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (ino == 0)
		return NULL;

	ef = get_ufsfile(info->filemap, ino, &exists);
	if (!ef)
		return NULL;

	if (exists == 1)
		return (fdesc_t *) ef->file;
	else if (exists == 0) {
		r = ufs_read_inode(info, ino, &ef->file->f_inode);
		if (r < 0) {
			open_ufsfile_destroy(ef);
			return NULL;
		}
		ef->file->f_lastalloc = INVALID_BLOCK;
		ef->file->f_num = ino;
		type = ef->file->f_inode.di_mode >> 12;
		ef->file->f_type = ufs_to_fstitch_type(type);
		ef->file->f_numfrags = ufs_get_file_numblocks(object, (fdesc_t *) ef->file);
		ef->file->f_lastfrag = ufs_get_file_block(object, (fdesc_t *) ef->file, (ef->file->f_numfrags - 1) * super->fs_fsize);
		return (fdesc_t *) ef->file;
	}

	return NULL;
}

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, number);
	struct ufs_info * info = (struct ufs_info *) object;
	return CALL(info->ubd, read_block, number, 1, page);
}

static bdesc_t * ufs_synthetic_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, number);
	struct ufs_info * info = (struct ufs_info *) object;
	return CALL(info->ubd, synthetic_read_block, number, 1, page);
}

static void ufs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("UFSDEBUG: %s %p\n", __FUNCTION__, fdesc);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) fdesc;
	open_ufsfile_t * uf;

	if (f) {
		uf = hash_map_find_val(info->filemap, (void *) f->f_num);
		if (uf) {
			if (uf->count < 2)
				hash_map_erase(info->filemap, (void *) f->f_num);
			open_ufsfile_destroy(uf);
		}
	}
}

static int ufs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("UFSDEBUG: %s %d, %s\n", __FUNCTION__, parent, name);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * pfile;
	int r;

	if (!ino || ufs_check_name(name))
		return -EINVAL;

	pfile = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pfile)
		return -ENOENT;

	if (pfile->f_type != TYPE_DIR)
		return -ENOTDIR;

	r = CALL(info->parts.p_dirent, search_dirent, pfile, name, ino, NULL);
	ufs_free_fdesc(object, (fdesc_t *) pfile);
	return r;
}

static uint32_t ufs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	Dprintf("UFSDEBUG: %s %p\n", __FUNCTION__, file);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t n;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (f->f_type == TYPE_SYMLINK)
		return 0;

	assert(ROUNDUP32(super->fs_fsize, 2) == super->fs_fsize);
	n = f->f_inode.di_size >> super->fs_fshift;
	if (f->f_inode.di_size != (n << super->fs_fshift))
		n++;

	return n;
}

// Offset is a byte offset
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("UFSDEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t fragno, blockno, nindirb, nindirf;
	uint32_t block_off[UFS_NIADDR], frag_off[UFS_NIADDR], pt_off[UFS_NIADDR];
	bdesc_t * indirect[UFS_NIADDR];
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (offset % super->fs_fsize || offset >= f->f_inode.di_size || f->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;

	nindirb = super->fs_nindir;
	nindirf = nindirb / super->fs_frag;
	blockno = offset / super->fs_bsize;
	fragno = (offset / super->fs_fsize) % super->fs_frag;

	if (blockno < UFS_NDADDR) {
		return f->f_inode.di_db[blockno] + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb) {
		block_off[0] = blockno - UFS_NDADDR;
		frag_off[0] = block_off[0] / nindirf;
		pt_off[0] = block_off[0] % nindirf;

		indirect[0] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[0] + frag_off[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		return (*((uint32_t *) bdesc_data(indirect[0]) + pt_off[0])) + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb) {
		block_off[1] = blockno - UFS_NDADDR - nindirb;
		frag_off[1] = block_off[1] / nindirf / nindirb;
		pt_off[1] = (block_off[1] / nindirb) % nindirf;

		frag_off[0] = (block_off[1] % nindirb) / nindirf;
		pt_off[0] = block_off[1] % nindirf;

		indirect[1] = CALL(info->ubd, read_block,
				f->f_inode.di_ib[1] + frag_off[1], 1, NULL);
		if (!indirect[1])
			return -ENOENT;

		block_off[0] = *((uint32_t *) bdesc_data(indirect[1]) + pt_off[1]);

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0], 1, NULL);
		if (!indirect[0])
			return -ENOENT;

		return (*((uint32_t *) bdesc_data(indirect[0]) + pt_off[0])) + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		// We'll only need triple indirect ptrs when the filesize is:
		//  4 KB Blocksize: > 4GB
		//  8 KB Blocksize: > 32GB
		// 16 KB Blocksize: > 256GB

		// FIXME write some tedious code
	}

	return -1;
}

static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct ufs_info * info = (struct ufs_info *) object;
	int r;

	do {
		r = CALL(info->parts.p_dirent, get_dirent, (ufs_fdesc_t *) file, entry, size, basep);
		if (r < 0)
			return r;
	} while (entry->d_fileno == 0);

	return r;
}

static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t offset;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (f->f_type == TYPE_SYMLINK)
		return -EINVAL;

	if (!head || !f || block == INVALID_BLOCK)
		return -EINVAL;

	if (block != f->f_lastalloc)
		// hmm, that's not the right block
		return -1;

	if (f->f_numfrags % super->fs_frag) {
		// not appending to a new block,
		// the fragment has been attached implicitly
		f->f_numfrags++;
		f->f_lastfrag = block;
		f->f_lastalloc = INVALID_BLOCK;

		return 0;
	}

	offset = f->f_numfrags * super->fs_fsize;
	r = write_block_ptr(object, file, offset, block, head);
	if (r < 0)
		return r;

	f->f_numfrags++;
	f->f_lastfrag = block;
	f->f_lastalloc = INVALID_BLOCK;

	return 0;
}

static int empty_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	return -ENOENT;
}

static char link_buf[UFS_MAXPATHLEN];

static fdesc_t * allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, patch_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * nf;
	ufs_fdesc_t * pf;
	open_ufsfile_t * open_file;
	ufs_fdesc_t * ln = (ufs_fdesc_t *) link;
	uint32_t inum = 0;
	int r, createdot = 0, ex;
	uint16_t mode;
	uint32_t x32;
	uint16_t x16;
	struct dirent dirinfo;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };

	if (!head || ufs_check_name(name))
		return NULL;

	switch (type)
	{
		case TYPE_FILE:
			mode = UFS_IFREG;
			break;
		case TYPE_SYMLINK:
			mode = UFS_IFLNK;
			break;
		case TYPE_DIR:
			mode = UFS_IFDIR;
			break;
		default:
			return NULL;
	}

	// Don't create directory hard links, except for . and ..
	if (!strcmp(name, "."))
		createdot = 1;
	else if (!strcmp(name, ".."))
		createdot = 1;

	// Don't create .  and .. when we are linking to an existing directory
	if (ln && !createdot && type == TYPE_DIR)
		createdot = 1;

	// Don't link files of different types
	if (ln && type != ln->f_type)
		return NULL;

	pf = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pf)
		return NULL;

	r = CALL(info->parts.p_dirent, search_dirent, pf, name, NULL, NULL);
	if (r >= 0) // File exists already
		goto allocate_name_exit;

	if (!ln) {
		// Allocate new inode
		inum = CALL(info->parts.p_allocator, find_free_inode, (fdesc_t *) pf, 0);
		if (inum == INVALID_BLOCK)
			goto allocate_name_exit;

		open_file = get_ufsfile(info->filemap, inum, &ex);
		if (!open_file)
			goto allocate_name_exit;
		assert(ex == 0);

		nf = open_file->file;

		nf->f_numfrags = 0;
		nf->f_lastfrag = 0;
		nf->f_lastalloc = INVALID_BLOCK;

		nf->f_num = inum;
		nf->f_type = type;

		memset(&nf->f_inode, 0, sizeof(struct UFS_dinode));

		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UID, sizeof(x32), &x32);
		if (r > 0)
			nf->f_inode.di_uid = x32;
		else if (r == -ENOENT)
			nf->f_inode.di_uid = 0;
		else
			assert(0);

		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_GID, sizeof(x32), &x32);
		if (r > 0)
			nf->f_inode.di_gid = x32;
		else if (r == -ENOENT)
			nf->f_inode.di_gid = 0;
		else
			assert(0);

		nf->f_inode.di_mode = mode | UFS_IREAD | UFS_IWRITE;
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UNIX_PERM, sizeof(x16), &x16);
		if (r > 0)
			nf->f_inode.di_mode |= x16;
		else if (r != -ENOENT)
			assert(0);
			
		nf->f_inode.di_nlink = 1;
		nf->f_inode.di_gen = 0; // FIXME use random number?

		if (type == TYPE_SYMLINK)
		{
			r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_SYMLINK, sizeof(link_buf), link_buf);
			if (r < 0)
				goto allocate_name_exit2;
			else
			{
				fsmetadata_t fsm;
				fsm.fsm_feature = FSTITCH_FEATURE_SYMLINK;
				fsm.fsm_value.p.data = link_buf;
				fsm.fsm_value.p.length = r;
				r = ufs_set_metadata2(object, nf, &fsm, 1, head);
				if (r < 0)
					goto allocate_name_exit2;
			}
		}

		// Write new inode to disk and allocate it
		r = ufs_write_inode(info, inum, nf->f_inode, head);
		if (r < 0)
			goto allocate_name_exit2;

		r = ufs_write_inode_bitmap(info, inum, UFS_USED, head);
		if (r != 0)
			goto allocate_name_exit2;

		*newino = inum;
	}
	else {
		open_file = get_ufsfile(info->filemap, ln->f_num, &ex);
		if (!open_file)
			goto allocate_name_exit;
		assert(ex == 1);
		nf = open_file->file;
		*newino = ln->f_num;
	}

	// Create directory entry
	dirinfo.d_fileno = nf->f_num;
	dirinfo.d_filesize = nf->f_inode.di_size;
	dirinfo.d_type = nf->f_type;
	strcpy(dirinfo.d_name, name);
	dirinfo.d_namelen = strlen(name);
	dirinfo.d_reclen = sizeof(struct dirent) + dirinfo.d_namelen - DIRENT_MAXNAMELEN;
	r = CALL(info->parts.p_dirent, insert_dirent, pf, dirinfo, head);
	if (r < 0) {
		if (!ln)
			ufs_write_inode_bitmap(info, inum, UFS_FREE, head);
		goto allocate_name_exit2;
	}

	// Increase link count
	if (ln) {
		nf->f_inode.di_nlink++;
		r = ufs_write_inode(info, nf->f_num, nf->f_inode, head);
		if (r < 0)
			goto allocate_name_exit2;
	}

	// Create . and ..
	if (type == TYPE_DIR && !createdot) {
		fdesc_t * cfdesc;
		inode_t newino;

		cfdesc = allocate_name(object, nf->f_num, ".", TYPE_DIR, (fdesc_t *) nf, &emptymd, &newino, head);
		if (!cfdesc)
			goto allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);

		cfdesc = allocate_name(object, nf->f_num, "..", TYPE_DIR, (fdesc_t *) pf, &emptymd, &newino, head);
		if (!cfdesc)
			goto allocate_name_exit2;
		ufs_free_fdesc(object, cfdesc);

		r = ufs_update_summary(info, inum / super->fs_ipg, 1, 0, 0, 0, head);
		if (r < 0)
			goto allocate_name_exit2;
	}

	ufs_free_fdesc(object, (fdesc_t *) pf);
	return (fdesc_t *) nf;

allocate_name_exit2:
	ufs_free_fdesc(object, (fdesc_t *) nf);
allocate_name_exit:
	ufs_free_fdesc(object, (fdesc_t *) pf);
	*newino = INODE_NONE;
	return NULL;
}

static fdesc_t * ufs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %s\n", __FUNCTION__, name);
	int createdot = 0;

	if (!head || ufs_check_name(name))
		return NULL;

	// Users cannot create . and ..
	if (!strcmp(name, "."))
		createdot = 1;
	else if (!strcmp(name, ".."))
		createdot = 1;

	if (createdot)
		return NULL;

	return allocate_name(object, parent, name, type, link, initialmd, newino, head);
}

static int ufs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %s %s\n", __FUNCTION__, oldname, newname);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * old_pfdesc;
	ufs_fdesc_t * new_pfdesc;
	ufs_fdesc_t * oldf;
	ufs_fdesc_t * newf;
	struct dirent entry;
	uint32_t p;
	int r, existing = 0, dir_offset;
	inode_t ino, newino;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };

	if (!head || ufs_check_name(oldname) || ufs_check_name(newname))
		return -EINVAL;

	if (!strcmp(oldname, newname) && (oldparent == newparent)) // Umm, ok
		return 0;

	old_pfdesc = (ufs_fdesc_t *) ufs_lookup_inode(object, oldparent);
	if (!old_pfdesc)
		return -ENOENT;

	r = CALL(info->parts.p_dirent, search_dirent, old_pfdesc, oldname, &ino, NULL);
	if (r < 0)
		goto ufs_rename_exit;

	oldf = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	if (!oldf) {
		r = -ENOENT;
		goto ufs_rename_exit;
	}

	new_pfdesc = (ufs_fdesc_t *) ufs_lookup_inode(object, newparent);
	if (!new_pfdesc) {
		r = -ENOENT;
		goto ufs_rename_exit2;
	}

	r = CALL(info->parts.p_dirent, search_dirent, new_pfdesc, newname, &ino, &dir_offset);
	if (r < 0)
		if (r == -ENOENT)
			newf = NULL;
		else
			goto ufs_rename_exit3;
	else {
		assert(dir_offset >= 0);
		newf = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	}

	if (newf) {
		// Overwriting a directory makes little sense
		if (newf->f_type == TYPE_DIR) {
			r = -ENOTEMPTY;
			goto ufs_rename_exit4;
		}

		// File already exists
		existing = 1;

		p = dir_offset;
		r = CALL(info->parts.p_dirent, get_dirent, new_pfdesc, &entry, sizeof(entry), &p);
		if (r < 0)
			goto ufs_rename_exit4;

		entry.d_fileno = oldf->f_num;
		r = CALL(info->parts.p_dirent, modify_dirent, new_pfdesc, entry, dir_offset, head);
		if (r < 0)
			goto ufs_rename_exit4;

		oldf->f_inode.di_nlink++;
		r = ufs_write_inode(info, oldf->f_num, oldf->f_inode, head);
		if (r < 0)
			goto ufs_rename_exit4;
	}
	else {
		// Link files together
		newf = (ufs_fdesc_t *) ufs_allocate_name(object, newparent, newname, oldf->f_type, (fdesc_t *) oldf, &emptymd, &newino, head);
		if (!newf) {
			r = -1;
			goto ufs_rename_exit3;
		}
		assert(ino == newino);
	}

	r = CALL(info->parts.p_dirent, delete_dirent, old_pfdesc, oldname, head);
	if (r < 0)
		goto ufs_rename_exit4;

	oldf->f_inode.di_nlink--;
	r = ufs_write_inode(info, oldf->f_num, oldf->f_inode, head);
	if (r < 0)
		goto ufs_rename_exit4;

	if (existing) {
		newf->f_inode.di_nlink--;
		r = ufs_write_inode(info, newf->f_num, newf->f_inode, head);
		if (r < 0)
			goto ufs_rename_exit4;

	   if (newf->f_inode.di_nlink == 0) {
		   uint32_t block, i, n = newf->f_numfrags;
		   for (i = 0; i < n; i++) {
			   block = ufs_truncate_file_block(object, (fdesc_t *) newf, head);
			   if (block == INVALID_BLOCK) {
				   r = -1;
				   goto ufs_rename_exit4;
			   }
			   r = ufs_free_block(object, (fdesc_t *) newf, block, head);
			   if (r < 0)
				   goto ufs_rename_exit4;
		   }

		   memset(&newf->f_inode, 0, sizeof(struct UFS_dinode));
		   r = ufs_write_inode(info, newf->f_num, newf->f_inode, head);
		   if (r < 0)
			   goto ufs_rename_exit4;

		   r = ufs_write_inode_bitmap(info, newf->f_num, UFS_FREE, head);
		   if (r < 0)
			   goto ufs_rename_exit4;
	   }
	}

	r = 0;

ufs_rename_exit4:
	ufs_free_fdesc(object, (fdesc_t *) newf);
ufs_rename_exit3:
	ufs_free_fdesc(object, (fdesc_t *) new_pfdesc);
ufs_rename_exit2:
	ufs_free_fdesc(object, (fdesc_t *) oldf);
ufs_rename_exit:
	ufs_free_fdesc(object, (fdesc_t *) old_pfdesc);
	return r;
}

static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	uint32_t offset, blockno, truncated;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !f || f->f_numfrags == 0 || f->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;

	truncated = f->f_lastfrag;
	assert(truncated != INVALID_BLOCK);

	if ((f->f_numfrags - 1) % super->fs_frag) {

		// not truncating the entire block
		// the fragment has been attached implicitly
		f->f_numfrags--;
		f->f_lastfrag--;

		return truncated;
	}

	offset = (f->f_numfrags - 1) * super->fs_fsize;
	r = erase_block_ptr(object, file, offset, head);
	if (r < 0)
		return INVALID_BLOCK;

	if (offset != 0) {
		offset -= super->fs_bsize;
		blockno = ufs_get_file_block(object, file, offset);
		assert(blockno != INVALID_BLOCK); // FIXME handle better
		f->f_lastfrag = blockno + super->fs_frag - 1;
	}
	else
		f->f_lastfrag = 0;

	f->f_numfrags--;

	return truncated;
}

/* FIXME: we need to unset f_lastalloc (i.e. to INVALID_BLOCK) here somehow */
static int ufs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, block);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || f->f_type == TYPE_SYMLINK)
		return -EINVAL;

	if (file) {
		// Whole block time
		if (f->f_numfrags >= UFS_NDADDR * super->fs_frag) {
			if (f->f_numfrags % super->fs_frag == 0) {
				assert(block % super->fs_frag == 0);

				// free the entire block
				return erase_wholeblock(object, block / super->fs_frag, file, head);
			}
			else {
				// Do nothing
				return 0;
			}
		}
		else {
			f->f_inode.di_blocks -= 4;
			r = ufs_write_inode(info, f->f_num, f->f_inode, head);
			if (r < 0)
				return r;
			return ufs_write_fragment_bitmap(info, block, UFS_FREE, head);
		}
	}

	// Free the fragment, no questions asked
	return ufs_write_fragment_bitmap(info, block, UFS_FREE, head);
}

static int ufs_remove_name(LFS_t * object, inode_t parent, const char * name, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s %d %s\n", __FUNCTION__, parent, name);
	struct ufs_info * info = (struct ufs_info *) object;
	ufs_fdesc_t * pfile;
	ufs_fdesc_t * f;
	inode_t filenum;
	int r, minlinks = 1;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || ufs_check_name(name))
		return -EINVAL;

	pfile = (ufs_fdesc_t *) ufs_lookup_inode(object, parent);
	if (!pfile)
		return -ENOENT;

	if (pfile->f_type != TYPE_DIR) {
		r = -ENOTDIR;
		goto ufs_remove_name_error2;
	}

	r = CALL(info->parts.p_dirent, search_dirent, pfile, name, &filenum, NULL);
	if (r < 0)
		goto ufs_remove_name_error2;

	f = (ufs_fdesc_t *) ufs_lookup_inode(object, filenum);
	if (!f) {
		r = -ENOENT;
		goto ufs_remove_name_error2;
	}

	if (f->f_type == TYPE_DIR) {
		if (f->f_inode.di_nlink > 2 && !strcmp(name, "..")) {
			r = -ENOTEMPTY;
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
	r = CALL(info->parts.p_dirent, delete_dirent, pfile, name, head);
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
				number = ufs_truncate_file_block(object, (fdesc_t *) f, head);
				if (number == INVALID_BLOCK) {
					r = -EINVAL;
					goto ufs_remove_name_error;
				}

				r = ufs_free_block(object, (fdesc_t *) f, number, head);
				if (r < 0)
					goto ufs_remove_name_error;
			}
		}

		// Clear inode
		memset(&f->f_inode, 0, sizeof(struct UFS_dinode));
		r = ufs_write_inode(info, f->f_num, f->f_inode, head);
		if (r < 0)
			goto ufs_remove_name_error;

		r = ufs_write_inode_bitmap(info, f->f_num, UFS_FREE, head);
		if (r < 0)
			goto ufs_remove_name_error;
	}
	else {
		f->f_inode.di_nlink--;
		r = ufs_write_inode(info, f->f_num, f->f_inode, head);
		if (r < 0)
			goto ufs_remove_name_error;
	}

	if (f->f_type == TYPE_DIR) {
		int cyl = f->f_num / super->fs_ipg;

		pfile->f_inode.di_nlink--;
		r = ufs_write_inode(info, pfile->f_num, pfile->f_inode, head);
		if (r < 0)
			goto ufs_remove_name_error;

		// Update group summary
		r = ufs_update_summary(info, cyl, -1, 0, 0, 0, head);
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

static int ufs_write_block(LFS_t * object, bdesc_t * block, uint32_t number, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;
	assert(head);

	return CALL(info->ubd, write_block, block, number);
}

static patch_t ** ufs_get_write_head(LFS_t * object)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;
	return info->write_head;
}

static int32_t ufs_get_block_space(LFS_t * object)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;
	return CALL(info->ubd, get_block_space);
}

static const bool ufs_features[] = {[FSTITCH_FEATURE_SIZE] = 1, [FSTITCH_FEATURE_FILETYPE] = 1, [FSTITCH_FEATURE_NLINKS] = 1, [FSTITCH_FEATURE_FILE_LFS] = 1, [FSTITCH_FEATURE_UID] = 1, [FSTITCH_FEATURE_GID] = 1, [FSTITCH_FEATURE_UNIX_PERM] = 1, [FSTITCH_FEATURE_BLOCKSIZE] = 1, [FSTITCH_FEATURE_DEVSIZE] = 1, [FSTITCH_FEATURE_MTIME] = 1, [FSTITCH_FEATURE_SYMLINK] = 1};

static size_t ufs_get_max_feature_id(LFS_t * object)
{
	return sizeof(ufs_features) / sizeof(ufs_features[0]) - 1;
}

static const bool * ufs_get_feature_array(LFS_t * object)
{
	return ufs_features;
}

static int ufs_get_metadata(LFS_t * object, const ufs_fdesc_t * f, uint32_t id, size_t size, void * data)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;

	if (id == FSTITCH_FEATURE_SIZE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(int32_t))
			return -ENOMEM;
		size = sizeof(int32_t);

		*((int32_t *) data) = f->f_inode.di_size;
	}
	else if (id == FSTITCH_FEATURE_FILETYPE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_type;
	}
	else if (id == FSTITCH_FEATURE_NLINKS) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = (uint32_t) f->f_inode.di_nlink;
	}
	else if (id == FSTITCH_FEATURE_FREESPACE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = count_free_space(info);
	}
	else if (id == FSTITCH_FEATURE_FILE_LFS) {
		if (size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);

		*((typeof(object) *) data) = object;
	}
	else if (id == FSTITCH_FEATURE_UID) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.di_uid;
	}
	else if (id == FSTITCH_FEATURE_GID) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.di_gid;
	}
	else if (id == FSTITCH_FEATURE_UNIX_PERM) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);

		*((uint16_t *) data) = f->f_inode.di_mode & UFS_IPERM;
	}
	else if (id == FSTITCH_FEATURE_BLOCKSIZE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = object->blocksize;
	}
	else if (id == FSTITCH_FEATURE_DEVSIZE) {
		const struct UFS_Super * super = CALL(info->parts.p_super, read);
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = super->fs_dsize;
	}
	else if (id == FSTITCH_FEATURE_MTIME) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(int32_t))
			return -ENOMEM;
		size = sizeof(int32_t);

		*((int32_t *) data) = f->f_inode.di_mtime;
	}
	else if (id == FSTITCH_FEATURE_SYMLINK) {
		if (!f || f->f_type != TYPE_SYMLINK)
			return -EINVAL;

		if (size < f->f_inode.di_size)
			return -ENOMEM;
		size = f->f_inode.di_size;

		if (size < CALL(info->parts.p_super, read)->fs_maxsymlinklen)
			memcpy(data, (char *) f->f_inode.di_db, size);
		else
			assert(0); // TOOD: read(link, size)
	}
	else
		return -EINVAL;

	return size;
}

static int ufs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, ino);
	int r;
	const ufs_fdesc_t * f = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);

	r = ufs_get_metadata(object, f, id, size, data);

	if (f)
		ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	const ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	return ufs_get_metadata(object, f, id, size, data);
}

static int ufs_set_metadata2(LFS_t * object, ufs_fdesc_t * f, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) object;

	assert(head && f && (!nfsm || fsm));

 retry:
	if (nfsm == 0)
		return ufs_write_inode(info, f->f_num, f->f_inode, head);
	
	if (fsm->fsm_feature == FSTITCH_FEATURE_SIZE) {
		if (fsm->fsm_value.u >= UFS_MAXFILESIZE)
			return -EINVAL;

		f->f_inode.di_size = fsm->fsm_value.u;
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_FILETYPE) {
		uint8_t fs_type;

		fs_type = fstitch_to_ufs_type(fsm->fsm_value.u);
		if (fs_type == (uint8_t) -EINVAL
		    || fs_type != (f->f_inode.di_mode >> 12))
			return -EINVAL;
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_UID) {
		f->f_inode.di_uid = fsm->fsm_value.u;
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_GID) {
		f->f_inode.di_gid = fsm->fsm_value.u;
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_UNIX_PERM) {
		f->f_inode.di_mode = (f->f_inode.di_mode & ~UFS_IPERM)
			| (fsm->fsm_value.u & UFS_IPERM);
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_MTIME) {
		// XXX!!!!
		//f->f_inode.di_mtime = f->f_inode.di_mtime;
		f->f_inode.di_mtime = fsm->fsm_value.u;
	}
	else if (fsm->fsm_feature == FSTITCH_FEATURE_SYMLINK) {
		if (!f || f->f_type != TYPE_SYMLINK)
			return -EINVAL;

		f->f_inode.di_size = fsm->fsm_value.p.length;
		if (fsm->fsm_value.p.length < CALL(info->parts.p_super, read)->fs_maxsymlinklen)
			memcpy((char *) f->f_inode.di_db, fsm->fsm_value.p.data, fsm->fsm_value.p.length);
		else
			assert(0); // TODO: write(link, size)
	} else
		return -EINVAL;

	fsm++;
	nfsm--;
	goto retry;
}

static int ufs_set_metadata2_inode(LFS_t * object, inode_t ino, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head)
{
	int r;
	ufs_fdesc_t * f = (ufs_fdesc_t *) ufs_lookup_inode(object, ino);
	if (!f)
		return -EINVAL;
	r = ufs_set_metadata2(object, f, fsm, nfsm, head);
	ufs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ufs_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t *fsm, size_t nfsm, patch_t ** head)
{
	ufs_fdesc_t * f = (ufs_fdesc_t *) file;
	return ufs_set_metadata2(object, f, fsm, nfsm, head);
}

static int ufs_get_root(LFS_t * lfs, inode_t * ino)
{
	*ino = UFS_ROOT_INODE;
	return UFS_ROOT_INODE;
}

static void ufs_destroy_parts(LFS_t * lfs)
{
	struct ufs_info * info = (struct ufs_info *) lfs;
	if (info->parts.p_allocator)
		DESTROY(info->parts.p_allocator);
	if (info->parts.p_dirent)
		DESTROY(info->parts.p_dirent);
	if (info->parts.p_cg)
		DESTROY(info->parts.p_cg);
	if (info->parts.p_super)
		DESTROY(info->parts.p_super);
}

static int ufs_destroy(LFS_t * lfs)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info = (struct ufs_info *) lfs;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	int32_t super_fs_ncg = super->fs_ncg;
	(void) super_fs_ncg;
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);

	ufs_destroy_parts(lfs);
	bdesc_release(&info->csum_block);
	sfree(info->csums, sizeof(struct UFS_csum) * super_fs_ncg);
	hash_map_destroy(info->filemap);

	memset(info, 0, sizeof(*info));
	free(info);

	return 0;
}

LFS_t * ufs_lfs(BD_t * block_device)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	struct ufs_info * info;
	LFS_t * lfs;

	if (DIRENT_MAXNAMELEN < UFS_MAXNAMELEN) {
		printf("struct dirent is too small!\n");
		return NULL;
	}

	if (!block_device)
		return NULL;

	printf(frsum_warning);

	info = malloc(sizeof(*info));
	if (!info)
		return NULL;

	lfs = &info->lfs;
	LFS_INIT(lfs, ufs);
	OBJMAGIC(lfs) = UFS_MAGIC;
	lfs->blockdev = block_device;

	info->ubd = block_device;
	info->write_head = CALL(block_device, get_write_head);
	info->parts.base = lfs;
	info->parts.p_super = ufs_super_wb(info); // Initialize first
	info->parts.p_allocator = ufs_alloc_lastpos(info);
	info->parts.p_dirent = ufs_dirent_linear(info);
	info->parts.p_cg = ufs_cg_wb(info);
	assert(info->parts.p_allocator);
	assert(info->parts.p_dirent);
	assert(info->parts.p_cg);
	assert(info->parts.p_super);
	if (!info->parts.p_allocator
			|| !info->parts.p_dirent
			|| !info->parts.p_cg
			|| !info->parts.p_super)
		ufs_destroy_parts(lfs);

	info->filemap = hash_map_create();
	if (!info->filemap) {
		ufs_destroy_parts(lfs);
		free(info);
		return NULL;
	}

	if (check_super(lfs)) {
		ufs_destroy_parts(lfs);
		free(info);
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
