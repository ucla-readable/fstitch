/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/hash_set.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/depman.h>
#include <kfs/modman.h>
#include <kfs/josfs_base.h>

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

#define block_is_free read_bitmap
#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)
#define isvalid(x) (x >= reserved && x < s_nblocks)

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	bdesc_t * bitmap_cache; // Bitmap mini write through cache!
};

struct josfs_fdesc {
	uint32_t dirb;
	uint32_t index;
	char fullpath[JOSFS_MAXPATHLEN];
	JOSFS_File_t * file;
};

static uint32_t josfs_get_file_block_num(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int josfs_set_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);

/* these two "pair" functions will help when we use CALL and might lose chdescs unless they are weak retained */
static int weak_retain_pair(chdesc_t ** head, chdesc_t ** tail)
{
	int i = 0, j = 0;
	if(head && tail)
	{
		i = chdesc_weak_retain(*head, head);
		j = chdesc_weak_retain(*tail, tail);
	}
	return (i < 0) ? i : j;
}

static void weak_forget_pair(chdesc_t ** head, chdesc_t ** tail)
{
	if(head && tail)
	{
		chdesc_weak_forget(head);
		chdesc_weak_forget(tail);
	}
}

// Equivalent to JOS's read_super
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	uint32_t numblocks;
	int r;

	/* make sure we have the block size we expect */
	if (CALL(info->ubd, get_blocksize) != JOSFS_BLKSIZE) {
		printf("Block device size is not BLKSIZE!\n");
		return -1;
	}

	/* the superblock is in block 1 */
	info->super_block = CALL(info->ubd, read_block, 1);
	if (!info->super_block)
	{
		printf("Unable to read superblock!\n");
		return -1;
	}
	r = bdesc_retain(&info->super_block);
	if (r < 0)
		return r;

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
		DFprintf("block %d is %d\n", j, blist[j]);
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
	DFprintf("%s is %d bytes, %d blocks\n", file.f_name, file.f_size, j);
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

		if (d == -E_NOT_FOUND) {
			r = -2;
			goto fsck_dir_cleanup;
		}

		if (d != 0) {
			free(temp_file);
			i++;
			continue;
		}

		DFprintf("Checking %s\n", entry.d_name);
		blockno = i / JOSFS_BLKFILES;
		dirblock = josfs_get_file_block(object, (fdesc_t *) fdesc, blockno * JOSFS_BLKSIZE);
		if (dirblock) {
			target = (JOSFS_File_t *) dirblock->ddesc->data;
			target += i % JOSFS_BLKFILES;
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
		temp_fdesc.fullpath[0] = 0;

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
	struct lfs_info * info = (struct lfs_info *) object->instance;
	bdesc_t * bdesc;
	int target;
	uint32_t * ptr;
	bool result;

	target = 2 + (blockno / (JOSFS_BLKBITSIZE));

	if (info->bitmap_cache && info->bitmap_cache->number != target) {
		bdesc_release(&info->bitmap_cache);
		info->bitmap_cache = NULL;
	}

	if (! info->bitmap_cache) {
		bdesc = CALL(info->ubd, read_block, target);
		if (!bdesc || bdesc->length != JOSFS_BLKSIZE) {
			printf("josfs_base: trouble reading bitmap!\n");
			if (bdesc)
				bdesc_drop(&bdesc);
			return -1;
		}
		assert(bdesc_retain(&bdesc) >= 0); // TODO: handle error
		info->bitmap_cache = bdesc;
	}

	ptr = ((uint32_t *) info->bitmap_cache->ddesc->data) + ((blockno % JOSFS_BLKBITSIZE) / 32);
	result = *ptr & (1 << (blockno % 32));

	if (result)
		return 1;
	return 0;
}

