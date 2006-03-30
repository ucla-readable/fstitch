/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <inc/error.h>
#include <lib/assert.h>
#include <lib/hash_set.h>
#include <lib/jiffies.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/josfs_base.h>

#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in josfs_base.c
#endif

#define JOSFS_BASE_DEBUG 0
#define JOSFS_BASE_DEBUG_FSCK 0

#if JOSFS_BASE_DEBUG_FSCK
#define DFprintf(x...) printf(x)
#else
#define DFprintf(x...)
#endif

#if JOSFS_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define INODE_ROOT ((inode_t) 1)

#define block_is_free read_bitmap
#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	bdesc_t * bitmap_cache; // Bitmap mini write through cache!
};

struct josfs_fdesc {
	/* extend struct fdesc */
	fdesc_common_t * common;
	fdesc_common_t base;

	uint32_t dirb; // Block number on the block device of a block in one of
	               // the containing directory's data blocks. It is the block
	               // which contains the on-disk File structure for this file.
	uint32_t index; // the byte index in that block of the JOSFS_File_t for this file
	inode_t ino;
	JOSFS_File_t * file;
};

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number);
static int josfs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head);
static int get_dirent_name(LFS_t * object, JOSFS_File_t * file, const char ** name, uint32_t * basep);
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t get_file_block(LFS_t * object, JOSFS_File_t * file, uint32_t offset);
static uint32_t josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head);
static int josfs_set_metadata(LFS_t * object, struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head);

// Equivalent to JOS's read_super
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t numblocks;

	/* make sure we have the block size we expect */
	if (CALL(info->ubd, get_blocksize) != JOSFS_BLKSIZE) {
		printf("Block device size is not JOSFS_BLKSIZE!\n");
		return -1;
	}

	/* the superblock is in block 1 */
	info->super_block = CALL(info->ubd, read_block, 1, 1);
	if (!info->super_block)
	{
		printf("Unable to read superblock!\n");
		return -1;
	}
	if (super->s_magic != JOSFS_FS_MAGIC) {
		printf("josfs_base: bad file system magic number\n");
		return -1;
	}

	numblocks = CALL(info->ubd, get_numblocks);

	printf("JOS Filesystem size: %u blocks (%uMB)\n", (int) super->s_nblocks, (int) (super->s_nblocks / (1024 * 1024 / JOSFS_BLKSIZE)));
	if (super->s_nblocks > numblocks) {
		printf("josfs_base: file system is too large\n");
		return -1;
	}

	bdesc_retain(info->super_block);
	return 0;
}

// Equivalent to JOS's read_bitmap
static int check_bitmap(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int i, blocks_to_read;

	blocks_to_read = super->s_nblocks / JOSFS_BLKBITSIZE;

	if (super->s_nblocks % JOSFS_BLKBITSIZE)
		blocks_to_read++;

	// Make sure the reserved and root blocks are marked in-use.
	if (block_is_free(object, 0) || block_is_free(object, 1)) {
		printf("josfs_base: Boot Sector or Parition Table marked free!\n");
		return -1;
	}

	// Make sure that the bitmap blocks are marked in-use.
	for (i = 0; i < blocks_to_read; i++) {
		if (block_is_free(object, 2+i)) {
			printf("josfs_base: Free Block Bitmap block %d marked free!\n", 2+i);
			return -1;
		}
	}

	return 0;
}

// Return 1 if block is free
static int read_bitmap(LFS_t * object, uint32_t blockno)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	uint32_t * ptr;

	if (blockno >= super->s_nblocks) {
		printf("josfs_base: requested status of block %u past end of file system!\n", blockno);
		return -1;
	}

	target = 2 + (blockno / (JOSFS_BLKBITSIZE));

	if (info->bitmap_cache && info->bitmap_cache->number != target)
		bdesc_release(&info->bitmap_cache);

	if (! info->bitmap_cache) {
		bdesc = CALL(info->ubd, read_block, target, 1);
		if (!bdesc || bdesc->ddesc->length != JOSFS_BLKSIZE) {
			printf("josfs_base: trouble reading bitmap! (blockno = %u)\n", blockno);
			return -1;
		}
		bdesc_retain(bdesc);
		info->bitmap_cache = bdesc;
	}

	ptr = ((uint32_t *) info->bitmap_cache->ddesc->data) + ((blockno % JOSFS_BLKBITSIZE) / 32);
	if (*ptr & (1 << (blockno % 32)))
		return 1;
	return 0;
}

