#include <lib/platform.h>
#include <lib/hash_set.h>
#include <lib/jiffies.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/debug.h>
#include <kfs/modman.h>
#include <kfs/josfs_base.h>

#define JOSFS_BASE_DEBUG 0

#if JOSFS_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define INODE_ROOT ((inode_t) 1)

#define block_is_free read_bitmap
#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)

struct josfs_info
{
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
	uint32_t numblocks;

	/* make sure we have the block size we expect */
	if (object->blockdev->blocksize != JOSFS_BLKSIZE) {
		printf("Block device size is not JOSFS_BLKSIZE!\n");
		return -1;
	}

	/* the superblock is in block 1 */
	info->super_block = CALL(object->blockdev, read_block, 1, 1);
	if (!info->super_block)
	{
		printf("Unable to read superblock!\n");
		return -1;
	}
	if (super->s_magic != JOSFS_FS_MAGIC) {
		printf("josfs_base: bad file system magic number\n");
		return -1;
	}

	numblocks = object->blockdev->numblocks;

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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
	int i, blocks_to_read;

	blocks_to_read = super->s_nblocks / JOSFS_BLKBITSIZE;

	if (super->s_nblocks % JOSFS_BLKBITSIZE)
		blocks_to_read++;

	// Make sure the reserved and root blocks are marked in-use.
	if (block_is_free(object, 0) || block_is_free(object, 1)) {
		printf("josfs_base: Boot Sector or Partition Table marked free!\n");
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
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
		bdesc = CALL(object->blockdev, read_block, target, 1);
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
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
		bdesc = CALL(object->blockdev, read_block, target, 1);

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
	r = chdesc_create_bit(bdesc, object->blockdev, (blockno % JOSFS_BLKBITSIZE) / 32, 1 << (blockno % 32), head);
	if (r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, value ? "free block" : "allocate block");

	r = CALL(object->blockdev, write_block, bdesc);

	return r;
}

static uint32_t count_free_space(LFS_t * object)
{
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
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
					return -ENOMEM;
				}
			}
		}
	}

	*file = NULL;
	Dprintf("JOSFSDEBUG: dir_lookup done: NOT FOUND\n");
	return -ENOENT;
}

static int josfs_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -EINVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int josfs_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -EINVAL;
	
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int josfs_get_root(LFS_t * object, inode_t * ino)
{
	*ino = INODE_ROOT;
	return 0;
}

// file and purpose parameter are ignored
static uint32_t josfs_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_block\n");
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
	uint32_t s_nblocks = super->s_nblocks;
	uint32_t bitmap_size = (s_nblocks + JOSFS_BLKBITSIZE + 1) / JOSFS_BLKBITSIZE;
	uint32_t bitmap_block, blockno;
	uint32_t * curbitmap;
	int r;

	if (!head)
		return INVALID_BLOCK;

	for (bitmap_block = 0; bitmap_block < bitmap_size; bitmap_block++)
	{
		if (info->bitmap_cache && info->bitmap_cache->number != bitmap_block+2)
			bdesc_release(&info->bitmap_cache);
		if (!info->bitmap_cache)
		{
			bdesc_t * bdesc = CALL(object->blockdev, read_block, bitmap_block+2, 1);
			if (!bdesc || bdesc->ddesc->length != JOSFS_BLKSIZE)
			{
				printf("josfs_base: trouble reading bitmap! (blockno = %u)\n", bitmap_block+2);
				return -1;
			}
			bdesc_retain(bdesc);
			info->bitmap_cache = bdesc;
		}

		curbitmap = (uint32_t *) info->bitmap_cache->ddesc->data;
		for (blockno = 0; blockno < JOSFS_BLKBITSIZE; blockno += 32, curbitmap++)
		{
			uint32_t mask = 1;
			uint32_t full_blockno;

			if (!*curbitmap)
				continue;

			while (!(*curbitmap & mask))
				mask <<= 1, blockno++;
			full_blockno = blockno + bitmap_block * JOSFS_BLKBITSIZE;

			r = write_bitmap(object, full_blockno, 0, head);
			if (r < 0)
				return INVALID_BLOCK;
			assert(!block_is_free(object, full_blockno));
			return full_blockno;
		}
	}

	return INVALID_BLOCK;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_block %u\n", number);
	return CALL(object->blockdev, read_block, number, 1);
}

