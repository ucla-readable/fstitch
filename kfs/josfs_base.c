// Need to handle change descriptors
// Make sure we're writing in the 'correct' order

/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/hash_set.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/josfs_base.h>

#define JOSFS_BASE_DEBUG 0

#if JOSFS_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define block_is_free read_bitmap
#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)
#define isvalid(x) (x >= reserved && x < s_nblocks)

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
};

struct josfs_fdesc {
	bdesc_t * dirb;
	int index;
	char fullpath[JOSFS_MAXPATHLEN];
	JOSFS_File_t * file;
};

static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int josfs_set_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);
static void get_parent_path(const char * path, char * parent);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);
static int dir_lookup(LFS_t * object, JOSFS_File_t* dir, const char* name, JOSFS_File_t** file, bdesc_t** dirb, int *index);

// Equivalent to JOS's read_super
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	uint32_t numblocks;

	/* make sure we have the block size we expect */
	if (CALL(info->ubd, get_blocksize) != JOSFS_BLKSIZE) {
		printf("Block device size is not BLKSIZE!\n");
		return -1;
	}

	/* the superblock is in block 1 */
	info->super_block = CALL(info->ubd, read_block, 1);
	bdesc_retain(&info->super_block);

	if (super->s_magic != JOSFS_FS_MAGIC) {
		printf("josfs_base: bad file system magic number\n");
		bdesc_release(&info->super_block);
		return -1;
	}

	numblocks = CALL(info->ubd, get_numblocks);

	printf("JOS Filesystem size: %d blocks (%dMB)\n", super->s_nblocks, super->s_nblocks / (1024 * 1024 / BLKSIZE));
	if (super->s_nblocks > numblocks) {
		printf("josfs_base: file system is too large\n");
		bdesc_release(&info->super_block);
		return -1;
	}

	return 0;
}

// Equivalent to JOS's read_bitmap
static int check_bitmap(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
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
			printf("josfs_base: Free Block Bitmap block %d marked free!\n");
			return -1;
		}
	}

	return 0;
}

static int fsck_file(LFS_t * object, JOSFS_File_t file, int reserved, int8_t *fbmap, int8_t *ubmap, uint32_t *blist)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	bdesc_t * indirect;
	const uint32_t  s_nblocks = super->s_nblocks;
	uint32_t * k;
	int j;

	// Get block list
	for (j = 0; j < JOSFS_NINDIRECT; j++)
		blist[j] = 0;
	for (j = 0; j < JOSFS_NDIRECT; j++)
		blist[j] = file.f_direct[j];
	if (file.f_indirect != 0) {
		if (isvalid(file.f_indirect)) {
			if (fbmap[file.f_indirect]) {
				printf("indirect block pointing to free block: %s\n", file.f_name);
			}
			else if (ubmap[file.f_indirect]) {
				ubmap[file.f_indirect]--;

				indirect = CALL(info->ubd, read_block, file.f_indirect);
				if (indirect) {
					k = ((uint32_t *) indirect->ddesc->data) + JOSFS_NDIRECT;
					for (j = JOSFS_NDIRECT; j < JOSFS_NINDIRECT; j++) {
						blist[j] = *k;
						k++;
					}
					bdesc_drop(&indirect);
				}
				else {
					printf("error reading indirect block!\n");
					return -1;
				}
			}
			else {
				printf("indirect block pointing to used block: %s, %d\n", file.f_name, file.f_indirect);
			}
		}
		else {
			printf("invalid indirect block number: %s, %d", file.f_name, file.f_indirect);
		}
	}

	// Check block list
	for (j = 0; j < JOSFS_NINDIRECT; j++) {
		Dprintf("block %d is %d\n", j, blist[j]);
		if (blist[j] == 0)
			break;
		else if (isvalid(blist[j]))
			if (fbmap[blist[j]])
				printf("file pointing to free block: %s\n", file.f_name);
			else if (ubmap[blist[j]])
				ubmap[blist[j]]--;
			else
				printf("file pointing to used block: %s, %d -> %d\n", file.f_name, j, blist[j]);
		else
			printf("file pointing to invalid block number: %s, %d -> %d\n", file.f_name, j, blist[j]);
	}
	Dprintf("%s is %d bytes, %d blocks\n", file.f_name, file.f_size, j);
	if (ROUNDUP32(file.f_size, JOSFS_BLKSIZE) != j*JOSFS_BLKSIZE)
		printf("Invalid file size: %s, %d bytes, %d blocks\n", file.f_name, file.f_size, j);

	return 0;
}

