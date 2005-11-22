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

#define block_is_free read_bitmap

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	struct UFS_Super * super;
	// commonly used values
	uint16_t ipf; // inodes per fragment
};

struct ufs_fdesc {
	uint32_t dir_inode;
	uint32_t dir_offset;
	char fullpath[UFS_MAXPATHLEN];
	UFS_File_t * file;
};

static bdesc_t * ufs_lookup_block(LFS_t * object, uint32_t number);
static int ufs_free_block(LFS_t * object, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t ufs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int ufs_set_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);

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

	if (num >= info->super->fs_ipg * info->super->fs_ncg)
		return -E_INVAL;
	if (!inode)
		return -E_INVAL;

	cg = num / info->super->fs_ipg;
	cg_off = num - cg * info->super->fs_ipg;
	fragno = cg_off / info->ipf;
	frag_off = cg_off - fragno * info->ipf;
	fragno += calc_cylgrp_start(object, cg) + info->super->fs_iblkno;

	inode_table = CALL(info->ubd, read_block, fragno);
	if (!inode_table)
		return -E_UNSPECIFIED;
	wanted = (struct UFS_dinode *) (inode_table->ddesc->data);
	wanted += frag_off;
	memcpy(inode, wanted, sizeof(struct UFS_dinode));

	// Not sure what chflags do, so raise a warning if any are set
	if (inode->di_flags)
		printf("Warning, inode %d has chflags set: %d\n", num);

	return 0;
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
			calc_cylgrp_start(object, num) + info->super->fs_cblkno);
	if (!b)
		return -E_UNSPECIFIED;

	memcpy(cg, b->ddesc->data, sizeof(struct UFS_cg));

	return 0;
}

// TODO do more checks, move printf statements elsewhere, mark fs as unclean
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t numblocks;

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

	info->ipf = info->super->fs_inopb / info->super->fs_frag;
	numblocks = CALL(info->ubd, get_numblocks);

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

// TODO
// Return 1 if block is free
static int read_bitmap(LFS_t * object, uint32_t blockno)
{
	//struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	return 0;
}

// TODO
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s %d\n", __FUNCTION__, blockno);
	//struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	return 0;
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

static int dir_lookup(void * object, struct UFS_File dir, const char * name, struct ufs_fdesc * new_fdesc)
{
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
			return read_inode(object, entry.d_fileno, &new_fdesc->file->f_inode);
		}
	}

	return 0;
}

static int walk_path(void * object, const char * path, struct ufs_fdesc * new_fdesc)
{
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

// TODO
// purpose parameter is ignored
static uint32_t ufs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	count_free_space(object);
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
	struct ufs_fdesc * f = (struct ufs_fdesc *) fdesc;;

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

	n = f->file->f_inode.di_size / info->super->fs_frag;
	if (f->file->f_inode.di_size % info->super->fs_frag)
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

	if (offset % info->super->fs_fsize || offset > f->file->f_inode.di_size)
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
			return -E_UNSPECIFIED;

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
			return -E_UNSPECIFIED;

		block_off[0] = (*((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1])) + fragno;

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_UNSPECIFIED;
		return (*((uint32_t *) (indirect[0]->ddesc->data) + pt_off[0])) + fragno;
	}
	else if (blockno < UFS_NDADDR + nindirb * nindirb * nindirb) {
		block_off[2] = blockno - UFS_NDADDR - nindirb * nindirb;
		frag_off[2] = block_off[2] / nindirf / nindirb / nindirb;
		pt_off[2] = (block_off[2] / nindirb / nindirb) % nindirf;

		frag_off[1] = block_off[2] / nindirf / nindirb;
		pt_off[1] = (block_off[2] / nindirb) % nindirf;

		frag_off[0] = (block_off[2] % nindirb) / nindirf;
		pt_off[0] = block_off[2] % nindirf;

		indirect[2] = CALL(info->ubd, read_block,
				f->file->f_inode.di_ib[2] + frag_off[2]);
		if (!indirect[1])
			return -E_UNSPECIFIED;

		block_off[1] = (*((uint32_t *) (indirect[2]->ddesc->data) + pt_off[2])) + fragno;

		indirect[1] = CALL(info->ubd, read_block,
				block_off[1] + frag_off[1]);
		if (!indirect[1])
			return -E_UNSPECIFIED;

		block_off[0] = (*((uint32_t *) (indirect[1]->ddesc->data) + pt_off[1])) + fragno;

		indirect[0] = CALL(info->ubd, read_block, block_off[0] + frag_off[0]);
		if (!indirect[0])
			return -E_UNSPECIFIED;
		return (*((uint32_t *) (indirect[0]->ddesc->data) + pt_off[0])) + fragno;
	}

	return -E_UNSPECIFIED;
}

static int ufs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("UFSDEBUG: %s %x, %d\n", __FUNCTION__, basep, *basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ufs_fdesc * f = (struct ufs_fdesc *) file;
	struct UFS_direct * dirfile;
	struct UFS_dinode inode;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, offset, actual_len;
	int r;

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

	actual_len = sizeof(struct dirent) + dirfile->d_namlen - DIRENT_MAXNAMELEN;
	if (size < actual_len)
		return -E_INVAL;

	r = read_inode(object, dirfile->d_ino, &inode); 
	if (r < 0)
		return r;

	if (inode.di_size > UFS_MAXFILESIZE) {
		printf("%s: file too big?\n", __FUNCTION__);
		inode.di_size &= UFS_MAXFILESIZE;
	}
	entry->d_filesize = inode.di_size;

	switch(dirfile->d_type)
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
	entry->d_fileno = dirfile->d_ino;
	entry->d_reclen = actual_len;
	entry->d_namelen = dirfile->d_namlen;
	strncpy(entry->d_name, dirfile->d_name, dirfile->d_namlen);
	entry->d_name[dirfile->d_namlen] = 0;

	*basep += dirfile->d_reclen;
	return 0;
}

// TODO
static int ufs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
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

// TODO
static uint32_t ufs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
}

// TODO
static int ufs_free_block(LFS_t * object, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("UFSDEBUG: %s\n", __FUNCTION__);
	return 0;
}

// TODO
static int ufs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
}

// TODO
static int ufs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
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

// TODO
static int ufs_set_metadata(LFS_t * object, const struct ufs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return 0;
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

// TODO
static int ufs_sync(LFS_t * object, const char * name)
{
	return 0;
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