static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: write_bitmap %u\n", blockno);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	chdesc_t * ch;
	int r;

	if (!head)
		return -1;

	if (blockno == 0) {
		printf("josfs_base: attempted to write status of zero block!\n");
		return -1;
	}
	else if (blockno >= super->s_nblocks) {
		printf("josfs_base: attempted to write status of block %u past end of file system!\n", blockno);
		return -1;
	}

	target = 2 + (blockno / JOSFS_BLKBITSIZE);

	if (info->bitmap_cache && info->bitmap_cache->number == target)
		bdesc = info->bitmap_cache;
	else {
		if(info->bitmap_cache)
			bdesc_release(&info->bitmap_cache);
		bdesc = CALL(info->ubd, read_block, target, 1);

		if (!bdesc || bdesc->ddesc->length != JOSFS_BLKSIZE) {
			printf("josfs_base: trouble reading bitmap! (blockno = %u)\n", blockno);
			return -1;
		}

		bdesc_retain(bdesc);
		info->bitmap_cache = bdesc;
	}

	/* does it already have the right value? */
	if (((uint32_t *) bdesc->ddesc->data)[(blockno % JOSFS_BLKBITSIZE) / 32] >> (blockno % 32) == value)
		return 0;
	/* bit chdescs take offset in increments of 32 bits */
	ch = chdesc_create_bit(bdesc, info->ubd, (blockno % JOSFS_BLKBITSIZE) / 32, 1 << (blockno % 32));
	if (!ch)
		return -1;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*head = ch;

	r = CALL(info->ubd, write_block, bdesc);

	return r;
}

static uint32_t count_free_space(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	const uint32_t s_nblocks = super->s_nblocks;
	uint32_t i, count = 0;

	for (i = 0; i < s_nblocks; i++)
		if (read_bitmap(object, i))
			count++;
	return count;
}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, JOSFS_File_t* dir, const char* name, JOSFS_File_t** file, uint32_t * dirb, int *index)
{
	Dprintf("JOSFSDEBUG: dir_lookup %s\n", name);
	uint32_t i, basep = 0;
	int r = 0;

	for (i = 0; r >= 0; i++)
	{
		const char * found_name = NULL;
		r = get_dirent_name(object, dir, &found_name, &basep);
		if (r == 0 && !strcmp(found_name, name)) {
			bdesc_t * dirblock = NULL;
			uint32_t blockno = i / JOSFS_BLKFILES;
			*dirb = get_file_block(object, dir, blockno * JOSFS_BLKSIZE);
			if (*dirb != INVALID_BLOCK)
				dirblock = josfs_lookup_block(object, *dirb);
			if (dirblock) {
				uint8_t * target = (uint8_t *) dirblock->ddesc->data;
				*index = (i % JOSFS_BLKFILES) * sizeof(JOSFS_File_t);
				target += *index;
				*file = malloc(sizeof(JOSFS_File_t));
				if (*file) {
					memcpy(*file, target, sizeof(JOSFS_File_t));
					Dprintf("JOSFSDEBUG: dir_lookup done: FOUND\n");
					return 0;
				}
				else {
					Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM2\n");
					return -E_NO_MEM;
				}
			}
		}
	}

	*file = NULL;
	Dprintf("JOSFSDEBUG: dir_lookup done: NOT FOUND\n");
	return -E_NOT_FOUND;
}

static int josfs_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -E_INVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int josfs_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -E_INVAL;
	
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int josfs_get_root(LFS_t * object, inode_t * ino)
{
	*ino = INODE_ROOT;
	return 0;
}

static uint32_t josfs_get_blocksize(LFS_t * object)
{
	return JOSFS_BLKSIZE;
}

static BD_t * josfs_get_blockdev(LFS_t * object)
{
	return ((struct lfs_info *) OBJLOCAL(object))->ubd;
}