static int fsck_dir(LFS_t * object, fdesc_t * f, uint8_t * fbmap, uint8_t * ubmap, uint32_t * blist, hash_set_t * hsdirs)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	const uint32_t s_nblocks = super->s_nblocks;
	struct josfs_fdesc * fdesc = (struct josfs_fdesc *) f;
	int d, r, blockno;
	uint32_t basep = 0, i = 0;
	struct dirent entry;
	bdesc_t * dirblock;
	JOSFS_File_t * temp_file;
	JOSFS_File_t * target;
	int reserved = 2 + (s_nblocks / JOSFS_BLKBITSIZE);

	if (s_nblocks % JOSFS_BLKBITSIZE)
		reserved++;

	do {
		temp_file = malloc(sizeof(JOSFS_File_t));
		if (!temp_file) {
			r = -1;
			goto fsck_dir_cleanup;
		}

		d = josfs_get_dirent(object, (fdesc_t *) fdesc, &entry, sizeof(struct dirent), &basep);
		if (d != 0) {
			free(temp_file);
			i++;
			continue;
		}

		Dprintf("Checking %s\n", entry.d_name);
		blockno = i * sizeof(JOSFS_File_t) / JOSFS_BLKSIZE;
		dirblock = josfs_get_file_block(object, (fdesc_t *) fdesc, blockno * JOSFS_BLKSIZE);
		if (dirblock) {
			target = (JOSFS_File_t *) dirblock->ddesc->data;
			target += i % (JOSFS_BLKSIZE / sizeof(JOSFS_File_t));
			memcpy(temp_file, target, sizeof(JOSFS_File_t));
			bdesc_drop(&dirblock);
			if ((r = fsck_file(object, *temp_file, reserved, fbmap, ubmap, blist)) < 0)
				goto fsck_dir_cleanup;

			if (entry.d_type == TYPE_DIR) {
				if ((r = hash_set_insert(hsdirs, temp_file)) < 0) {
					printf("error with hash_set_insert()\n");
					goto fsck_dir_cleanup;
				}
			}
		}
		else {
			printf("error reading from file!\n");
			r = -1;
			goto fsck_dir_cleanup;
		}
		i++;
	}
	while (d >= 0);

	return 0;

fsck_dir_cleanup:
	free(temp_file);
	return r;
}

static int fsck(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	const uint32_t s_nblocks = super->s_nblocks;
	int8_t *free_bitmap = malloc(sizeof(int8_t) * s_nblocks);
	int8_t *used_bitmap = malloc(sizeof(int8_t) * s_nblocks);
	uint32_t *blocklist = malloc(sizeof(uint32_t) * JOSFS_NINDIRECT);
	struct josfs_fdesc temp_fdesc;
	JOSFS_File_t rootfile;
	JOSFS_File_t * dirfile = NULL;
	uint32_t j;
	hash_set_t * hsdirs = NULL;
	hash_set_it_t * hsitr = NULL;
	int reserved = 2 + (s_nblocks / JOSFS_BLKBITSIZE);
	int d = 0, r = 0;

	if (s_nblocks % JOSFS_BLKBITSIZE)
		reserved++;

	for (j = 0; j < s_nblocks; j++) {
		free_bitmap[j] = read_bitmap(object, j);
		used_bitmap[j] = 1 - free_bitmap[j];
	}
	for (j = 0; j < reserved; j++)
		used_bitmap[j]--;

	hsdirs = hash_set_create();
	if (hsdirs) {
		temp_fdesc.file = &rootfile;
		strcpy(temp_fdesc.fullpath, "");

		memcpy(&rootfile, &super->s_root, sizeof(JOSFS_File_t));
		fsck_file(object, rootfile, reserved, free_bitmap, used_bitmap, blocklist);

		do {
			if (r == 0)
				d = fsck_dir(object, (fdesc_t *) &temp_fdesc, free_bitmap, used_bitmap, blocklist, hsdirs);

			if (dirfile)
				free(dirfile);

			if (d < 0)
				r = d;

			hsitr = hash_set_it_create();
			if (hsitr) {
				dirfile = (JOSFS_File_t *) hash_set_next(hsdirs, hsitr);
				hash_set_it_destroy(hsitr);

				if (dirfile) {
					hash_set_erase(hsdirs, dirfile);
					temp_fdesc.file = dirfile;
				}
			}
			else {
				printf("hash_set_it_create failed!\n");
				r = -1;
				break; // gonna leak mem, but if we're here, we're probably screwed
			}
		}
		while (dirfile != NULL);

		if (r >= 0) {
			for (j = 0; j < s_nblocks; j++) {
				if (used_bitmap[j] > 0)
					printf("block %d is marked as used, but unclaimed\n", j);
				else if (used_bitmap[j] < 0)
					printf("block %d used by multiple files\n", j);
			}
		}
		hash_set_destroy(hsdirs);
	}
	else {
		printf("hash_set_create failed!\n");
		r = -1;
	}

	free(blocklist);
	free(free_bitmap);
	free(used_bitmap);
	if (r < 0)
		printf("JOS FSCK encountered some problems\n");
	else
		printf("JOS FSCK Complete!\n");
	return r;
}

