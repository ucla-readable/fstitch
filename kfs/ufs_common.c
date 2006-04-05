#include <inc/error.h>
#include <lib/assert.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>

#include <kfs/ufs_common.h>

// Assuming fixed number of inodes per cylinder group, so we don't have
// to read the cylinder group descriptor and confirm this every time.
// The last cylinder group may have less inodes?
int read_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode * inode)
{
	int cg, cg_off, fragno, frag_off;
	struct UFS_dinode * wanted;
	bdesc_t * inode_table;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (num < UFS_ROOT_INODE)
		printf("Warning, trying to read inode %d\n", num);

	if (!inode || num >= super->fs_ipg * super->fs_ncg)
		return -E_INVAL;

	cg = num / super->fs_ipg; // Cylinder group #
	cg_off = num % super->fs_ipg; // nth inode in cg
	fragno = cg_off / info->ipf; // inode is in nth fragment
	frag_off = cg_off % info->ipf; // inode is nth inode in fragment
	fragno += CALL(info->parts.p_cg, get_cylstart, cg) + super->fs_iblkno; // real fragno

	inode_table = CALL(info->ubd, read_block, fragno, 1);
	if (!inode_table)
		return -E_NOT_FOUND;
	wanted = (struct UFS_dinode *) (inode_table->ddesc->data);
	wanted += frag_off;
	memcpy(inode, wanted, sizeof(struct UFS_dinode));

	// Not sure what chflags do, so raise a warning if any are set
	if (inode->di_flags)
		printf("Warning, inode %d has chflags set: %d\n", num, inode->di_flags);

	return 0;
}

int write_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode inode, chdesc_t ** head)
{
	int cg, cg_off, fragno, frag_off, r, offset;
	bdesc_t * inode_table;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head)
		return -E_INVAL;

	if (num < 2)
		printf("Warning, trying to write inode %d\n", num);

	if (num >= super->fs_ipg * super->fs_ncg)
		return -E_INVAL;

	cg = num / super->fs_ipg; // Cylinder group #
	cg_off = num % super->fs_ipg; // nth inode in cg
	fragno = cg_off / info->ipf; // inode is in nth fragment
	frag_off = cg_off % info->ipf; // inode is nth inode in fragment
	fragno += CALL(info->parts.p_cg, get_cylstart, cg) + super->fs_iblkno; // real fragno

	inode_table = CALL(info->ubd, read_block, fragno, 1);
	if (!inode_table)
		return -E_NOT_FOUND;
	offset = sizeof(struct UFS_dinode) * frag_off;
	r = chdesc_create_byte(inode_table, info->ubd, offset, sizeof(struct UFS_dinode), &inode, head);
	if (r < 0)
		return r;

	return CALL(info->ubd, write_block, inode_table);
}

uint32_t read_btot(struct lfs_info * info, uint32_t num)
{
	uint32_t blockno, offset;
	uint32_t * ptr;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	cg = CALL(info->parts.p_cg, read, num / super->fs_fpg);
	if (!cg)
		return INVALID_BLOCK;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return INVALID_BLOCK;

	offset = cg->cg_btotoff + offset / 256;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return INVALID_BLOCK;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % super->fs_fsize) / 4;

	return *ptr;
}

uint16_t read_fbp(struct lfs_info * info, uint32_t num)
{
	uint32_t blockno, offset;
	uint32_t * ptr;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	cg = CALL(info->parts.p_cg, read, num / super->fs_fpg);
	if (!cg)
		return UFS_INVALID16;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return UFS_INVALID16;

	offset = cg->cg_boff + offset / 512;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return UFS_INVALID16;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if ((num / 1024) % 2)
		return ((*ptr >> 16) & 0xFFFF);
	return (*ptr & 0xFFFF);
}