static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: write_bitmap %d\n", blockno);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	bdesc_t * bdesc;
	int target;
	int r;
	uint32_t * ptr;
	chdesc_t * ch;

	if (blockno == 0) {
		printf("josfs_base: attempted to write to zero block!\n");
		return -1;
	}

	target = 2 + (blockno / JOSFS_BLKBITSIZE);

	if (info->bitmap_cache && info->bitmap_cache->number == target) {
		bdesc = info->bitmap_cache;
	}
	else {
		bdesc_release(&info->bitmap_cache);
		info->bitmap_cache = NULL;
		bdesc = CALL(info->ubd, read_block, target);
	}

	if (!bdesc || bdesc->length != JOSFS_BLKSIZE) {
		printf("josfs_base: trouble reading bitmap!\n");
		if (bdesc)
			bdesc_drop(&bdesc);
		return -1;
	}

	info->bitmap_cache = bdesc;
	bdesc_retain(&bdesc);

	if (head && tail) {
		if (((uint32_t *) bdesc->ddesc->data)[(blockno % JOSFS_BLKBITSIZE) / 32] >> (blockno % 32) == value) {
			/* already has the right value */
			bdesc_drop(&bdesc);
			return 0;
		}
		/* bit chdescs take offset in increments of 32 bits */
		ch = chdesc_create_bit(bdesc, (blockno % JOSFS_BLKBITSIZE) / 32, 1 << (blockno % 32));
		if (!ch) {
			return -1;
		}

		r = depman_add_chdesc(ch);
		assert(r >= 0); // TODO: handle error

		if (*head) {
			if ((r = chdesc_add_depend(ch, *head)) < 0) {
				return r;
			}
		}

		*tail = ch;
		*head = ch;
	}
	else {
		bdesc_touch(bdesc);
		ptr = ((uint32_t *) bdesc->ddesc->data) + ((blockno % JOSFS_BLKBITSIZE) / 32);
		if (value)
			*ptr |= (1 << (blockno % 32));
		else
			*ptr &= ~(1 << (blockno % 32));
	}

	weak_retain_pair(head, tail);
	r = CALL(info->ubd, write_block, bdesc);
	weak_forget_pair(head, tail);

	return r;
}

// Skip over slashes.
static inline const char* skip_slash(const char* p)
{
	while (*p == '/')
		p++;
	return p;
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

	for (i = len - 1; i >= 0; i--) {
		if (parent[i] == '/')
			break;
	}

	while (parent[i] == '/') {
		parent[i] = 0;
		i--;
	}

}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, JOSFS_File_t* dir, const char* name, JOSFS_File_t** file, uint32_t * dirb, int *index)
{
	Dprintf("JOSFSDEBUG: dir_lookup %s\n", name);
	int r, blockno;
	uint32_t i = 0, basep = 0;
	struct dirent entry;
	bdesc_t * dirblock;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	uint8_t * target;

	if (!temp_fdesc) {
		Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM\n");
		return -E_NO_MEM;
	}

	strncpy(temp_fdesc->fullpath, name, JOSFS_MAXPATHLEN - 1);
	temp_fdesc->fullpath[JOSFS_MAXPATHLEN - 1] = 0;
	temp_fdesc->file = dir;
	do {
		r = josfs_get_dirent(object, (fdesc_t *) temp_fdesc, &entry, sizeof(struct dirent), &basep);
		if (r == 0 && strcmp(entry.d_name, name) == 0) {
			blockno = i / JOSFS_BLKFILES;
			*dirb = josfs_get_file_block_num(object, (fdesc_t *) temp_fdesc, blockno * JOSFS_BLKSIZE);
			dirblock = josfs_get_file_block(object, (fdesc_t *) temp_fdesc, blockno * JOSFS_BLKSIZE);
			if (dirblock) {
				*index = (i % JOSFS_BLKFILES) * sizeof(JOSFS_File_t);
				target = (uint8_t *) dirblock->ddesc->data;
				target += *index;
				*file = malloc(sizeof(JOSFS_File_t));
				if (*file) {
					memcpy(*file, target, sizeof(JOSFS_File_t));
					(*file)->f_dir = dir;

					bdesc_drop(&dirblock);
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

// Evaluate a path name, starting at the root.
// On success, set *pfile to the file we found
// and set *pdir to the directory the file is in.
// If we cannot find the file but find the directory
// it should be in, set *pdir and copy the final path
// element into lastelem.
static int walk_path(LFS_t * object, const char* path, JOSFS_File_t** pdir, JOSFS_File_t** pfile, char* lastelem, uint32_t* dirb, int *index)
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

	// Special case of root
	if (path[0] == 0) {
		*pfile = file;
		*dirb = 1;
		*index = (uint32_t) &((struct JOSFS_Super *) NULL)->s_root;
		return 0;
	}

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
	bitmap_size = (s_nblocks + JOSFS_BLKBITSIZE + 1) / JOSFS_BLKBITSIZE;

	for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
		if (block_is_free(object, blockno)) {
			bdesc_t * bdesc;
			write_bitmap(object, blockno, 0, head, tail);
			assert(!block_is_free(object, blockno));
			bdesc = bdesc_alloc(info->ubd, blockno, 0, JOSFS_BLKSIZE);
			/* FIXME maybe use chdescs? */
			memset(bdesc->ddesc->data, 0, JOSFS_BLKSIZE);
			return bdesc;
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
	uint32_t blockno;
	int index;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!temp_fdesc)
		return NULL;

	if (walk_path(object, name, &dir, &f, filename, &blockno, &index) == 0) {
		temp_fdesc->file = f;
		temp_fdesc->dirb = blockno;
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
		free(f);
	}
}

static uint32_t josfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	//Dprintf("JOSFSDEBUG: josfs_get_file_numblocks %s\n", f->file->f_name);
	bdesc_t * indirect;
	uint32_t nblocks = 0;
	int i;

	for (i = 0; i < JOSFS_NDIRECT; i++) {
		if (f->file->f_direct[i])
			nblocks++;
		else
			break;
	}

	// f->file->f_indirect -> i == JOSFS_NDIRECT
	assert(!f->file->f_indirect || i == JOSFS_NDIRECT);

	if (f->file->f_indirect) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			uint32_t * j = (uint32_t *) indirect->ddesc->data;
			for (i = JOSFS_NDIRECT; i < NINDIRECT; i++) {
				if (j[i])
					nblocks++;
				else
					break;
			}
			bdesc_drop(&indirect);
		}
	}

	//Dprintf("JOSFSDEBUG: josfs_get_file_numblocks returns %d\n", nblocks);
	return nblocks;
}

// Offset is a byte offset
static uint32_t josfs_get_file_block_num(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * indirect;
	uint32_t blockno, nblocks;

	//Dprintf("JOSFSDEBUG: josfs_get_file_block_num %s, %d\n", f->file->f_name, offset);

	nblocks = josfs_get_file_numblocks(object, file);
	if (offset % JOSFS_BLKSIZE == 0 && offset < nblocks * JOSFS_BLKSIZE) {
		if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
			indirect = CALL(info->ubd, read_block, f->file->f_indirect);
			if (indirect) {
				blockno = ((uint32_t *) indirect->ddesc->data)[offset / JOSFS_BLKSIZE];
				bdesc_drop(&indirect);
				return blockno;
			}
		}
		else {
			blockno = f->file->f_direct[offset / JOSFS_BLKSIZE];
			return blockno;
		}
	}

	return -1;
}