// Return 1 if block is free
static int read_bitmap(LFS_t * object, uint32_t blockno)
{
	bdesc_t * bdesc;
	int target;
	uint32_t * ptr;
	bool result;

	target = 2 + (blockno / (JOSFS_BLKBITSIZE));

	bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

	if (bdesc->length != JOSFS_BLKSIZE) {
		printf("josfs_base: trouble reading bitmap!\n");
		bdesc_drop(&bdesc);
		return -1;
	}

	ptr = ((uint32_t *) bdesc->ddesc->data) + (blockno / 32);
	result = *ptr & (1 << (blockno % 32));
	bdesc_drop(&bdesc);

	if (result)
		return 1;
	return 0;
}

// FIXME chdesc
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: write_bitmap\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	bdesc_t * bdesc;
	int target;
	int r;
	uint32_t * ptr;

	if (blockno == 0) {
		printf("josfs_base: attempted to write to zero block!\n");
		return -1;
	}

	target = 2 + (blockno / (JOSFS_BLKBITSIZE));

	bdesc = CALL(info->ubd, read_block, target);

	if (bdesc->length != JOSFS_BLKSIZE) {
		printf("josfs_base: trouble reading bitmap!\n");
		return -1;
	}

	bdesc_touch(bdesc);
	ptr = ((uint32_t *) bdesc->ddesc->data) + (blockno / 32);
	if (value)
		*ptr |= (1 << (blockno % 32));
	else
		*ptr &= ~(1 << (blockno % 32));

	if ((r = CALL(info->ubd, write_block, bdesc)) < 0)
		return r;

	return 0;
}

// Skip over slashes.
static inline const char* skip_slash(const char* p)
{
	while (*p == '/')
		p++;
	return p;
}

static void get_parent_path(const char * path, char * parent) {
	int i;
	int len = strlen(path);
	strcpy(parent, path);

	while (parent[len-1] == '/') {
		parent[len-1] = 0;
		len--;
	}

	for (i = len - 1; i >= 0; i--) {
		if (parent[i] == '/')
			break;
	}

	while (parent[i] == '/') {
		parent[i] = 0;
		i--;
	}
}

// Evaluate a path name, starting at the root.
// On success, set *pfile to the file we found
// and set *pdir to the directory the file is in.
// If we cannot find the file but find the directory
// it should be in, set *pdir and copy the final path
// element into lastelem.
static int walk_path(LFS_t * object, const char* path, JOSFS_File_t** pdir, JOSFS_File_t** pfile, char* lastelem, bdesc_t** dirb, int *index)
{
	Dprintf("JOSFSDEBUG: walk_path %s\n", path);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	const char* p;
	char name[JOSFS_MAXNAMELEN];
	JOSFS_File_t *dir = NULL, *file = NULL;
	int r;

	path = skip_slash(path);
	file = &super->s_root;
	dir = 0;
	name[0] = 0;

	if (pdir)
		*pdir = 0;
	*pfile = 0;
	while (*path != '\0') {
		dir = file;
		p = path;
		while (*path != '/' && *path != '\0')
			path++;
		if (path - p >= JOSFS_MAXNAMELEN)
			return -E_BAD_PATH;
		memcpy(name, p, path - p);
		name[path - p] = '\0';
		path = skip_slash(path);

		if (dir->f_type != TYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(object, dir, name, &file, dirb, index)) < 0) {
			if (r == -E_NOT_FOUND && *path == '\0') {
				if (pdir)
					*pdir = dir;
				if (lastelem)
					strcpy(lastelem, name);
				*pfile = 0;
			}
			return r;
		}
	}

	if (pdir)
		*pdir = dir;
	*pfile = file;
	return 0;
}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, JOSFS_File_t* dir, const char* name, JOSFS_File_t** file, bdesc_t** dirb, int *index)
{
	Dprintf("JOSFSDEBUG: dir_lookup %s\n", name);
	int r, blockno;
	uint32_t i = 0, basep = 0;
	struct dirent entry;
	bdesc_t * dirblock;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	JOSFS_File_t * target;

	if (!temp_fdesc) {
		Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM\n");
		return -E_NO_MEM;
	}

	strcpy(temp_fdesc->fullpath, name);
	temp_fdesc->file = dir;
	do {
		r = josfs_get_dirent(object, (fdesc_t *) temp_fdesc, &entry, sizeof(struct dirent), &basep);
		if (r == 0 && strcmp(entry.d_name, name) == 0) {
			blockno = i * sizeof(JOSFS_File_t) / JOSFS_BLKSIZE;
			dirblock = josfs_get_file_block(object, (fdesc_t *) temp_fdesc, blockno * JOSFS_BLKSIZE);
			if (dirblock) {
				*index = i % (JOSFS_BLKSIZE / sizeof(JOSFS_File_t));
				target = (JOSFS_File_t *) dirblock->ddesc->data;
				target += *index;
				*file = malloc(sizeof(JOSFS_File_t));
				if (*file) {
					memcpy(*file, target, sizeof(JOSFS_File_t));
					(*file)->f_dir = dir;

					*dirb = dirblock;
					bdesc_retain(&dirblock);
					free(temp_fdesc);
					Dprintf("JOSFSDEBUG: dir_lookup done: FOUND\n");
					return 0;
				}
				else {
					bdesc_drop(&dirblock);
					free(temp_fdesc);
					Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM2\n");
					return -E_NO_MEM;
				}
			}
		}
		i++;
	} while (r >= 0);

	free(temp_fdesc);

	Dprintf("JOSFSDEBUG: dir_lookup done: NOT FOUND\n");
	return -E_NOT_FOUND;
}