int read_inode_bitmap(struct lfs_info * info, uint32_t num)
{
	uint32_t blockno, offset;
	uint32_t * ptr;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	cg = CALL(info->parts.p_cg, read, num / super->fs_ipg);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % super->fs_ipg;
	if (offset >= cg->cg_niblk)
		return -E_INVAL;

	offset = cg->cg_iusedoff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_ipg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_USED;
	return UFS_FREE;
}

int read_fragment_bitmap(struct lfs_info * info, uint32_t num)
{
	uint32_t blockno, offset;
	uint32_t * ptr;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	cg = CALL(info->parts.p_cg, read, num / super->fs_fpg);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return -E_INVAL;

	offset = cg->cg_freeoff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_FREE;
	return UFS_USED;
}

int read_block_bitmap(struct lfs_info * info, uint32_t num)
{
	uint32_t blockno, offset, blocknum;
	uint32_t * ptr;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	blocknum = num * super->fs_frag;
	cg = CALL(info->parts.p_cg, read, blocknum / super->fs_fpg);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % (super->fs_fpg / super->fs_frag);
	if (offset >= cg->cg_nclusterblks)
		return -E_INVAL;

	offset = cg->cg_clusteroff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, blocknum / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if (*ptr & (1 << (num % 32)))
		return UFS_FREE;
	return UFS_USED;
}

int write_btot(struct lfs_info * info, uint32_t num, uint32_t value, chdesc_t ** head)
{
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || value > 128)
		return -E_INVAL;

	cg = CALL(info->parts.p_cg, read, num / super->fs_fpg);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return -E_INVAL;

	offset = cg->cg_btotoff + offset / 256;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, ROUNDDOWN32(offset, 4), 4, &value, head);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	return 0;
}

int write_fbp(struct lfs_info * info, uint32_t num, uint16_t value, chdesc_t ** head)
{
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || value > 128)
		return -E_INVAL;

	cg = CALL(info->parts.p_cg, read, num / super->fs_fpg);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return -E_INVAL;

	offset = cg->cg_boff + offset / 512;
	blockno = CALL(info->parts.p_cg, get_cylstart, num / super->fs_fpg)
		+ super->fs_cblkno + offset / super->fs_fsize;

	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, ROUNDDOWN32(offset,2), 2, &value, head);
	if (r >= 0)
		r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	return 0;
}