// file and purpose parameter are ignored
static uint32_t josfs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t blockno, bitmap_size, s_nblocks;
	int r;

	if (!head)
		return INVALID_BLOCK;

	s_nblocks = super->s_nblocks;
	bitmap_size = (s_nblocks + JOSFS_BLKBITSIZE + 1) / JOSFS_BLKBITSIZE;

	for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
		if (block_is_free(object, blockno)) {
			r = write_bitmap(object, blockno, 0, head);
			if (r < 0)
				return INVALID_BLOCK;

			assert(!block_is_free(object, blockno));
			return blockno;
		}
	}

	return INVALID_BLOCK;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, read_block, number, 1);
}

static bdesc_t * josfs_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	Dprintf("JOSFSDEBUG: josfs_synthetic_lookup_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, synthetic_read_block, number, 1, synthetic);
}

static int josfs_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_cancel_synthetic_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, cancel_block, number);
}

static fdesc_t * josfs_lookup_inode(LFS_t * object, inode_t ino)
{
	struct josfs_fdesc * fd;
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t *dirblock;

	fd = (struct josfs_fdesc *)malloc(sizeof(struct josfs_fdesc));
	if (!fd)
		goto josfs_lookup_inode_exit;

	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	if (ino == INODE_ROOT)
	{
		fd->dirb = 1;
		fd->index = (uint32_t) &((struct JOSFS_Super *) NULL)->s_root;
	}
	else
	{
		fd->dirb = ino / JOSFS_BLKFILES;
		fd->index = (ino % JOSFS_BLKFILES) * sizeof(JOSFS_File_t);
	}
	fd->ino = ino;

	if (ino == INODE_ROOT) { // superblock
		fd->file = &super->s_root;
	} else {
		JOSFS_File_t *file = malloc(sizeof(JOSFS_File_t));
		if (!file)
		{
			goto josfs_lookup_inode_exit;
			return NULL;
		}

		dirblock = CALL(info->ubd, read_block, fd->dirb, 1);
		if (!dirblock)
			goto josfs_lookup_inode_exit2;
		
		memcpy(file, dirblock->ddesc->data + fd->index, sizeof(JOSFS_File_t));
		fd->file = file;
	}
	return (fdesc_t*)fd;

 josfs_lookup_inode_exit2:
	if (fd->file != &super->s_root)
		free(fd->file);
 josfs_lookup_inode_exit:
	free(fd);
	return NULL;
}

static void josfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("JOSFSDEBUG: josfs_free_fdesc %p\n", fdesc);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) fdesc;

	if (f) {
		if (f->file && f->file != &super->s_root)
			free(f->file);
		free(f);
	}
}

static int josfs_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	int index = 0;
	struct josfs_fdesc *fd;
	JOSFS_File_t * parent_file;
	JOSFS_File_t * file;
	uint32_t dirb = 0;
	int r;

	// "." and ".." are (at least right now) supported by code further up
	// (this seems hacky, but it would be hard to figure out parent's parent from here)

	fd = (struct josfs_fdesc *)josfs_lookup_inode(object, parent);
	if (!fd)
		return -E_INVAL;
	parent_file = fd->file;

	r = dir_lookup(object, parent_file, name, &file, &dirb, &index);
	josfs_free_fdesc(object, (fdesc_t *) fd);
	fd = NULL;
	parent_file = NULL;
	if (file != &super->s_root)
		free(file);
	file = NULL;
	if (r < 0)
		return r;
	*ino = dirb * JOSFS_BLKFILES + (index / sizeof(JOSFS_File_t));
	return 0;
}

static uint32_t get_file_numblocks(struct lfs_info * info, JOSFS_File_t * file)
{
	bdesc_t * indirect;
	uint32_t nblocks = 0;
	int i;

	for (i = 0; i < JOSFS_NDIRECT; i++) {
		if (!file->f_direct[i])
			break;
		nblocks++;
	}

	// file->f_indirect -> i == JOSFS_NDIRECT
	assert(!file->f_indirect || i == JOSFS_NDIRECT);

	if (file->f_indirect) {
		indirect = CALL(info->ubd, read_block, file->f_indirect, 1);
		if (indirect) {
			uint32_t * j = (uint32_t *) indirect->ddesc->data;
			for (i = JOSFS_NDIRECT; i < JOSFS_NINDIRECT; i++) {
				if (!j[i])
					break;
				nblocks++;
			}
		}
	}

	return nblocks;
}