static uint32_t josfs_get_blocksize(LFS_t * object)
{
	return JOSFS_BLKSIZE;
}

static BD_t * josfs_get_blockdev(LFS_t * object)
{
	return ((struct lfs_info *) object->instance)->ubd;
}

// purpose parameter is ignored
static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	int blockno;
	int bitmap_size;
	int s_nblocks;

	if (size != JOSFS_BLKSIZE)
		return NULL;

	s_nblocks = super->s_nblocks;
	bitmap_size = s_nblocks / JOSFS_BLKBITSIZE;

	if (s_nblocks % JOSFS_BLKBITSIZE)
		bitmap_size++;

	for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
		if (block_is_free(object, blockno) == 1) {
			write_bitmap(object, blockno, 0, head, tail);
			return bdesc_alloc(info->ubd, blockno, 0, JOSFS_BLKSIZE);
		}
	}

	return NULL;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_block %d\n", number);
	struct lfs_info * info = (struct lfs_info *) object->instance;

	if (offset != 0 || size != JOSFS_BLKSIZE)
		return NULL;

	return CALL(info->ubd, read_block, number);
}

static fdesc_t * josfs_lookup_name(LFS_t * object, const char * name)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_name %s\n", name);
	char filename[JOSFS_MAXNAMELEN];
	JOSFS_File_t *dir = NULL, *f = NULL;
	bdesc_t * dirblock = NULL;
	int index;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!temp_fdesc)
		return NULL;

	if (walk_path(object, name, &dir, &f, filename, &dirblock, &index) == 0) {
		temp_fdesc->file = f;
		temp_fdesc->dirb = dirblock;
		temp_fdesc->index = index;
		return (fdesc_t *) temp_fdesc;
	}
	free(temp_fdesc);
	return NULL;
}

static void josfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("JOSFSDEBUG: josfs_free_fdesc %x\n", fdesc);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) fdesc;;

	if (f) {
		if (f->file && f->file != &super->s_root)
			free(f->file);
		if (f->dirb)
			bdesc_release(&f->dirb);
		free(f);
	}
}

static uint32_t josfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	Dprintf("JOSFSDEBUG: josfs_get_file_numblocks %s\n", f->file->f_name);
	bdesc_t * indirect;
	uint32_t nblocks = 0;
	int i;
	uint32_t * j;

	for (i = 0; i < JOSFS_NDIRECT; i++) {
		if (f->file->f_direct[i])
			nblocks++;
		else
			break;
	}

	if (f->file->f_indirect) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			for (j = ((uint32_t *) indirect->ddesc->data) + JOSFS_NDIRECT; j < (uint32_t *) (indirect->ddesc->data + JOSFS_BLKSIZE); j++) {
				if (*j)
					nblocks++;
				else
					break;
			}
			bdesc_drop(&indirect);
		}
	}

	Dprintf("JOSFSDEBUG: josfs_get_file_numblocks returns %d\n", nblocks);
	return nblocks;
}