int write_inode_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head)
{
	uint32_t blockno, offset, * ptr;
	int r, cyl;
	bdesc_t * block;
	chdesc_t * ch;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head)
		return -E_INVAL;

	cyl = num / super->fs_ipg;
	cg = CALL(info->parts.p_cg, read, cyl);
	if (!cg)
		return -E_UNSPECIFIED;

	if (value == UFS_USED)
		value = 1;
	else
		value = 0;

	offset = num % super->fs_ipg;
	if (offset >= cg->cg_niblk)
		return -E_INVAL;

	offset = cg->cg_iusedoff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, cyl)
		+ super->fs_cblkno + offset / super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("%s already at the right value! (%d: %d)\n", __FUNCTION__, num, value);
		return 1;
	}

	ch = chdesc_create_bit(block, info->ubd, (offset % super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*head = ch;

	if (value)
		r = update_summary(info, cyl, 0, 0, -1, 0, head);
	else
		r = update_summary(info, cyl, 0, 0, 1, 0, head);

	return r;
}

#warning frsum code needs to be correctly reimplemented
int write_fragment_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head)
{
	const struct UFS_cg * cg;
	uint32_t blockno, offset, * ptr;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	int r, nf, cyl = num / super->fs_fpg;
	int nfrags_before, nfrags_after, unused_before = 0, unused_after = 0;
	bdesc_t * block; // , * cgblock;
	chdesc_t * ch;

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

	if (!head)
		return -E_INVAL;

	cg = CALL(info->parts.p_cg, read, cyl);
	if (!cg)
		return -E_UNSPECIFIED;

	if (value == UFS_USED)
		value = 0;
	else
		value = 1;

	offset = num % super->fs_fpg;
	if (offset >= cg->cg_ndblk)
		return -E_INVAL;

	offset = cg->cg_freeoff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, cyl)
		+ super->fs_cblkno + offset / super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data) + (offset % super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("already at the right value!\n");
		return 1;
	}

	nfrags_before = BitsSetTable256[(*ptr >> ROUNDDOWN32(num % 32, 8)) & 0xFF];
	if (nfrags_before == 8)
		unused_before = 1;

	/*
	blockno = CALL(info->parts.p_cg, get_cylstart, cyl) + super->fs_cblkno;
	cgblock = CALL(info->ubd, read_block, blockno, 1);
	if (!cgblock)
		return -E_NOT_FOUND;
		*/

	ch = chdesc_create_bit(block, info->ubd,
			(offset % super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*head = ch;

	nfrags_after = BitsSetTable256[(*ptr >> ROUNDDOWN32(num % 32, 8)) & 0xFF];
	if (nfrags_after == 8)
		unused_after = 1;

	if (value) { // Marked fragment as free
		if (unused_after) { // Mark the whole block as free
			r = write_block_bitmap(info, num / super->fs_frag, UFS_FREE, head);
			if (r < 0)
				return r;
			nf = 1 - super->fs_frag;
		}
		else
			nf = 1;
	}
	else { // Marked fragment as used
		if (unused_before) { // Mark the whole block as used
			r = write_block_bitmap(info, num / super->fs_frag, UFS_USED, head);
			if (r < 0)
				return r;
			nf = super->fs_frag - 1;
		}
		else
			nf = -1;
	}

	/* TODO
	if (nfrags_before > 0 && nfrags_before < 8) {
		offset = (uint32_t) &((struct UFS_cg *) NULL)->cg_frsum[nfrags_before];
		cg->cg_frsum[nfrags_before]--;
		r = chdesc_create_byte(cgblock, info->ubd, (uint16_t) offset,
				sizeof(cg->cg_frsum[nfrags_before]),
				&cg->cg_frsum[nfrags_before], head);
		if (r < 0)
			return r;

	}
	if (nfrags_after > 0 && nfrags_after < 8) {
		offset = (uint32_t) &((struct UFS_cg *) NULL)->cg_frsum[nfrags_after];
		cg->cg_frsum[nfrags_after]++;
		r = chdesc_create_byte(cgblock, info->ubd, (uint16_t) offset,
				sizeof(cg->cg_frsum[nfrags_after]),
				&cg->cg_frsum[nfrags_after], head);
		if (r < 0)
			return r;
	}

	r = CALL(info->ubd, write_block, cgblock);
	if (r < 0)
		return r;
	*/

	return update_summary(info, cyl, 0, 0, 0, nf, head);
}

// This is the 'raw' function to write the block bitmap
// You probably want allocate_wholeblock()
int write_block_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head)
{
	uint32_t blocknum, blockno, offset, * ptr, btot;
	uint16_t fbp;
	int r, cyl;
	bdesc_t * block;
	chdesc_t * ch;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head)
		return -E_INVAL;

	if (value == UFS_USED)
		value = 0;
	else
		value = 1;

	blocknum = num * super->fs_frag;
	cyl = blocknum / super->fs_fpg;
	cg = CALL(info->parts.p_cg, read, cyl);
	if (!cg)
		return -E_UNSPECIFIED;

	offset = num % (super->fs_fpg / super->fs_frag);
	if (offset >= cg->cg_nclusterblks)
		return -E_INVAL;

	offset = cg->cg_clusteroff + offset / 8;
	blockno = CALL(info->parts.p_cg, get_cylstart, cyl)
		+ super->fs_cblkno + offset / super->fs_fsize;
	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -E_NOT_FOUND;

	ptr = ((uint32_t *) block->ddesc->data)
		+ (offset % super->fs_fsize) / 4;

	if (((*ptr >> (num % 32)) & 1) == value) {
		printf("%s already at the right value! (%d: %d)\n", __FUNCTION__, num, value);
		return 1;
	}

	ch = chdesc_create_bit(block, info->ubd, (offset % super->fs_fsize) / 4, 1 << (num % 32));
	if (!ch)
		return -1;

	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*head = ch;

	if (value) {
		btot = read_btot(info, blocknum) + 1;
		fbp = read_fbp(info, blocknum) + 1;
	}
	else {
		btot = read_btot(info, blocknum) - 1;
		fbp = read_fbp(info, blocknum) - 1;
	}
	r = write_btot(info, blocknum, btot, head);
	if (r < 0)
		return r;
	r = write_fbp(info, blocknum, fbp, head);
	if (r < 0)
		return r;


	if (value)
		r = update_summary(info, cyl, 0, 1, 0, 0, head);
	else
		r = update_summary(info, cyl, 0, -1, 0, 0, head);

	return r;
}