static uint32_t josfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return get_file_numblocks(info, f->file);
}

static uint32_t get_file_block(LFS_t * object, JOSFS_File_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * indirect;
	uint32_t blockno, nblocks;

	nblocks = get_file_numblocks(info, file);
	if (offset % JOSFS_BLKSIZE || offset >= nblocks * JOSFS_BLKSIZE)
		return INVALID_BLOCK;

	if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
		indirect = CALL(info->ubd, read_block, file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;
		blockno = ((uint32_t *) indirect->ddesc->data)[offset / JOSFS_BLKSIZE];
	}
	else
		blockno = file->f_direct[offset / JOSFS_BLKSIZE];
	return blockno;
}

// Offset is a byte offset
static uint32_t josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	//Dprintf("JOSFSDEBUG: josfs_get_file_block_num %s, %u\n", file->f_name, offset);
	return get_file_block(object, f->file, offset);
}

static int fill_dirent(JOSFS_File_t * dirfile, inode_t ino, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	uint16_t namelen = MIN(strlen(dirfile->f_name), sizeof(entry->d_name) - 1);
	uint16_t reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;

	if (size < reclen)
		return -E_INVAL;

	// If the name length is 0 (or less?) then we assume it's an empty slot
	if (namelen < 1) {
		entry->d_reclen = 0;
		*basep += 1;
		return 1;
	}

	entry->d_fileno = ino;

	switch(dirfile->f_type)
	{
		case JOSFS_TYPE_FILE:
			entry->d_type = TYPE_FILE;
			break;
		case JOSFS_TYPE_DIR:
			entry->d_type = TYPE_DIR;
			break;
		default:
			entry->d_type = TYPE_INVAL;
	}
	entry->d_filesize = dirfile->f_size;
	entry->d_reclen = reclen;
	entry->d_namelen = namelen;
	strncpy(entry->d_name, dirfile->f_name, namelen + 1);
	entry->d_name[namelen] = 0;

	*basep += 1;
	return 0;
}

// Get a dirent's name and name only; useful if the caller does not
// know an open file's inode number
static int get_dirent_name(LFS_t * object, JOSFS_File_t * file, const char ** name, uint32_t * basep)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * dirblock = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;

	// Make sure it's a directory and we can read from it
	if (file->f_type != JOSFS_TYPE_DIR)
		return -E_NOT_DIR;

	blockno = *basep / JOSFS_BLKFILES;

	if (blockno >= get_file_numblocks(info, file))
		return -E_UNSPECIFIED;

	blockno = get_file_block(object, file, blockno * JOSFS_BLKSIZE);
	if (blockno != INVALID_BLOCK)
		dirblock = josfs_lookup_block(object, blockno);
	if (!dirblock)
		return -E_NOT_FOUND;
	dirfile = (JOSFS_File_t *) dirblock->ddesc->data + (*basep % JOSFS_BLKFILES);

	(*basep)++;
	if (strlen(dirfile->f_name) < 1)
	{
		*name = NULL;
		return 1;
	}
	else
	{
		*name = dirfile->f_name;
		return 0;
	}
}