static bdesc_t * josfs_synthetic_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_synthetic_lookup_block %u\n", number);
	return CALL(object->blockdev, synthetic_read_block, number, 1);
}

static fdesc_t * josfs_lookup_inode(LFS_t * object, inode_t ino)
{
	struct josfs_fdesc * fd;
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
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
			goto josfs_lookup_inode_exit;

		dirblock = CALL(object->blockdev, read_block, fd->dirb, 1);
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);
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
		return -EINVAL;
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

static uint32_t get_file_numblocks(LFS_t *object, JOSFS_File_t * file)
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
		indirect = CALL(object->blockdev, read_block, file->f_indirect, 1);
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
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return get_file_numblocks(object, f->file);
}

static uint32_t get_file_block(LFS_t * object, JOSFS_File_t * file, uint32_t offset)
{
	bdesc_t * indirect;
	uint32_t blockno, nblocks;

	nblocks = get_file_numblocks(object, file);
	if (offset % JOSFS_BLKSIZE || offset >= nblocks * JOSFS_BLKSIZE)
		return INVALID_BLOCK;

	if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
		indirect = CALL(object->blockdev, read_block, file->f_indirect, 1);
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
		return -EINVAL;

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
	bdesc_t * dirblock = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;

	// Make sure it's a directory and we can read from it
	if (file->f_type != JOSFS_TYPE_DIR)
		return -ENOTDIR;

	blockno = *basep / JOSFS_BLKFILES;

	if (blockno >= get_file_numblocks(object, file))
		return -1;

	blockno = get_file_block(object, file, blockno * JOSFS_BLKSIZE);
	if (blockno != INVALID_BLOCK)
		dirblock = josfs_lookup_block(object, blockno);
	if (!dirblock)
		return -ENOENT;
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
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;
	int r;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type != JOSFS_TYPE_DIR)
		return -ENOTDIR;

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

	do {
		blockno = (*basep - 2) / JOSFS_BLKFILES;

		if (blockno >= get_file_numblocks(object, f->file))
			return -1;

		blockno = get_file_block(object, f->file, blockno * JOSFS_BLKSIZE);
		if (blockno != INVALID_BLOCK)
			dirblock = josfs_lookup_block(object, blockno);
		if (!dirblock)
			return -ENOENT;
		dirfile = &((JOSFS_File_t *) dirblock->ddesc->data)[(*basep - 2) % JOSFS_BLKFILES];

		r = fill_dirent(dirfile, f->ino, entry, size, basep);
	} while (r >= 0 && !entry->d_reclen);
	return r;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_append_file_block\n");
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(object, f->file);
	bdesc_t * indirect = NULL, * dirblock = NULL;
	int r, offset;

	if (!head || nblocks < 0)
		return -EINVAL;
	if (nblocks >= JOSFS_NINDIRECT)
		return -ENOSPC;

	if (nblocks > JOSFS_NDIRECT) {
		indirect = CALL(object->blockdev, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return -ENOSPC;

		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, object->blockdev, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add indirect block");

		return CALL(object->blockdev, write_block, indirect);
	}
	else if (nblocks == JOSFS_NDIRECT) {
		uint32_t inumber = josfs_allocate_block(object, NULL, 0, head);
		bdesc_t * indirect;
		if (inumber == INVALID_BLOCK)
			return -ENOSPC;
		indirect = josfs_synthetic_lookup_block(object, inumber);

		// Initialize the new indirect block
		if ((r = chdesc_create_init(indirect, object->blockdev, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init indirect block");

		// Initialize the structure, then point to it
		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return -ENOSPC;

		// this head is from josfs_allocate_block() above
		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, object->blockdev, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add indirect block");

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), &inumber, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "set indirect block");

		/* FIXME handle the return values better? */
		r = CALL(object->blockdev, write_block, indirect);
		r |= CALL(object->blockdev, write_block, dirblock);

		if (r >= 0)
			f->file->f_indirect = inumber;

		return r;
	}
	else {
		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return -ENOSPC;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks];
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add direct block");

		r = CALL(object->blockdev, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_direct[nblocks] = block;
		return 0;
	}
}

static fdesc_t * josfs_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_name %s\n", name);
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
	nblock = get_file_numblocks(object, ((struct josfs_fdesc *) pdir_fdesc)->file);

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
				if ((r = chdesc_create_byte(blk, object->blockdev, offset, sizeof(JOSFS_File_t), &temp_file, head)) < 0) 
					goto allocate_name_exit2;
				KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init dirent");

				r = CALL(object->blockdev, write_block, blk);
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
		blk = josfs_synthetic_lookup_block(object, number);
	else
		blk = NULL;
	if (!blk)
		goto allocate_name_exit2;
	if (chdesc_create_init(blk, object->blockdev, head) < 0)
		goto allocate_name_exit3;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init dir block");

	dir_fdesc = (struct josfs_fdesc *) josfs_lookup_inode(object, parent);
	assert(dir_fdesc && dir_fdesc->file);
	dir = dir_fdesc->file;
	updated_size = dir->f_size + JOSFS_BLKSIZE;
	temp_head = *head;
	r = josfs_set_metadata(object, (struct josfs_fdesc *) pdir_fdesc, KFS_FEATURE_SIZE, sizeof(uint32_t), &updated_size, &temp_head);
	josfs_free_fdesc(object, (fdesc_t *) dir_fdesc);
	dir_fdesc = NULL;
	dir = NULL;
	if (r < 0)
		goto allocate_name_exit3;
	r = lfs_add_fork_head(temp_head);
	assert(r >= 0);

	memset(&temp_file, 0, sizeof(JOSFS_File_t));
	strcpy(temp_file.f_name, name);
	temp_file.f_type = type;

	temp_head = *head;
	if (chdesc_create_byte(blk, object->blockdev, 0, sizeof(JOSFS_File_t), &temp_file, head) < 0)
		goto allocate_name_exit3;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init dirent");

	if ((r = CALL(object->blockdev, write_block, blk)) < 0)
		goto allocate_name_exit3;
		
	if (josfs_append_file_block(object, pdir_fdesc, number, &temp_head) >= 0) {
		r = lfs_add_fork_head(temp_head);
		assert(r >= 0);
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

static int empty_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	return -ENOENT;
}

static int josfs_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_rename\n");
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
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };

	if (!head)
		return -EINVAL;

	r = josfs_lookup_name(object, oldparent, oldname, &inode);
	if (r)
		return r;

	oldfdesc = josfs_lookup_inode(object, inode);
	if (!oldfdesc)
		return -ENOENT;

	old = (struct josfs_fdesc *) oldfdesc;
	dirblock = CALL(object->blockdev, read_block, old->dirb, 1);
	if (!dirblock) {
		josfs_free_fdesc(object, oldfdesc);
		return -EINVAL;
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

	newfdesc = josfs_allocate_name(object, newparent, newname, filetype, NULL, &emptymd, &not_used, head);
	if (!newfdesc)
		return -EEXIST;

	new = (struct josfs_fdesc *) newfdesc;
	strcpy(temp_file.f_name, new->file->f_name);
	new->file->f_size = temp_file.f_size;
	new->file->f_indirect = temp_file.f_indirect;
	for (i = 0; i < JOSFS_NDIRECT; i++)
		new->file->f_direct[i] = temp_file.f_direct[i];

	dirblock = CALL(object->blockdev, read_block, new->dirb, 1);
	if (!dirblock) {
		josfs_free_fdesc(object, newfdesc);
		return -EINVAL;
	}

	/* WARNING: JOSFS has no inodes, so we write a copy of the combined
	 * inode/dirent before freeing the old one in order to avoid losing the
	 * file. But this is not soft updates safe, as we might crash and later
	 * delete one of the files, marking its resources as free. Oh well. */
	offset = new->index;
	if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(JOSFS_File_t), &temp_file, head)) < 0) {
		josfs_free_fdesc(object, newfdesc);
		return r;
	}
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "rename");

	josfs_free_fdesc(object, newfdesc);
	r = CALL(object->blockdev, write_block, dirblock);

	if (r < 0)
		return r;

	if (josfs_remove_name(object, oldparent, oldname, head) < 0)
		return josfs_remove_name(object, newparent, newname, head);

	return 0;
}