// Offset is a byte offset
static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t blockno;

	Dprintf("JOSFSDEBUG: josfs_get_file_block %s, %d\n", f->file->f_name, offset);
	blockno = (uint32_t) f; // Shut up compiler

	blockno = josfs_get_file_block_num(object, file, offset);
	if (blockno < 0)
		return NULL;

	return CALL(info->ubd, read_block, blockno);
}

static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("JOSFSDEBUG: josfs_get_dirent %x, %d\n", basep, *basep);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock;
	JOSFS_File_t * dirfile;
	int blockno, i;
	uint16_t namelen, reclen;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	blockno = *basep / JOSFS_BLKFILES;

	if (blockno >= josfs_get_file_numblocks(object, file))
		return -E_UNSPECIFIED;

	dirblock = josfs_get_file_block(object, file, blockno * JOSFS_BLKSIZE);
	if (dirblock) {
		dirfile = (JOSFS_File_t *) dirblock->ddesc->data + (*basep % JOSFS_BLKFILES);

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
		return -E_INVAL;
	}
	return -E_NOT_FOUND;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_append_file_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t num, inum, nblocks = josfs_get_file_numblocks(object, file);
	bdesc_t * indirect = NULL, * dirblock = NULL;
	uint32_t * indirect_offset;
	JOSFS_File_t * dirfile;
	int r, offset;
	chdesc_t *newtail, * curhead, *oldhead = NULL;

	if (head && tail)
		oldhead = *head;

	num = block->number;
	bdesc_drop(&block);

	if (nblocks >= JOSFS_NINDIRECT || nblocks < 0) {
		return -E_INVAL;
	}
	else if (nblocks > JOSFS_NDIRECT) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			r = bdesc_retain(&indirect);
			assert(r >= 0); // TODO: handle error
			if (head && tail) {
				offset = nblocks * sizeof(uint32_t);
				if ((r = chdesc_create_byte(indirect, offset, sizeof(uint32_t), &num, head, tail)) < 0)
					goto append_file_block_failed;

				r = depman_add_chdesc(*head);
				assert(r >= 0); // TODO: handle error

				if (oldhead) {
					if ((chdesc_add_depend(*tail, oldhead)) < 0) {
						goto append_file_block_failed;
					}
				}
			}
			else {
				bdesc_touch(indirect);
				indirect_offset = ((uint32_t *) indirect->ddesc->data) + nblocks;
				*indirect_offset = num;
			}

			weak_retain_pair(head, tail);
			r = CALL(info->ubd, write_block, indirect);
			weak_forget_pair(head, tail);
			bdesc_release(&indirect);

			return r;
		}
	}
	else if (nblocks == JOSFS_NDIRECT) {
		indirect = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, head, tail);
		if (indirect) {
			// Initialize the structure, then point to it
			inum = indirect->number;
			r = bdesc_retain(&indirect); // Can only have one hot potato at a time
			assert(r >= 0); // TODO: handle error

			dirblock = CALL(info->ubd, read_block, f->dirb);
			if (dirblock) {
				if (head && tail) {
					// this head is from josfs_allocate_block() above
					curhead = *head;
					offset = nblocks * sizeof(uint32_t);
					if ((r = chdesc_create_byte(indirect, offset, sizeof(uint32_t), &num, head, &newtail)) < 0)
						goto append_file_block_failed;

					if ((r = chdesc_add_depend(newtail, curhead)) < 0)
						goto append_file_block_failed;

					curhead = *head;
					offset = f->index;
					offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
					if ((r = chdesc_create_byte(dirblock, offset, sizeof(uint32_t), &inum, head, &newtail)) < 0)
						goto append_file_block_failed;

					if ((r = chdesc_add_depend(newtail, curhead)) < 0)
						goto append_file_block_failed;

					// this should add both changes at once, because they are linked
					r = depman_add_chdesc(*head);
					assert(r >= 0); // TODO: handle error
				}
				else {
					bdesc_touch(indirect);
					indirect_offset = ((uint32_t *) indirect->ddesc->data) + nblocks;
					*indirect_offset = num;

					bdesc_touch(dirblock);
					dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
					dirfile->f_indirect = inum;
				}

				weak_retain_pair(head, tail);
				/* FIXME handle the return values better? */
				r = CALL(info->ubd, write_block, indirect);
				r |= CALL(info->ubd, write_block, dirblock);
				weak_forget_pair(head, tail);

				if (r >= 0)
					f->file->f_indirect = inum;

				bdesc_release(&indirect);
				return r;
			}
			bdesc_release(&indirect);
		}
	}
	else {
		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (dirblock) {
			if (head && tail) {
				offset = f->index;
				offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks];
				if ((r = chdesc_create_byte(dirblock, offset, sizeof(uint32_t), &num, head, tail)) < 0)
					goto append_file_block_failed;

				r = depman_add_chdesc(*head);
				assert(r >= 0); // TODO: handle error

				if (oldhead) {
					if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
						goto append_file_block_failed;
				}
			}
			else {
				bdesc_touch(dirblock);
				dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
				dirfile->f_direct[nblocks] = num;
			}

			weak_retain_pair(head, tail);
			r = CALL(info->ubd, write_block, dirblock);
			weak_forget_pair(head, tail);
			if (r < 0)
				return r;

			f->file->f_direct[nblocks] = num;
			return 0;
		}
	}

	/* fell out of one of the blocks above... */
	return -E_NO_DISK;