static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * indirect;
	uint32_t blockno, nblocks;

	Dprintf("JOSFSDEBUG: josfs_get_file_block %s, %d\n", f->file->f_name, offset);

	nblocks = josfs_get_file_numblocks(object, file);
	if (offset % JOSFS_BLKSIZE == 0 && offset < nblocks * JOSFS_BLKSIZE) {
		if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
			indirect = CALL(info->ubd, read_block, f->file->f_indirect);
			if (indirect) {
				blockno = ((uint32_t *) indirect->ddesc->data)[offset / JOSFS_BLKSIZE];
				bdesc_drop(&indirect);
				return CALL(info->ubd, read_block, blockno);
			}
		}
		else {
			blockno = f->file->f_direct[offset / JOSFS_BLKSIZE];
			return CALL(info->ubd, read_block, blockno);
		}
	}

	return NULL;
}

static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("JOSFSDEBUG: josfs_get_dirent %x, %d\n", basep, *basep);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock;
	JOSFS_File_t * dirfile;
	int blockno, i;
	uint16_t namelen, reclen;
	int retval = E_NOT_DIR;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type == TYPE_DIR) {
		blockno = *basep * sizeof(JOSFS_File_t) / JOSFS_BLKSIZE;
		dirblock = josfs_get_file_block(object, file, blockno * JOSFS_BLKSIZE);
		if (dirblock) {
			dirfile = (JOSFS_File_t *) dirblock->ddesc->data + (*basep % (JOSFS_BLKSIZE / sizeof(JOSFS_File_t)));

			namelen = strlen(dirfile->f_name);
			namelen = MIN(namelen, sizeof(entry->d_name) - 1);

			// If the name length is 0 (or less?) then we assume it's an empty slot
			if (namelen < 1) {
				entry->d_reclen = 0;
				bdesc_drop(&dirblock);
				*basep += 1;
				return 1;
			}

			reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;

			if (size >= reclen) {
				// Pseudo unique fileno generator
				entry->d_fileno = 0;
				for (i = 0; f->fullpath[i]; i++) {
					entry->d_fileno *= 5;
					entry->d_fileno += f->fullpath[i];
				}

				entry->d_type = dirfile->f_type;
				entry->d_filesize = dirfile->f_size;
				entry->d_reclen = reclen;
				entry->d_namelen = namelen;
				strncpy(entry->d_name, dirfile->f_name, sizeof(entry->d_name));

				bdesc_drop(&dirblock);
				*basep += 1;
				return 0;
			}
			bdesc_drop(&dirblock);
		}
		retval = E_UNSPECIFIED;
	}

	return -retval;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_append_file_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = josfs_get_file_numblocks(object, file);
	bdesc_t * indirect;
	uint32_t * indirect_offset;
	JOSFS_File_t * dirfile;
	int r;

	if (nblocks >= JOSFS_NINDIRECT || nblocks < 0) {
		return -E_INVAL;
	}
	else if (nblocks > JOSFS_NDIRECT) {
		// FIXME chdesc
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			bdesc_touch(indirect);
			indirect_offset = ((uint32_t *) indirect->ddesc->data) + nblocks;
			*indirect_offset = block->number;
			bdesc_drop(&block);
			return CALL(info->ubd, write_block, indirect);
		}
	}
	else if (nblocks == JOSFS_NDIRECT) {
		// FIXME chdesc
		indirect = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, NULL, NULL);
		if (indirect) {
			// FIXME chdesc
			bdesc_touch(f->dirb);
			dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
			dirfile->f_indirect = indirect->number;
			if ((r = CALL(info->ubd, write_block, f->dirb)) < 0) {
				bdesc_drop(&block);
				return r;
			}

			// FIXME chdesc
			bdesc_retain(&indirect);
			bdesc_touch(indirect);
			f->file->f_indirect = indirect->number;
			indirect_offset = ((uint32_t *) indirect->ddesc->data) + nblocks;
			*indirect_offset = block->number;
			bdesc_drop(&block);
			r = CALL(info->ubd, write_block, indirect);
			bdesc_release(&indirect);
			return r;
		}
	}
	else {
		// FIXME chdesc
		bdesc_touch(f->dirb);
		dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
		dirfile->f_direct[nblocks] = block->number;
		if ((r = CALL(info->ubd, write_block, f->dirb)) < 0) {
			bdesc_drop(&block);
			return r;
		}

		f->file->f_direct[nblocks] = block->number;
		bdesc_drop(&block);
		return 0;
	}

	/* fell out of one of the blocks above... */
	bdesc_drop(&block);
	return -E_NO_DISK;
}