static uint32_t josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_truncate_file_block\n");
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(object, f->file);
	bdesc_t * indirect = NULL, *dirblock = NULL;
	uint32_t blockno, data = 0;
	uint16_t offset;
	int r;

	if (!head || nblocks > JOSFS_NINDIRECT || nblocks < 1)
		return INVALID_BLOCK;

	if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(object->blockdev, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
		offset = (nblocks - 1) * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, object->blockdev, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "detach indirect block");

		r = CALL(object->blockdev, write_block, indirect);
		return blockno;
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(object->blockdev, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);

		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "clear indirect block");

		r = CALL(object->blockdev, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_indirect = 0;
		r = josfs_free_block(object, NULL, indirect->number, head);

		return blockno;
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks - 1];
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "detach direct block");

		r = CALL(object->blockdev, write_block, dirblock);

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
	fdesc_t * file;
	bdesc_t * dirblock = NULL;
	struct josfs_fdesc * f;
	uint16_t offset;
	uint8_t data = 0;
	inode_t inode;
	int r;

	if (!head)
		return -EINVAL;

	r = josfs_lookup_name(object, parent, name, &inode);
	if (r)
		return r;

	file = josfs_lookup_inode(object, inode);
	if (!file)
		return -EINVAL;

	f = (struct josfs_fdesc *) file;

	dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
	if (!dirblock) {
		r = -ENOSPC;
		goto remove_name_exit;
	}

	offset = f->index;
	offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_name[0];
	if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, 1, &data, head)) < 0)
		goto remove_name_exit;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "clear name[0]");

	r = CALL(object->blockdev, write_block, dirblock);
	if (r >= 0)
		f->file->f_name[0] = '\0';

	/* free all the file's blocks */
	if(f->file->f_direct[0])
	{
		int i;
		chdesc_t * fork;
		
		if(f->file->f_indirect)
		{
			bdesc_t * indirect = CALL(object->blockdev, read_block, f->file->f_indirect, 1);
			uint32_t * blocks = (uint32_t *) indirect->ddesc->data;
			
			for(i = JOSFS_NDIRECT; i < JOSFS_NINDIRECT; i++)
				if(blocks[i])
				{
					fork = *head;
					r = josfs_free_block(object, file, blocks[i], &fork);
					r = lfs_add_fork_head(fork);
				}
			r = josfs_free_block(object, file, f->file->f_indirect, &fork);
			r = lfs_add_fork_head(fork);
			f->file->f_indirect = 0;
		}
		for(i = 0; i < JOSFS_NDIRECT; i++)
			if(f->file->f_direct[i])
			{
				fork = *head;
				r = josfs_free_block(object, file, f->file->f_direct[i], &fork);
				r = lfs_add_fork_head(fork);
				f->file->f_direct[i] = 0;
			}
	}