append_file_block_failed:
	if (indirect)
		bdesc_release(&indirect);
	if (dirblock)
		bdesc_drop(&dirblock);
	return r;
}

static fdesc_t * josfs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	char filename[JOSFS_MAXNAMELEN];
	char pname[JOSFS_MAXNAMELEN];
	JOSFS_File_t *dir = NULL, *f = NULL;
	JOSFS_File_t temp_file;
	struct josfs_fdesc * new_fdesc;
	bdesc_t * blk = NULL;
	fdesc_t * pdir_fdesc;
	int i, j, r, index, offset;
	uint32_t nblock, dirblock;
	chdesc_t * oldhead = NULL, * newtail, * curhead;

	if (link)
		return NULL;

	new_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!new_fdesc)
		return NULL;

	strncpy(new_fdesc->fullpath, name, JOSFS_MAXPATHLEN);
	new_fdesc->fullpath[JOSFS_MAXPATHLEN - 1] = 0;

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
							memset(&temp_file, 0, sizeof(JOSFS_File_t));
							strcpy(temp_file.f_name, filename);
							temp_file.f_type = type;

							if (head && tail) {
								oldhead = *head;
								offset = j * sizeof(JOSFS_File_t);
								if ((r = chdesc_create_byte(blk, offset, sizeof(JOSFS_File_t), &temp_file, head, tail)) < 0) {
									bdesc_drop(&blk);
									free(new_fdesc);
									josfs_free_fdesc(object, pdir_fdesc);
									return NULL;
								}

								r = depman_add_chdesc(*head);
								assert(r >= 0); // TODO: handle error

								if (oldhead) {
									if ((chdesc_add_depend(*tail, oldhead)) < 0) {
										bdesc_drop(&blk);
										free(new_fdesc);
										josfs_free_fdesc(object, pdir_fdesc);
										return NULL;
									}
								}
							}
							else {
								bdesc_touch(blk);
								f = (JOSFS_File_t *) blk->ddesc->data; // reset ptr after bdesc_touch
								memcpy(&f[j], &temp_file, sizeof(JOSFS_File_t));
							}


							// must retain before passing the hot potato...
							r = bdesc_retain(&blk);
							assert(r >= 0);

							weak_retain_pair(head, tail);
							r = CALL(info->ubd, write_block, blk);
							weak_forget_pair(head, tail);

							if (r >= 0) {
								new_fdesc->file = malloc(sizeof(JOSFS_File_t));
								memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
								new_fdesc->file->f_dir = dir;
								new_fdesc->dirb = blk->number;
								new_fdesc->index = j * sizeof(JOSFS_File_t);
								josfs_free_fdesc(object, pdir_fdesc);
								return (fdesc_t *) new_fdesc;
							}
							else {
								bdesc_release(&blk);
								free(new_fdesc);
								josfs_free_fdesc(object, pdir_fdesc);
								return NULL;
							}
						}
					}
					bdesc_drop(&blk);
				}

				// No empty slots, gotta allocate a new block
				if ((blk = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, head, tail)) != NULL) {
					r =bdesc_retain(&blk);
					assert(r >= 0); // TODO: handle error
					dir->f_size += JOSFS_BLKSIZE;
					r = josfs_set_metadata(object, (struct josfs_fdesc *) pdir_fdesc, KFS_feature_size.id, sizeof(uint32_t), &(dir->f_size), head, tail);
					if (r >= 0) {
						memset(&temp_file, 0, sizeof(JOSFS_File_t));
						strcpy(temp_file.f_name, filename);
						temp_file.f_type = type;

						if (head && tail) {
							curhead = *head;
							if ((r = chdesc_create_byte(blk, 0, sizeof(JOSFS_File_t), &temp_file, head, &newtail)) < 0) {
								bdesc_release(&blk);
								josfs_free_fdesc(object, pdir_fdesc);
								free(new_fdesc);
								return NULL;
							}

							if ((r = chdesc_add_depend(newtail, curhead)) < 0) {
								bdesc_release(&blk);
								josfs_free_fdesc(object, pdir_fdesc);
								free(new_fdesc);
								return NULL;
							}

							r = depman_add_chdesc(*head);
							assert(r >= 0); // TODO: handle error
						}
						else {
							bdesc_touch(blk);
							f = (JOSFS_File_t *) blk->ddesc->data;
							memcpy(&f[0], &temp_file, sizeof(JOSFS_File_t));
						}

						weak_retain_pair(head, tail);
						r = CALL(info->ubd, write_block, blk);
						weak_forget_pair(head, tail);

						if (r >= 0) {
							if ((r = josfs_append_file_block(object, pdir_fdesc, blk, head, tail)) >= 0) {
								new_fdesc->file = malloc(sizeof(JOSFS_File_t));
								memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
								new_fdesc->file->f_dir = dir;
								new_fdesc->dirb = blk->number;
								new_fdesc->index = 0;
								josfs_free_fdesc(object, pdir_fdesc);
								return (fdesc_t *) new_fdesc;
							}
						}
					}
					josfs_free_block(object, blk, head, tail);
					bdesc_release(&blk);
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
	JOSFS_File_t temp_file;
	bdesc_t * dirblock = NULL;
	int i, r, offset;
	chdesc_t * oldhead = NULL, * newtail, * curhead;

	if (head && tail)
		oldhead = *head;

	oldfdesc = josfs_lookup_name(object, oldname);
	if (oldfdesc) {
		old = (struct josfs_fdesc *) oldfdesc;
		dirblock = CALL(info->ubd, read_block, old->dirb);
		if (!dirblock) {
			josfs_free_fdesc(object, oldfdesc);
			return -E_INVAL;
		}

		oldfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + old->index);
		memcpy(&temp_file, oldfile, sizeof(JOSFS_File_t));
		josfs_free_fdesc(object, oldfdesc);

		newfdesc = josfs_allocate_name(object, newname, temp_file.f_type, NULL, head, tail);
		if (newfdesc) {
			new = (struct josfs_fdesc *) newfdesc;
			strcpy(temp_file.f_name, new->file->f_name);
			new->file->f_size = temp_file.f_size;
			new->file->f_indirect = temp_file.f_indirect;
			for (i = 0; i < JOSFS_NDIRECT; i++)
				new->file->f_direct[i] = temp_file.f_direct[i];

			dirblock = CALL(info->ubd, read_block, new->dirb);
			if (!dirblock) {
				josfs_free_fdesc(object, newfdesc);
				return -E_INVAL;
			}

			if (head && tail) {
				curhead = *head;
				offset = new->index;
				if ((r = chdesc_create_byte(dirblock, offset, sizeof(JOSFS_File_t), &temp_file, head, &newtail)) < 0) {
					bdesc_drop(&dirblock);
					josfs_free_fdesc(object, newfdesc);
					return r;
				}

				if ((r = chdesc_add_depend(newtail, curhead)) < 0) {
					bdesc_drop(&dirblock);
					josfs_free_fdesc(object, newfdesc);
					return r;
				}

				r = depman_add_chdesc(*head);
				assert(r >= 0); // TODO: handle error
			}
			else {
				bdesc_touch(dirblock);
				newfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + new->index);
				memcpy(newfile, &temp_file, sizeof(JOSFS_File_t));
			}

			weak_retain_pair(head, tail);
			r = CALL(info->ubd, write_block, dirblock);
			weak_forget_pair(head, tail);
			josfs_free_fdesc(object, newfdesc);

			if (r < 0) {
				bdesc_drop(&dirblock);
				return r;
			}

			if (josfs_remove_name(object, oldname, head, tail) == 0)
				return 0;
			else
				josfs_remove_name(object, newname, head, tail);
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
	bdesc_t * indirect = NULL, *dirblock = NULL, * retval = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;
	int r, offset;
	uint32_t data = 0;
	chdesc_t * oldhead = NULL;

	if (head && tail)
		oldhead = *head;

	if (nblocks > JOSFS_NINDIRECT || nblocks < 1) {
		return NULL;
	}
	else if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
			if (head && tail) {
				offset = (nblocks - 1) * sizeof(uint32_t);
				if ((r = chdesc_create_byte(indirect, offset, sizeof(uint32_t), &data, head, tail)) < 0)
					goto truncate_file_block_failed;

				r = depman_add_chdesc(*head);
				assert(r >= 0); // TODO: handle error

				if (oldhead) {
					if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
						goto truncate_file_block_failed;
				}
			}
			else {
				bdesc_touch(indirect);
				*((uint32_t *) (indirect->ddesc->data) + nblocks - 1) = 0;
			}

			weak_retain_pair(head, tail);
			r = CALL(info->ubd, write_block, indirect);
			weak_forget_pair(head, tail);

			if (r < 0)
				goto truncate_file_block_failed;

			weak_retain_pair(head, tail);
			retval = CALL(info->ubd, read_block, blockno);
			weak_forget_pair(head, tail);

			return retval;
		}
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			r = bdesc_retain(&indirect);
			assert(r >= 0); // TODO: handle error
			blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);

			dirblock = CALL(info->ubd, read_block, f->dirb);
			if (dirblock) {
				if (head && tail) {
					offset = f->index;
					offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
					if ((r = chdesc_create_byte(dirblock, offset, sizeof(uint32_t), &data, head, tail)) < 0)
						goto truncate_file_block_failed;

					r = depman_add_chdesc(*head);
					assert(r >= 0); // TODO: handle error

					if (oldhead) {
						if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
							goto truncate_file_block_failed;
					}
				}
				else {
					bdesc_touch(dirblock);
					dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
					dirfile->f_indirect = 0;
				}

				weak_retain_pair(head, tail);
				r = CALL(info->ubd, write_block, dirblock);
				weak_forget_pair(head, tail);

				if (r < 0)
					goto truncate_file_block_failed;

				f->file->f_indirect = 0;
				r = josfs_free_block(object, indirect, head, tail);

				if (r < 0)
					goto truncate_file_block_failed;

				bdesc_release(&indirect);

				weak_retain_pair(head, tail);
				retval = CALL(info->ubd, read_block, blockno);
				weak_forget_pair(head, tail);

				return retval;
			}
			bdesc_release(&indirect);
		}
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (dirblock) {
			if (head && tail) {
				offset = f->index;
				offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks - 1];
				if ((r = chdesc_create_byte(dirblock, offset, sizeof(uint32_t), &data, head, tail)) < 0)
					goto truncate_file_block_failed;

				r = depman_add_chdesc(*head);
				assert(r >= 0); // TODO: handle error

				if (oldhead) {
					if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
						goto truncate_file_block_failed;
				}
			}
			else {
				bdesc_touch(dirblock);
				dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
				dirfile->f_direct[nblocks - 1] = 0;
			}

			weak_retain_pair(head, tail);
			r = CALL(info->ubd, write_block, dirblock);
			weak_forget_pair(head, tail);

			if (r < 0)
				goto truncate_file_block_failed;

			f->file->f_direct[nblocks - 1] = 0;

			weak_retain_pair(head, tail);
			retval = CALL(info->ubd, read_block, blockno);
			weak_forget_pair(head, tail);

			return retval;
		}
	}

	return NULL;