static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("JOSFSDEBUG: josfs_get_dirent %p, %u\n", basep, *basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type != JOSFS_TYPE_DIR)
		return -E_NOT_DIR;

	if (*basep == 0)
	{
		JOSFS_File_t d = {
			.f_name = {0},
			.f_size = 0,
			.f_type = JOSFS_TYPE_DIR,
			.f_direct = {0},
			.f_indirect = 0
		};
		strncpy(d.f_name, ".", JOSFS_MAXNAMELEN);
		d.f_name[JOSFS_MAXNAMELEN - 1] = 0;
		return fill_dirent(&d, f->ino, entry, size, basep);
	}

	if (*basep == 1)
	{
		JOSFS_File_t d = {
			.f_name = {0},
			.f_size = 0,
			.f_type = JOSFS_TYPE_DIR,
			.f_direct = {0},
			.f_indirect = 0
		};
		inode_t parent;
		strncpy(d.f_name, "..", JOSFS_MAXNAMELEN);
		d.f_name[JOSFS_MAXNAMELEN - 1] = 0;
		assert(f->common->parent != INODE_NONE);
		if (f->ino != INODE_ROOT)
		{
			assert(f->common->parent != INODE_NONE);
			parent = f->common->parent;
		}
		else
			parent = f->ino;
		return fill_dirent(&d, parent, entry, size, basep);
	}

	blockno = (*basep - 2) / JOSFS_BLKFILES;

	if (blockno >= get_file_numblocks(info, f->file))
		return -E_UNSPECIFIED;

	blockno = get_file_block(object, f->file, blockno * JOSFS_BLKSIZE);
	if (blockno != INVALID_BLOCK)
		dirblock = josfs_lookup_block(object, blockno);
	if (!dirblock)
		return -E_NOT_FOUND;
	dirfile = &((JOSFS_File_t *) dirblock->ddesc->data)[(*basep - 2) % JOSFS_BLKFILES];

	return fill_dirent(dirfile, f->ino, entry, size, basep);
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_append_file_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(info, f->file);
	bdesc_t * indirect = NULL, * dirblock = NULL;
	int r, offset;

	if (!head || nblocks >= JOSFS_NINDIRECT || nblocks < 0)
		return -E_INVAL;

	if (nblocks > JOSFS_NDIRECT) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return -E_NO_DISK;

		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		return CALL(info->ubd, write_block, indirect);
	}
	else if (nblocks == JOSFS_NDIRECT) {
		uint32_t inumber = josfs_allocate_block(object, NULL, 0, head);
		chdesc_t * temp_head = *head;
		bdesc_t * indirect;
		if (inumber == INVALID_BLOCK)
			return -E_NO_DISK;
		indirect = josfs_lookup_block(object, inumber);

		// Initialize the new indirect block
		if ((r = chdesc_create_init(indirect, info->ubd, &temp_head)) < 0)
			return r;

		// Initialize the structure, then point to it
		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_NO_DISK;

		// this head is from josfs_allocate_block() above
		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &inumber, head)) < 0)
			return r;

		/* FIXME handle the return values better? */
		r = CALL(info->ubd, write_block, indirect);
		r |= CALL(info->ubd, write_block, dirblock);

		if (r >= 0)
			f->file->f_indirect = inumber;

		return r;
	}
	else {
		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_NO_DISK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_direct[nblocks] = block;
		return 0;
	}
}