// [ndir, ..., nffree] parameters are deltas
int update_summary(struct lfs_info * info, int cyl, int ndir, int nbfree, int nifree, int nffree, chdesc_t ** head)
{
	struct UFS_csum sum;
	struct UFS_csum * csum;
	int r;
	const struct UFS_cg * cg;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || cyl < 0 || cyl >= super->fs_ncg)
		return -E_INVAL;

	cg = CALL(info->parts.p_cg, read, cyl);
	if (!cg)
		return -E_UNSPECIFIED;

	// Update cylinder group
	sum.cs_ndir = cg->cg_cs.cs_ndir + ndir;
	sum.cs_nbfree = cg->cg_cs.cs_nbfree + nbfree;
	sum.cs_nifree = cg->cg_cs.cs_nifree + nifree;
	sum.cs_nffree = cg->cg_cs.cs_nffree + nffree;
	r = CALL(info->parts.p_cg, write_cs, cyl, &sum, head);
	if (r < 0)
		return r;

	// Update cylinder summary area
	csum = info->csums + cyl;
	csum->cs_ndir += ndir;
	csum->cs_nbfree += nbfree;
	csum->cs_nifree += nifree;
	csum->cs_nffree += nffree;

	r = chdesc_create_byte(info->csum_block, info->ubd,
			cyl * sizeof(struct UFS_csum), sizeof(struct UFS_csum),
			csum, head);
	if (r < 0)
		return r;

	r = CALL(info->ubd, write_block, info->csum_block);
	if (r < 0)
		return r;

	// Update superblock
	sum.cs_ndir = super->fs_cstotal.cs_ndir + ndir;
	sum.cs_nbfree = super->fs_cstotal.cs_nbfree + nbfree;
	sum.cs_nifree = super->fs_cstotal.cs_nifree + nifree;
	sum.cs_nffree = super->fs_cstotal.cs_nffree + nffree;
	return CALL(info->parts.p_super, write_cstotal, &sum, head);
}

int check_name(const char * p)
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

uint8_t kfs_to_ufs_type(uint8_t type)
{
	switch(type)
	{
		case TYPE_FILE:
			return UFS_DT_REG;
		case TYPE_DIR:
			return UFS_DT_DIR;
		case TYPE_SYMLINK:
			return UFS_DT_LNK;
			// case TYPE_DEVICE: ambiguous
		default:
			return -E_INVAL;
	}
	
}

uint8_t ufs_to_kfs_type(uint8_t type)
{
	switch(type)
	{
		case UFS_DT_REG:
			return TYPE_FILE;
		case UFS_DT_DIR:
			return TYPE_DIR;
			/*
			   case UFS_DT_LNK:
			   return TYPE_SYMLINK;
			   case UFS_DT_CHR:
			   case UFS_DT_BLK:
			   // TYPE_DEVICE is unreliable and is treated like a file...
			   return TYPE_DEVICE;
			   */
		default:
			kdprintf(STDERR_FILENO, "%s(): file type %u is currently unsupported\n", __FUNCTION__, type);
			return TYPE_INVAL;
	}
}