truncate_file_block_failed:
	if (indirect)
		bdesc_release(&indirect);
	if (dirblock)
		bdesc_drop(&dirblock);
	return retval;
}

static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_free_block\n");
	uint32_t number;

	if (!block || block->number == 0)
		return -E_INVAL;
	number = block->number;
	bdesc_drop(&block);
	write_bitmap(object, number, 1, head, tail);
	return 0;
}

static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_remove_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	fdesc_t * file;
	JOSFS_File_t * dirfile;
	bdesc_t * dirblock = NULL;
	struct josfs_fdesc * f;
	int r, offset;
	uint8_t data = 0;
	chdesc_t * oldhead;

	file = josfs_lookup_name(object, name);

	if (!file)
		return -E_INVAL;

	f = (struct josfs_fdesc *) file;

	dirblock = CALL(info->ubd, read_block, f->dirb);
	if (dirblock) {
		if (head && tail) {
			oldhead = *head;
			offset = f->index;
			offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_name[0];
			if ((r = chdesc_create_byte(dirblock, offset, 1, &data, head, tail)) < 0)
				goto remove_name_failed;

			r = depman_add_chdesc(*head);
			assert(r >= 0); // TODO: handle error

			if (oldhead) {
				if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
					goto remove_name_failed;
			}
		}
		else {
			bdesc_touch(dirblock);
			dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
			dirfile->f_name[0] = '\0';
		}

		weak_retain_pair(head, tail);
		r = CALL(info->ubd, write_block, dirblock);
		weak_forget_pair(head, tail);

		if (r < 0)
			goto remove_name_failed;

		f->file->f_name[0] = '\0';

		josfs_free_fdesc(object, file);
		return 0;
	}

	return -E_NO_DISK;