static fdesc_t * josfs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	char filename[JOSFS_MAXNAMELEN];
	char pname[JOSFS_MAXNAMELEN];
	JOSFS_File_t *dir = NULL, *f = NULL;
	struct josfs_fdesc * new_fdesc;
	bdesc_t * blk = NULL;
	bdesc_t * dirblock = NULL;
	fdesc_t * pdir_fdesc;
	int i, j, r, index;
	uint32_t nblock;

	if (link)
		return NULL;

	new_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!new_fdesc)
		return NULL;

	strcpy(new_fdesc->fullpath, name);

	if ((r = walk_path(object, name, &dir, &f, filename, &dirblock, &index)) != 0) {
		if (r == -E_NOT_FOUND && dir) {
			get_parent_path(name, pname);
			pdir_fdesc = josfs_lookup_name(object, pname);
			if (pdir_fdesc) {
			// Modified dir_alloc_file() from JOS
				nblock = josfs_get_file_numblocks(object, pdir_fdesc);

				// Search existing blocks for empty spot
				for (i = 0; i < nblock; i++) {
					if ((blk = josfs_get_file_block(object, pdir_fdesc, i*JOSFS_BLKSIZE)) == NULL) {
						josfs_free_fdesc(object, pdir_fdesc);
						free(new_fdesc);
						return NULL;
					}
					f = (JOSFS_File_t *) blk->ddesc->data;
					// Search for an empty slot
					for (j = 0; j < JOSFS_BLKFILES; j++) {
						if (f[j].f_name[0] == '\0') {
							bdesc_touch(blk);
							memset(&f[j], 0, sizeof(JOSFS_File_t));
							strcpy(f[j].f_name, filename);
							f[j].f_type = type;

							// FIXME chdesc
							if ((r = CALL(info->ubd, write_block, blk)) >= 0) {
								new_fdesc->file = malloc(sizeof(JOSFS_File_t));
								memcpy(new_fdesc->file, &f[j], sizeof(JOSFS_File_t));
								new_fdesc->file->f_dir = dir;
								bdesc_retain(&blk);
								new_fdesc->dirb = blk;
								new_fdesc->index = j;
								josfs_free_fdesc(object, pdir_fdesc);
								return (fdesc_t *) new_fdesc;
							}
							else {
								bdesc_drop(&blk);
								free(new_fdesc);
								josfs_free_fdesc(object, pdir_fdesc);
								return NULL;
							}
						}
					}
					bdesc_drop(&blk);
				}
				// No empty slots, gotta allocate a new block
				// FIXME chdesc
				if ((blk = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, NULL, NULL)) != NULL) {
					bdesc_retain(&blk);
					dir->f_size += JOSFS_BLKSIZE;
					// FIXME chdesc
					r = josfs_set_metadata(object, (struct josfs_fdesc *) pdir_fdesc, KFS_feature_size.id, sizeof(uint32_t), &(dir->f_size), NULL, NULL);

					bdesc_touch(blk);
					f = (JOSFS_File_t *) blk->ddesc->data;
					memset(&f[0], 0, sizeof(JOSFS_File_t));
					strcpy(f[0].f_name, filename);
					f[0].f_type = type;

					// FIXME chdesc
					if ((r = josfs_append_file_block(object, pdir_fdesc, blk, NULL, NULL)) >= 0) {
						// FIXME chdesc
						if ((r = CALL(info->ubd, write_block, blk)) >= 0) {
							new_fdesc->file = malloc(sizeof(JOSFS_File_t));
							memcpy(new_fdesc->file, &f[0], sizeof(JOSFS_File_t));
							new_fdesc->file->f_dir = dir;
							new_fdesc->dirb = blk;
							new_fdesc->index = 0;
							josfs_free_fdesc(object, pdir_fdesc);
							return (fdesc_t *) new_fdesc;
						}
					}
					// FIXME chdesc
					josfs_free_block(object, blk, NULL, NULL);
				}
				josfs_free_fdesc(object, pdir_fdesc);
			}
		}
	}
	free(new_fdesc);
	return NULL;
}