remove_name_exit:
	josfs_free_fdesc(object, file);
	return r;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_write_block\n");
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);

	if (!head)
		return -EINVAL;

	/* XXX: with blockman, I don't think this can happen anymore... */
	if (info->bitmap_cache && info->bitmap_cache->number == block->number)
		bdesc_release(&info->bitmap_cache);

	return CALL(object->blockdev, write_block, block);
}

static chdesc_t ** josfs_get_write_head(LFS_t * object)
{
	Dprintf("JOSFSDEBUG: josfs_get_write_head\n");
	return CALL(object->blockdev, get_write_head);
}

static int32_t josfs_get_block_space(LFS_t * object)
{
	Dprintf("JOSFSDEBUG: josfs_get_block_space\n");
	return CALL(object->blockdev, get_block_space);
}

static const bool josfs_features[] = {[KFS_FEATURE_SIZE] = 1, [KFS_FEATURE_FILETYPE] = 1, [KFS_FEATURE_FREESPACE] = 1, [KFS_FEATURE_FILE_LFS] = 1, [KFS_FEATURE_BLOCKSIZE] = 1, [KFS_FEATURE_DEVSIZE] = 1, [KFS_FEATURE_MTIME] = 1, [KFS_FEATURE_ATIME] = 1, [KFS_FEATURE_DELETE] = 1};

static size_t josfs_get_max_feature_id(LFS_t * object)
{
	return sizeof(josfs_features) / sizeof(josfs_features[0]) - 1;
}

static const bool * josfs_get_feature_array(LFS_t * object)
{
	return josfs_features;
}