remove_name_failed:
	josfs_free_fdesc(object, file);
	if (dirblock)
		bdesc_drop(&dirblock);
	return r;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_write_block\n");
	struct lfs_info * info = (struct lfs_info *) object->instance;
	int r;
	chdesc_t * oldhead;

	if (offset + size > JOSFS_BLKSIZE)
		return -E_INVAL;

	if (head && tail) {
		oldhead = *head;
		if ((r = chdesc_create_byte(block, offset, size, data, head, tail)) < 0) {
			bdesc_drop(&block);
			return r;
		}
		r = depman_add_chdesc(*head);
		assert(r >= 0); // TODO: handle error

		if (oldhead) {
			if ((r = chdesc_add_depend(*tail, oldhead)) < 0) {
				bdesc_drop(&block);
				return r;
			}
		}
	}
	else {
		bdesc_touch(block);
		memcpy(&block->ddesc->data[offset], data, size);
	}

	weak_retain_pair(head, tail);
	r = CALL(info->ubd, write_block, block);
	weak_forget_pair(head, tail);

	if (r < 0) {
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
	bdesc_t * dirblock = NULL;
	int r, offset;
	chdesc_t * oldhead;

	if (id == KFS_feature_size.id) {
		if (sizeof(off_t) == size) {
			if (*((off_t *) data) >= 0 && *((off_t *) data) < JOSFS_MAXFILESIZE) {
				dirblock = CALL(info->ubd, read_block, f->dirb);
				if (dirblock) {
					if (head && tail) {
						oldhead = *head;
						offset = f->index;
						offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_size;
						if ((r = chdesc_create_byte(dirblock, offset, sizeof(off_t), data, head, tail)) < 0)
							goto set_metadata_failed;

						r = depman_add_chdesc(*head);
						assert(r >= 0); // TODO: handle error

						if (oldhead) {
							if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
								goto set_metadata_failed;
						}
					}
					else {
						bdesc_touch(dirblock);
						dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
						dirfile->f_size = *((off_t *) data);
					}

					weak_retain_pair(head, tail);
					r = CALL(info->ubd, write_block, dirblock);
					weak_forget_pair(head, tail);

					if (r < 0)
						goto set_metadata_failed;

					f->file->f_size = *((off_t *) data);
					return 0;
				}
			}
		}
	}
	else if (id == KFS_feature_filetype.id) {
		if (sizeof(uint32_t) == size) {
			if (*((uint32_t *) data) == TYPE_FILE || *((uint32_t *) data) == TYPE_DIR) {
				dirblock = CALL(info->ubd, read_block, f->dirb);
				if (dirblock) {
					if (head && tail) {
						oldhead = *head;
						offset = f->index;
						offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_type;
						if ((r = chdesc_create_byte(dirblock, offset, sizeof(uint32_t), data, head, tail)) < 0)
							goto set_metadata_failed;

						r = depman_add_chdesc(*head);
						assert(r >= 0); // TODO: handle error

						if (oldhead) {
							if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
								goto set_metadata_failed;
						}
					}
					else {
						bdesc_touch(dirblock);
						dirfile = (JOSFS_File_t *) (((uint8_t *) dirblock->ddesc->data) + f->index);
						dirfile->f_type = *((uint32_t *) data);
					}

					weak_retain_pair(head, tail);
					r = CALL(info->ubd, write_block, dirblock);
					weak_forget_pair(head, tail);

					if (r < 0)
						goto set_metadata_failed;

					f->file->f_type = *((uint32_t *) data);
					return 0;
				}
			}
		}
	}

	return -E_INVAL;

set_metadata_failed:
	if (dirblock)
		bdesc_drop(&dirblock);
	return r;
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
	Dprintf("JOSFSDEBUG: josfs_sync %s\n", name);
	struct lfs_info * info = (struct lfs_info *) object->instance;
	fdesc_t * f;
	int i, r, nblocks;
	char * parent;

	if(!name || !name[0])
		return CALL(info->ubd, sync, NULL);

	f = josfs_lookup_name(object, name);
	if (!f)
		return -E_INVAL;

	nblocks = josfs_get_file_numblocks(object, f);
	for (i = 0 ; i < nblocks; i++) {
		if ((r = CALL(info->ubd, sync, josfs_get_file_block(object, f, i * JOSFS_BLKSIZE))) < 0)
			return r;
	}

	if (strcmp(name, "/") == 0)
		return 0;

	parent = malloc(JOSFS_MAXPATHLEN);
	get_parent_path(name, parent);
	if (strlen(parent) == 0)
		strcpy(parent, "/");
	r = josfs_sync(object, parent);
	free(parent);
	return r;
}

static int josfs_destroy(LFS_t * lfs)
{
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(((struct lfs_info *) lfs->instance)->ubd, lfs);
	
	free(lfs->instance);
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

// do_fsck: input  - fsck() if *do_fsck != 0
//          output - >= 0 if no errors, < 0 if errors
LFS_t * josfs(BD_t * block_device, int * do_fsck)
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
	ASSIGN(lfs, josfs, get_file_block_num);
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

	if (do_fsck)
		if (*do_fsck)
			*do_fsck = fsck(lfs);

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