static int josfs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_rename\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	fdesc_t * oldfdesc;
	fdesc_t * newfdesc;
	struct josfs_fdesc * old;
	struct josfs_fdesc * new;
	JOSFS_File_t * oldfile;
	JOSFS_File_t * newfile;
	int i, r;

	oldfdesc = josfs_lookup_name(object, oldname);
	if (oldfdesc) {
		old = (struct josfs_fdesc *) oldfdesc;
		// FIXME chdesc
		newfdesc = josfs_allocate_name(object, newname, old->file->f_type, NULL, NULL, NULL);
		if (newfdesc) {
			new = (struct josfs_fdesc *) newfdesc;
			new->file->f_size = old->file->f_size;
			new->file->f_indirect = old->file->f_indirect;
			for (i = 0; i < JOSFS_NDIRECT; i++)
				new->file->f_direct[i] = old->file->f_direct[i];

			bdesc_touch(new->dirb);
			oldfile = ((JOSFS_File_t *) old->dirb->ddesc->data) + old->index;
			newfile = ((JOSFS_File_t *) new->dirb->ddesc->data) + new->index;
			memcpy(newfile, oldfile, sizeof(JOSFS_File_t));
			strcpy(newfile->f_name, new->file->f_name);
			josfs_free_fdesc(object, oldfdesc);
			// FIXME chdesc
			if ((r = CALL(info->ubd, write_block, new->dirb)) < 0) {
				josfs_free_fdesc(object, newfdesc);
				return r;
			}
			josfs_free_fdesc(object, newfdesc);

			// FIXME chdesc
			if (josfs_remove_name(object, oldname, NULL, NULL) == 0)
				return 0;
			else
				josfs_remove_name(object, newname, NULL, NULL);
		}
	}

	return -E_NOT_FOUND;
}

static bdesc_t * josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_truncate_file_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = josfs_get_file_numblocks(object, file);
	bdesc_t * indirect;
	JOSFS_File_t * dirfile;
	uint32_t blockno;
	int r;

	if (nblocks > JOSFS_NINDIRECT || nblocks < 1) {
		return NULL;
	}
	else if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			bdesc_touch(indirect);
			blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
			*((uint32_t *) (indirect->ddesc->data) + nblocks - 1) = 0;
			// FIXME chdesc
			if ((r = CALL(info->ubd, write_block, indirect)) < 0) {
				bdesc_drop(&indirect);
				return NULL;
			}
			return CALL(info->ubd, read_block, blockno);
		}
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
			// FIXME chdesc
			if (josfs_free_block(object, indirect, NULL, NULL) == 0) {
				bdesc_touch(f->dirb);
				dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
				dirfile->f_indirect = 0;
				// FIXME chdesc
				if ((r = CALL(info->ubd, write_block, f->dirb)) < 0) {
					bdesc_drop(&indirect);
					return NULL;
				}

				f->file->f_indirect = 0;
				bdesc_drop(&indirect);
				return CALL(info->ubd, read_block, blockno);
			}
		}
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		bdesc_touch(f->dirb);
		dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
		dirfile->f_direct[nblocks - 1] = 0;
		// FIXME chdesc
		if ((r = CALL(info->ubd, write_block, f->dirb)) < 0)
			return NULL;

		f->file->f_direct[nblocks - 1] = 0;
		return CALL(info->ubd, read_block, blockno);
	}

	return NULL;
}

static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_free_block\n");

	if (!block || block->number == 0)
		return -E_INVAL;
	write_bitmap(object, block->number, 1, head, tail);
	bdesc_drop(&block);
	return 0;
}

static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_remove_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	fdesc_t * file;
	JOSFS_File_t * dirfile;
	struct josfs_fdesc * f;
	int r;

	file = josfs_lookup_name(object, name);

	if (!file)
		return -E_INVAL;

	f = (struct josfs_fdesc *) file;
	bdesc_touch(f->dirb);
	dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
	dirfile->f_name[0] = '\0';
	// FIXME chdesc
	if ((r = CALL(info->ubd, write_block, f->dirb)) < 0)
		return r;

	f->file->f_name[0] = '\0';

	josfs_free_fdesc(object, file);
	return 0;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_write_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	int r;

	if (offset + size > JOSFS_BLKSIZE)
		return -E_INVAL;

	if (head && tail) {
		r = chdesc_create_byte(block, offset, size, data, head, tail);
		if (r < 0) {
			bdesc_drop(&block);
			return r;
		}
	}
	else {
		bdesc_touch(block);
		memcpy(&block->ddesc->data[offset], data, size);
	}

	if ((r = CALL(info->ubd, write_block, block)) < 0) {
		bdesc_drop(&block);
		return r;
	}

	return 0;
}

static size_t josfs_get_num_features(LFS_t * object, const char * name)
{
	return 2;
}

static const feature_t * josfs_get_feature(LFS_t * object, const char * name, size_t num)
{
	Dprintf("JOSFSDEBUG: josfs_get_feature %s\n", name);
	if (num == 0)
		return &KFS_feature_size;
	else if (num == 1)
		return &KFS_feature_filetype;
	return NULL;
}