static int josfs_get_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, void * data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata\n");
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(object);

	if (id == KFS_FEATURE_SIZE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(int32_t))
			return -ENOMEM;
		size = sizeof(int32_t);

		*((int32_t *) data) = f->file->f_size;
	}
	else if (id == KFS_FEATURE_FILETYPE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		if (f->file->f_type == JOSFS_TYPE_FILE)
			*((uint32_t *) data) = TYPE_FILE;
		else if (f->file->f_type == JOSFS_TYPE_DIR)
			*((uint32_t *) data) = TYPE_DIR;
		else
			*((uint32_t *) data) = TYPE_INVAL;
	}
	else if (id == KFS_FEATURE_FREESPACE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = count_free_space(object);
	}
	else if (id == KFS_FEATURE_FILE_LFS) {
		if (size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);

		*((typeof(object) *) data) = object;
	}
	else if (id == KFS_FEATURE_BLOCKSIZE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = JOSFS_BLKSIZE;
	}
	else if (id == KFS_FEATURE_DEVSIZE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = super->s_nblocks;
	}
	else if (id == KFS_FEATURE_MTIME || id == KFS_FEATURE_ATIME) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		if (id == KFS_FEATURE_MTIME)
			*((uint32_t *) data) = f->file->f_mtime;
		else
			*((uint32_t *) data) = f->file->f_atime;
	}
	else
		return -EINVAL;

	return size;
}

static int josfs_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata_inode %u\n", ino);
	int r;
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_inode(object, ino);
	r = josfs_get_metadata(object, f, id, size, data);
	if (f)
		josfs_free_fdesc(object, (fdesc_t *) f);
	return r;
	
}

static int josfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_get_metadata(object, f, id, size, data);
}

static int josfs_set_metadata(LFS_t * object, struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	Dprintf("JOSFSDEBUG: josfs_set_metadata %s, %u, %u\n", f->file->f_name, id, size);
	bdesc_t * dirblock = NULL;
	int r;
	uint16_t offset;

	if (!head)
		return -EINVAL;

	if (id == KFS_FEATURE_SIZE) {
		if (sizeof(int32_t) != size || *((int32_t *) data) < 0 || *((int32_t *) data) > JOSFS_MAXFILESIZE)
			return -EINVAL;

		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return -EINVAL;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_size;
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(int32_t), data, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "set file size");

		r = CALL(object->blockdev, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_size = *((int32_t *) data);
		return 0;
	}
	else if (id == KFS_FEATURE_FILETYPE) {
		uint32_t fs_type;
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		switch(*((uint32_t *) data))
		{
			case TYPE_FILE:
				fs_type = JOSFS_TYPE_FILE;
				break;
			case TYPE_DIR:
				fs_type = JOSFS_TYPE_DIR;
				break;
			default:
				return -EINVAL;
		}

		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return -EINVAL;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_type;
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), &fs_type, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "set file type");

		r = CALL(object->blockdev, write_block, dirblock);

		if (r < 0)
			return r;

		f->file->f_type = fs_type;
		return 0;
	}
	else if (id == KFS_FEATURE_MTIME || id == KFS_FEATURE_ATIME) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;

		dirblock = CALL(object->blockdev, read_block, f->dirb, 1);
		if (!dirblock)
			return -EINVAL;

		offset = f->index;
		if (id == KFS_FEATURE_MTIME)
			offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_mtime;
		else
			offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_atime;
		if ((r = chdesc_create_byte(dirblock, object->blockdev, offset, sizeof(uint32_t), data, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, (id == KFS_FEATURE_MTIME) ? "set file mtime" : "set file atime");

		r = CALL(object->blockdev, write_block, dirblock);
		if (r < 0)
			return r;

		if (id == KFS_FEATURE_MTIME)
			f->file->f_mtime = *((uint32_t *) data);
		else
			f->file->f_atime = *((uint32_t *) data);
		return 0;
	}

	return -EINVAL;
}

static int josfs_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	int r;
	struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_inode(object, ino);
	if (!f)
		return -EINVAL;
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
	struct josfs_info * info = (struct josfs_info *) OBJLOCAL(lfs);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(lfs->blockdev, lfs);

	bdesc_release(&info->super_block);
	bdesc_release(&info->bitmap_cache);

	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

LFS_t * josfs(BD_t * block_device)
{
	struct josfs_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));

	if (!lfs)
		return NULL;

	info = malloc(sizeof(*info));
	if (!info) {
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, josfs, info);
	OBJMAGIC(lfs) = JOSFS_FS_MAGIC;

	lfs->blockdev = block_device;
	info->bitmap_cache = NULL;
	lfs->blocksize = JOSFS_BLKSIZE;

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