static fdesc_t * josfs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, inode_t * newino, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	JOSFS_File_t *dir = NULL, *f = NULL;
	struct josfs_fdesc * dir_fdesc = NULL;
	JOSFS_File_t temp_file;
	struct josfs_fdesc * new_fdesc;
	bdesc_t * blk = NULL;
	fdesc_t * pdir_fdesc;
	uint16_t offset;
	uint32_t nblock, number;
	int32_t updated_size;
	chdesc_t * temp_head;
	int i, r;

	if (!head || link)
		return NULL;

	switch (type)
	{
		case TYPE_FILE:
			type = JOSFS_TYPE_FILE;
			break;
		case TYPE_DIR:
			type = JOSFS_TYPE_DIR;
			break;
		default:
			return NULL;
	}

	new_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!new_fdesc)
		return NULL;
	new_fdesc->common = &new_fdesc->base;
	new_fdesc->base.parent = INODE_NONE;

	pdir_fdesc = josfs_lookup_inode(object, parent);
	if (!pdir_fdesc)
		goto allocate_name_exit;

	// Modified dir_alloc_file() from JOS
	nblock = get_file_numblocks(info, ((struct josfs_fdesc *) pdir_fdesc)->file);

	// Search existing blocks for empty spot
	for (i = 0; i < nblock; i++) {
		int j;
		number = get_file_block(object, ((struct josfs_fdesc *) pdir_fdesc)->file, i * JOSFS_BLKSIZE);
		if (number != INVALID_BLOCK)
			blk = josfs_lookup_block(object, number);
		else
			blk = NULL;
		if (!blk)
			goto allocate_name_exit2;

		f = (JOSFS_File_t *) blk->ddesc->data;
		// Search for an empty slot
		for (j = 0; j < JOSFS_BLKFILES; j++) {
			if (!f[j].f_name[0]) {
				memset(&temp_file, 0, sizeof(JOSFS_File_t));
				strcpy(temp_file.f_name, name);
				temp_file.f_type = type;

				offset = j * sizeof(JOSFS_File_t);
				if ((r = chdesc_create_byte(blk, info->ubd, offset, sizeof(JOSFS_File_t), &temp_file, head)) < 0) 
					goto allocate_name_exit2;

				r = CALL(info->ubd, write_block, blk);
				if (r < 0)
					goto allocate_name_exit2;

				new_fdesc->file = malloc(sizeof(JOSFS_File_t));
				assert(new_fdesc->file); // TODO: handle error
				memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
				new_fdesc->dirb = blk->number;
				new_fdesc->index = j * sizeof(JOSFS_File_t);
				new_fdesc->ino = blk->number * JOSFS_BLKFILES + j;
				josfs_free_fdesc(object, pdir_fdesc);
				*newino = new_fdesc->ino;
				return (fdesc_t *) new_fdesc;
			}
		}
		blk = NULL;
	}

	// No empty slots, gotta allocate a new block
	number = josfs_allocate_block(object, NULL, 0, head);
	if (number != INVALID_BLOCK)
		blk = josfs_lookup_block(object, number);
	else
		blk = NULL;
	if (!blk)
		goto allocate_name_exit2;
	temp_head = *head;
	if (chdesc_create_init(blk, info->ubd, &temp_head) < 0)
		goto allocate_name_exit3;

	/* TODO: change the order of the chdescs in this function! we "lose" the metadata update in the eventual head... */
	dir_fdesc = (struct josfs_fdesc *) josfs_lookup_inode(object, parent);
	assert(dir_fdesc && dir_fdesc->file);
	dir = dir_fdesc->file;
	updated_size = dir->f_size + JOSFS_BLKSIZE;
	r = josfs_set_metadata(object, (struct josfs_fdesc *) pdir_fdesc, KFS_feature_size.id, sizeof(uint32_t), &updated_size, &temp_head);
	josfs_free_fdesc(object, (fdesc_t *) dir_fdesc);
	dir_fdesc = NULL;
	dir = NULL;
	if (r < 0)
		goto allocate_name_exit3;

	memset(&temp_file, 0, sizeof(JOSFS_File_t));
	strcpy(temp_file.f_name, name);
	temp_file.f_type = type;

	if (chdesc_create_byte(blk, info->ubd, 0, sizeof(JOSFS_File_t), &temp_file, head) < 0)
		goto allocate_name_exit3;

	if ((r = CALL(info->ubd, write_block, blk)) < 0)
		goto allocate_name_exit3;
		
	if (josfs_append_file_block(object, pdir_fdesc, number, head) >= 0) {
		new_fdesc->file = malloc(sizeof(JOSFS_File_t));
		memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
		new_fdesc->dirb = blk->number;
		new_fdesc->index = 0;
		new_fdesc->ino = blk->number * JOSFS_BLKFILES;
		*newino = new_fdesc->ino;
		josfs_free_fdesc(object, pdir_fdesc);
		return (fdesc_t *) new_fdesc;
	}

allocate_name_exit3:
	josfs_free_block(object, NULL, number, head);
allocate_name_exit2:
	josfs_free_fdesc(object, pdir_fdesc);
allocate_name_exit:
	free(new_fdesc);
	return NULL;
}