static int josfs_get_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata\n");
	if (id == KFS_feature_size.id) {
		*data = malloc(sizeof(off_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(off_t);
		Dprintf("JOSFSDEBUG: josfs_get_metadata %s is size %d\n", f->file->f_name, f->file->f_size);
		memcpy(*data, &(f->file->f_size), sizeof(off_t));
	}
	else if (id == KFS_feature_filetype.id) {
		*data = malloc(sizeof(uint32_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(uint32_t);
		Dprintf("JOSFSDEBUG: josfs_get_metadata %s is type %d\n", f->file->f_name, f->file->f_type);
		memcpy(*data, &(f->file->f_type), sizeof(uint32_t));
	}

	return 0;
}

static int josfs_get_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata_name\n");
	int r;
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_name(object, name);
	if (!f)
		return -E_INVAL;
	r = josfs_get_metadata(object, f, id, size, data);
	josfs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int josfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_get_metadata(object, f, id, size, data);
}

static int josfs_set_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_set_metadata %s, %d, %d\n", f->file->f_name, id, size);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	JOSFS_File_t * dirfile;
	int r;

	if (id == KFS_feature_size.id) {
		if (sizeof(off_t) == size) {
			if (*((off_t *) data) >= 0 && *((off_t *) data) < JOSFS_MAXFILESIZE) {
				bdesc_touch(f->dirb);
				dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
				dirfile->f_size = *((off_t *) data);
				// FIXME chdesc
				if ((r = CALL(info->ubd, write_block, f->dirb)) < 0)
					return r;
				f->file->f_size = *((off_t *) data);
				return 0;
			}
		}
	}
	else if (id == KFS_feature_filetype.id) {
		if (sizeof(uint32_t) == size) {
			if (*((uint32_t *) data) == TYPE_FILE || *((uint32_t *) data) == TYPE_DIR) {
				bdesc_touch(f->dirb);
				dirfile = ((JOSFS_File_t *) f->dirb->ddesc->data) + f->index;
				dirfile->f_type = *((uint32_t *) data);
				// FIXME chdesc
				if ((r = CALL(info->ubd, write_block, f->dirb)) < 0)
					return r;
				f->file->f_type = *((uint32_t *) data);
				return 0;
			}
		}
	}

	return -E_INVAL;
}

static int josfs_set_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	int r;
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_name(object, name);
	if (!f)
		return -E_INVAL;
	r = josfs_set_metadata(object, f, id, size, data, head, tail);
	josfs_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int josfs_set_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_set_metadata(object, f, id, size, data, head, tail);
}

static int josfs_sync(LFS_t * object, const char * name)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	fdesc_t * f;
	int i, r, nblocks;

	if(!name || !name[0])
		return CALL(info->ubd, sync, NULL);

	f = josfs_lookup_name(object, name);
	if (!f)
		return -E_INVAL;

	/* FIXME this needs to sync all containing directories as well */
	nblocks = josfs_get_file_numblocks(object, f);
	for (i = 0 ; i < nblocks; i++) {
		if ((r = CALL(info->ubd, sync, josfs_get_file_block(object, f, i * JOSFS_BLKSIZE))) < 0)
			return r;
	}

	return 0;
}

static int josfs_destroy(LFS_t * lfs)
{
	free(lfs->instance);
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	return 0;
}

LFS_t * josfs(BD_t * block_device)
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
	lfs->instance = info;

	ASSIGN(lfs, josfs, get_blocksize);
	ASSIGN(lfs, josfs, get_blockdev);
	ASSIGN(lfs, josfs, allocate_block);
	ASSIGN(lfs, josfs, lookup_block);
	ASSIGN(lfs, josfs, lookup_name);
	ASSIGN(lfs, josfs, free_fdesc);
	ASSIGN(lfs, josfs, get_file_numblocks);
	ASSIGN(lfs, josfs, get_file_block);
	ASSIGN(lfs, josfs, get_dirent);
	ASSIGN(lfs, josfs, append_file_block);
	ASSIGN(lfs, josfs, allocate_name);
	ASSIGN(lfs, josfs, rename);
	ASSIGN(lfs, josfs, truncate_file_block);
	ASSIGN(lfs, josfs, free_block);
	ASSIGN(lfs, josfs, remove_name);
	ASSIGN(lfs, josfs, write_block);
	ASSIGN(lfs, josfs, get_num_features);
	ASSIGN(lfs, josfs, get_feature);
	ASSIGN(lfs, josfs, get_metadata_name);
	ASSIGN(lfs, josfs, get_metadata_fdesc);
	ASSIGN(lfs, josfs, set_metadata_name);
	ASSIGN(lfs, josfs, set_metadata_fdesc);
	ASSIGN(lfs, josfs, sync);
	ASSIGN_DESTROY(lfs, josfs, destroy);

	info->ubd = block_device;
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

	fsck(lfs);

	return lfs;
}

