#include <kfs/ufs_common.h>

// Assuming fixed number of inodes per cylinder group, so we don't have
// to read the cylinder group descriptor and confirm this every time.
// The last cylinder group may have less inodes?
int read_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode * inode)
{
	int cg, cg_off, fragno, frag_off;
	struct UFS_dinode * wanted;
	bdesc_t * inode_table;

	if (num < UFS_ROOT_INODE)
		printf("Warning, trying to read inode %d\n", num);

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
		printf("Warning, inode %d has chflags set: %d\n", num, inode->di_flags);

	return 0;
}

int write_inode(struct lfs_info * info, uint32_t num, struct UFS_dinode inode, chdesc_t ** head, chdesc_t ** tail)
{
	int cg, cg_off, fragno, frag_off, r, offset;
	bdesc_t * inode_table;

	if (!head || !tail)
		return -E_INVAL;

	if (num < 2)
		printf("Warning, trying to write inode %d\n", num);

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

int read_cg(struct lfs_info * info, uint32_t num, struct UFS_cg * cg)
{
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

uint32_t read_btot(struct lfs_info * info, uint32_t num)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(info, num / info->super->fs_fpg, &cg);
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

uint16_t read_fbp(struct lfs_info * info, uint32_t num)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(info, num / info->super->fs_fpg, &cg);
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

int read_inode_bitmap(struct lfs_info * info, uint32_t num)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(info, num / info->super->fs_ipg, &cg);
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

int read_fragment_bitmap(struct lfs_info * info, uint32_t num)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	r = read_cg(info, num / info->super->fs_fpg, &cg);
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

int read_block_bitmap(struct lfs_info * info, uint32_t num)
{
	struct UFS_cg cg;
	uint32_t blockno, offset, blocknum;
	uint32_t * ptr;
	int r;
	bdesc_t * block;

	blocknum = num * info->super->fs_frag;
	r = read_cg(info, blocknum / info->super->fs_fpg, &cg);
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

int write_btot(struct lfs_info * info, uint32_t num, uint32_t value, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;

	if (!head || !tail || value > 128)
		return -E_INVAL;

	r = read_cg(info, num / info->super->fs_fpg, &cg);
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

int write_fbp(struct lfs_info * info, uint32_t num, uint16_t value, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_cg cg;
	uint32_t blockno, offset;
	int r;
	bdesc_t * block;

	if (!head || !tail || value > 128)
		return -E_INVAL;

	r = read_cg(info, num / info->super->fs_fpg, &cg);
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

int write_inode_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
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

	r = read_cg(info, cyl, &cg);
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
		r = update_summary(info, cyl, 0, 0, -1, 0, head, &newtail);
	else
		r = update_summary(info, cyl, 0, 0, 1, 0, head, &newtail);

	return r;
}

int write_fragment_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
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

	r = read_cg(info, cyl, &cg);
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
			r = write_block_bitmap(info, num / info->super->fs_frag,
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
			r = write_block_bitmap(info, num / info->super->fs_frag,
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

	return update_summary(info, cyl, 0, 0, 0, nf, head, &newtail);
}

// This is the 'raw' function to write the block bitmap
// You probably want allocate_wholeblock()
int write_block_bitmap(struct lfs_info * info, uint32_t num, bool value, chdesc_t ** head, chdesc_t ** tail)
{
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
	r = read_cg(info, cyl, &cg);
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
		btot = read_btot(info, blocknum) + 1;
		fbp = read_fbp(info, blocknum) + 1;
	}
	else {
		btot = read_btot(info, blocknum) - 1;
		fbp = read_fbp(info, blocknum) - 1;
	}
	r = write_btot(info, blocknum, btot, head, &newtail);
	if (r < 0)
		return r;
	r = write_fbp(info, blocknum, fbp, head, &newtail);
	if (r < 0)
		return r;


	if (value)
		r = update_summary(info, cyl, 0, 1, 0, 0, head, &newtail);
	else
		r = update_summary(info, cyl, 0, -1, 0, 0, head, &newtail);

	return r;
}

// [ndir, ..., nffree] parameters are deltas
int update_summary(struct lfs_info * info, int cyl, int ndir, int nbfree, int nifree, int nffree, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_cg cg;
	struct UFS_csum * csum;
	uint32_t blockno;
	int r;
	bdesc_t * cgblock;
	chdesc_t * newtail;

	if (!head || !tail || cyl < 0 || cyl >= info->super->fs_ncg)
		return -E_INVAL;

	r = read_cg(info, cyl, &cg);
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
	csum = info->csums + cyl;
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
			   return TYPE_DEVICE;
			   */
		default:
			kdprintf(STDERR_FILENO, "%s(): file type %u is currently unsupported\n", __FUNCTION__, type);
			return TYPE_INVAL;
	}
}