static int josfs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_rename\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * oldfdesc;
	fdesc_t * newfdesc;
	struct josfs_fdesc * old;
	struct josfs_fdesc * new;
	JOSFS_File_t * oldfile;
	JOSFS_File_t temp_file;
	bdesc_t * dirblock = NULL;
	int i, r, offset;
	uint8_t filetype;
	inode_t inode;
	inode_t not_used;

	if (!head)
		return -E_INVAL;

	r = josfs_lookup_name(object, oldparent, oldname, &inode);
	if (r)
		return -E_NOT_FOUND;

	oldfdesc = josfs_lookup_inode(object, inode);
	if (!oldfdesc)
		return -E_NOT_FOUND;

	old = (struct josfs_fdesc *) oldfdesc;
	dirblock = CALL(info->ubd, read_block, old->dirb, 1);
	if (!dirblock) {
		josfs_free_fdesc(object, oldfdesc);
		return -E_INVAL;
	}

	oldfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + old->index);
	memcpy(&temp_file, oldfile, sizeof(JOSFS_File_t));
	josfs_free_fdesc(object, oldfdesc);

	switch (temp_file.f_type)
	{
		case JOSFS_TYPE_FILE:
			filetype = TYPE_FILE;
			break;
		case JOSFS_TYPE_DIR:
			filetype = TYPE_DIR;
			break;
		default:
			filetype = TYPE_INVAL;
	}

	newfdesc = josfs_allocate_name(object, newparent, newname, filetype, NULL, &not_used, head);
	if (!newfdesc)
		return -E_FILE_EXISTS;

	new = (struct josfs_fdesc *) newfdesc;
	strcpy(temp_file.f_name, new->file->f_name);
	new->file->f_size = temp_file.f_size;
	new->file->f_indirect = temp_file.f_indirect;
	for (i = 0; i < JOSFS_NDIRECT; i++)
		new->file->f_direct[i] = temp_file.f_direct[i];

	dirblock = CALL(info->ubd, read_block, new->dirb, 1);
	if (!dirblock) {
		josfs_free_fdesc(object, newfdesc);
		return -E_INVAL;
	}

	offset = new->index;
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(JOSFS_File_t), &temp_file, head)) < 0) {
		josfs_free_fdesc(object, newfdesc);
		return r;
	}

	josfs_free_fdesc(object, newfdesc);
	r = CALL(info->ubd, write_block, dirblock);

	if (r < 0)
		return r;

	if (josfs_remove_name(object, oldparent, oldname, head) < 0)
		return josfs_remove_name(object, newparent, newname, head);

	return 0;
}

static uint32_t josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_truncate_file_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(info, f->file);
	bdesc_t * indirect = NULL, *dirblock = NULL;
	uint32_t blockno, data = 0;
	uint16_t offset;
	int r;

	if (!head || nblocks > JOSFS_NINDIRECT || nblocks < 1)
		return INVALID_BLOCK;

	if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
		offset = (nblocks - 1) * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, indirect);
		return blockno;
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_indirect = 0;
		r = josfs_free_block(object, NULL, indirect->number, head);

		return blockno;
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks - 1];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_direct[nblocks - 1] = 0;

		return blockno;
	}
}

static int josfs_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_free_block\n");
	return write_bitmap(object, block, 1, head);
}

static int josfs_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_remove_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * file;
	bdesc_t * dirblock = NULL;
	struct josfs_fdesc * f;
	int r;
	uint16_t offset;
	uint8_t data = 0;
	inode_t inode;

	if (!head)
		return -E_INVAL;

	r = josfs_lookup_name(object, parent, name, &inode);
	if (r)
		return -E_INVAL;

	file = josfs_lookup_inode(object, inode);
	if (!file)
		return -E_INVAL;

	f = (struct josfs_fdesc *) file;

	dirblock = CALL(info->ubd, read_block, f->dirb, 1);
	if (!dirblock) {
		r = -E_NO_DISK;
		goto remove_name_exit;
	}

	offset = f->index;
	offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_name[0];
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, 1, &data, head)) < 0)
		goto remove_name_exit;

	r = CALL(info->ubd, write_block, dirblock);

	if (r >= 0)
		f->file->f_name[0] = '\0';

remove_name_exit:
	josfs_free_fdesc(object, file);
	return r;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_write_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (!head)
		return -E_INVAL;

	/* XXX: with blockman, I don't think this can happen anymore... */
	if (info->bitmap_cache && info->bitmap_cache->number == block->number)
		bdesc_release(&info->bitmap_cache);

	return CALL(info->ubd, write_block, block);
}

static const feature_t * josfs_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_freespace, &KFS_feature_file_lfs, &KFS_feature_blocksize, &KFS_feature_devicesize};

static size_t josfs_get_num_features(LFS_t * object, inode_t ino)
{
	return sizeof(josfs_features) / sizeof(josfs_features[0]);
}

static const feature_t * josfs_get_feature(LFS_t * object, inode_t ino, size_t num)
{
	if(num < 0 || num >= sizeof(josfs_features) / sizeof(josfs_features[0]))
		return NULL;
	return josfs_features[num];
}

static int josfs_get_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (id == KFS_feature_size.id) {
		if (!f)
			return -E_INVAL;

		*data = malloc(sizeof(int32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(int32_t);
		memcpy(*data, &(f->file->f_size), sizeof(int32_t));
	}
	else if (id == KFS_feature_filetype.id) {
		if (!f)
			return -E_INVAL;

		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		switch(f->file->f_type)
		{
			case JOSFS_TYPE_FILE:
				*((uint32_t *) *data) = TYPE_FILE;
				break;
			case JOSFS_TYPE_DIR:
				*((uint32_t *) *data) = TYPE_DIR;
				break;
			default:
				*((uint32_t *) *data) = TYPE_INVAL;
		}
	}
	else if (id == KFS_feature_freespace.id) {
		uint32_t free_space;
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		free_space = count_free_space(object);
		memcpy(*data, &free_space, sizeof(uint32_t));
	}
	else if (id == KFS_feature_file_lfs.id) {
		*data = malloc(sizeof(object));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(object);
		memcpy(*data, &object, sizeof(object));
	}
	else if (id == KFS_feature_blocksize.id) {
		uint32_t blocksize = josfs_get_blocksize(object);
		*data = malloc(sizeof(blocksize));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(blocksize);
		memcpy(*data, &blocksize, sizeof(blocksize));
	}
	else if (id == KFS_feature_devicesize.id) {
		uint32_t devicesize = super->s_nblocks;
		*data = malloc(sizeof(devicesize));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(devicesize);
		memcpy(*data, &devicesize, sizeof(devicesize));
	}
	else
		return -E_INVAL;

	return 0;
}

static int josfs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata_inode %u\n", ino);
	int r;
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_inode(object, ino);
	r = josfs_get_metadata(object, f, id, size, data);
	if (f)
		josfs_free_fdesc(object, (fdesc_t *) f);
	return r;
	
}

static int josfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_get_metadata(object, f, id, size, data);
}

static int josfs_set_metadata(LFS_t * object, struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_set_metadata %s, %u, %u\n", f->file->f_name, id, size);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * dirblock = NULL;
	int r;
	uint16_t offset;

	if (!head)
		return -E_INVAL;

	if (id == KFS_feature_size.id) {
		if (sizeof(int32_t) != size || *((int32_t *) data) < 0 || *((int32_t *) data) >= JOSFS_MAXFILESIZE)
			return -E_INVAL;

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_INVAL;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_size;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(int32_t), data, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_size = *((int32_t *) data);
		return 0;
	}
	else if (id == KFS_feature_filetype.id) {
		uint32_t fs_type;
		if (sizeof(uint32_t) != size)
			return -E_INVAL;
		switch(*((uint32_t *) data))
		{
			case TYPE_FILE:
				fs_type = JOSFS_TYPE_FILE;
				break;
			case TYPE_DIR:
				fs_type = JOSFS_TYPE_DIR;
				break;
			default:
				return -E_INVAL;
		}

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_INVAL;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_type;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &fs_type, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return r;

		f->file->f_type = fs_type;
		return 0;
	}

	return -E_INVAL;
}

static int josfs_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	int r;
	struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_inode(object, ino);
	if (!f)
		return -E_INVAL;
	r = josfs_set_metadata(object, f, id, size, data, head);
	josfs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int josfs_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_set_metadata(object, f, id, size, data, head);
}

static int josfs_destroy(LFS_t * lfs)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(lfs);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);

	bdesc_release(&info->super_block);
	bdesc_release(&info->bitmap_cache);

	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

LFS_t * josfs(BD_t * block_device)
{
	struct lfs_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));

	if (PGSIZE != 4096) {
		free(lfs);
		Dprintf("JOSFSDEBUG: PGSIZE != 4096\n");
		return NULL;
	}
	
	if (!lfs)
		return NULL;

	info = malloc(sizeof(*info));
	if (!info) {
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, josfs, info);
	OBJMAGIC(lfs) = JOSFS_FS_MAGIC;

	info->ubd = block_device;
	info->bitmap_cache = NULL;

	if (check_super(lfs)) {
		free(info);
		free(lfs);
		return NULL;
	}

	if (check_bitmap(lfs)) {
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
