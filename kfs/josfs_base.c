/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <lib/types.h>
#include <malloc.h>
#include <string.h>
#include <inc/error.h>
#include <lib/hash_set.h>
#include <lib/stdio.h>
#include <assert.h>

/* textbar, sleep from inc/lib.h */
int sleepj(int32_t jiffies);
int textbar_init(int use_line);
int textbar_close(void);
int textbar_set_progress(int progress, uint8_t color);

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

#define block_is_free read_bitmap
#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)
#define isvalid(x) (x >= reserved && x < s_nblocks)

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	bdesc_t * bitmap_cache; // Bitmap mini write through cache!
	int color;
	int p;
	int m;
};

struct josfs_fdesc {
	uint32_t dirb;
	uint32_t index;
	char fullpath[JOSFS_MAXPATHLEN];
	JOSFS_File_t * file;
};

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number);
static int josfs_free_block(LFS_t * object, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail);
static int josfs_set_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail);

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
	info->super_block = CALL(info->ubd, read_block, 1);
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

	printf("JOS Filesystem size: %d blocks (%dMB)\n", super->s_nblocks, super->s_nblocks / (1024 * 1024 / JOSFS_BLKSIZE));
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
			printf("josfs_base: Free Block Bitmap block %d marked free!\n");
			return -1;
		}
	}

	return 0;
}

static int fsck_file(LFS_t * object, JOSFS_File_t file, int reserved, int8_t *fbmap, int8_t *ubmap, uint32_t *blist)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * indirect;
	const uint32_t  s_nblocks = super->s_nblocks;
	uint32_t * k;
	int j, r = 1;

	// Get block list
	for (j = 0; j < JOSFS_NINDIRECT; j++)
		blist[j] = 0;
	for (j = 0; j < JOSFS_NDIRECT; j++)
		blist[j] = file.f_direct[j];
	if (file.f_indirect != 0) {
		if (isvalid(file.f_indirect)) {
			if (fbmap[file.f_indirect]) {
				printf("indirect block pointing to free block: %s\n", file.f_name);
				r++;
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
				}
				else {
					printf("error reading indirect block!\n");
					return -1;
				}
			}
			else {
				printf("indirect block pointing to used block: %s, %d\n", file.f_name, file.f_indirect);
				r++;
			}
		}
		else {
			printf("invalid indirect block number: %s, %d", file.f_name, file.f_indirect);
			r++;
		}
	}

	// Check block list
	for (j = 0; j < JOSFS_NINDIRECT; j++) {
		DFprintf("block %d is %d\n", j, blist[j]);
		if (blist[j] == 0)
			break;
		else if (isvalid(blist[j]))
			if (fbmap[blist[j]]) {
				printf("file pointing to free block: %s\n", file.f_name);
				r++;
			}
			else if (ubmap[blist[j]])
				ubmap[blist[j]]--;
			else {
				printf("file pointing to used block: %s, %d -> %d\n", file.f_name, j, blist[j]);
				r++;
			}
		else {
			printf("file pointing to invalid block number: %s, %d -> %d\n", file.f_name, j, blist[j]);
			r++;
		}
	}
	DFprintf("%s is %d bytes, %d blocks\n", file.f_name, file.f_size, j);
	if (ROUNDUP32(file.f_size, JOSFS_BLKSIZE) != j*JOSFS_BLKSIZE) {
		printf("Invalid file size: %s, %d bytes, %d blocks\n", file.f_name, file.f_size, j);
		r++;
	}

	if (r > 1)
		return r - 1;
	return 0;
}

static int fsck_dir(LFS_t * object, fdesc_t * f, uint8_t * fbmap, uint8_t * ubmap, uint32_t * blist, hash_set_t * hsdirs)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	const uint32_t s_nblocks = super->s_nblocks;
	const int reserved = 2 + ((s_nblocks + JOSFS_BLKBITSIZE - 1) / JOSFS_BLKBITSIZE);
	struct josfs_fdesc * fdesc = (struct josfs_fdesc *) f;
	int r = 0, s = 0;
	uint32_t basep = 0, i = 0;
	struct dirent entry;
	JOSFS_File_t * temp_file;
	JOSFS_File_t * target;

	for (i = 0; ; i++)
	{
		bdesc_t * dirblock = NULL;
		uint32_t blockno;
		
		temp_file = malloc(sizeof(JOSFS_File_t));
		if (!temp_file) {
			r = -1;
			break;
		}

		r = josfs_get_dirent(object, (fdesc_t *) fdesc, &entry, sizeof(struct dirent), &basep);

		if (r == -E_NOT_FOUND) {
			r = -2;
			break;
		}

		if (r) {
			free(temp_file);
			continue;
		}

		DFprintf("Checking %s\n", entry.d_name);

		if (info->m >= 0 && info->p < 140) {
			(info->p)++;
			textbar_set_progress(info->p, info->color);
			sleepj(5);
		}

		blockno = i / JOSFS_BLKFILES;
		blockno = josfs_get_file_block(object, (fdesc_t *) fdesc, blockno * JOSFS_BLKSIZE);
		if (blockno != INVALID_BLOCK)
			dirblock = josfs_lookup_block(object, blockno);
		if (dirblock) {
			target = (JOSFS_File_t *) dirblock->ddesc->data;
			target += i % JOSFS_BLKFILES;
			memcpy(temp_file, target, sizeof(JOSFS_File_t));

			if ((r = fsck_file(object, *temp_file, reserved, fbmap, ubmap, blist)) < 0) {
				if (r < 0)
					break;
				else
					s += r;
			}

			if (entry.d_type == JOSFS_TYPE_DIR) {
				if ((r = hash_set_insert(hsdirs, temp_file)) < 0) {
					printf("error with hash_set_insert()\n");
					break;
				}
			}
		}
		else {
			printf("error reading from file!\n");
			r = -1;
			break;
		}
	}
	
	if(r < 0)
	{
		if(temp_file)
			free(temp_file);
		return r;
	}

	return (s > 0) ? s : 0;
}

int josfs_fsck(LFS_t * object)
{
	if ((OBJMAGIC(object) != JOSFS_FS_MAGIC))
		return -E_INVAL;
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	const uint32_t s_nblocks = super->s_nblocks;
	int8_t *free_bitmap = malloc(sizeof(int8_t) * s_nblocks);
	int8_t *used_bitmap = malloc(sizeof(int8_t) * s_nblocks);
	uint32_t *blocklist = malloc(sizeof(uint32_t) * JOSFS_NINDIRECT);
	struct josfs_fdesc temp_fdesc;
	JOSFS_File_t rootfile;
	JOSFS_File_t * dirfile = NULL;
	uint32_t j;
	hash_set_t * hsdirs = NULL;
	hash_set_it_t hsitr;
	int reserved = 2 + (s_nblocks / JOSFS_BLKBITSIZE);
	int d = 0, r = 0, errors = 0;
	info->m = -1;
	info->p = 5;
	info->color = 10;

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
		info->m = textbar_init(-1);
		temp_fdesc.file = &rootfile;
		temp_fdesc.fullpath[0] = 0;

		memcpy(&rootfile, &super->s_root, sizeof(JOSFS_File_t));
		fsck_file(object, rootfile, reserved, free_bitmap, used_bitmap, blocklist);

		if (info->m >= 0) {
			textbar_set_progress(info->p, info->color);
			sleepj(5);
		}

		do {
			if (r == 0)
				d = fsck_dir(object, (fdesc_t *) &temp_fdesc, free_bitmap, used_bitmap, blocklist, hsdirs);

			if (dirfile)
				free(dirfile);

			if (d < 0)
				r = d;
			if (d > 0)
				errors += d;

			hash_set_it_init(&hsitr, hsdirs);
			dirfile = (JOSFS_File_t *) hash_set_next(&hsitr);

			if (dirfile) {
				hash_set_erase(hsdirs, dirfile);
				temp_fdesc.file = dirfile;
			}
		}
		while (dirfile);

		info->p = 140;

		if (r >= 0) {
			for (j = 0; j < s_nblocks; j++) {
				if (info->m >= 0) {
					if (info->p < 141 + (j*20/s_nblocks)) {
						(info->p)++;
						textbar_set_progress(info->p, info->color);
						sleepj(5);
					}
				}

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
		r = -E_NO_MEM;
	}

	if (r < 0 || errors) {
		if (info->m >= 0) {
			for (j = 0; j < 5; j++) {
				sleepj(75);
				textbar_set_progress(info->p, 12);
				sleepj(75);
				textbar_set_progress(0, 12);
			}
		}
	}

	textbar_close();
	free(blocklist);
	free(free_bitmap);
	free(used_bitmap);

	if (errors)
		return errors;
	return r;
}

// Return 1 if block is free
static int read_bitmap(LFS_t * object, uint32_t blockno)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	uint32_t * ptr;

	target = 2 + (blockno / (JOSFS_BLKBITSIZE));

	if (info->bitmap_cache && info->bitmap_cache->number != target) {
		bdesc_release(&info->bitmap_cache);
		info->bitmap_cache = NULL;
	}

	if (! info->bitmap_cache) {
		bdesc = CALL(info->ubd, read_block, target);
		if (!bdesc || bdesc->ddesc->length != JOSFS_BLKSIZE) {
			printf("josfs_base: trouble reading bitmap!\n");
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

static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: write_bitmap %d\n", blockno);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	chdesc_t * ch;
	int r;

	if (!head || !tail)
		return -1;

	if (blockno == 0) {
		printf("josfs_base: attempted to write to zero block!\n");
		return -1;
	}

	target = 2 + (blockno / JOSFS_BLKBITSIZE);
	*tail = NULL;

	if (info->bitmap_cache && info->bitmap_cache->number == target) {
		bdesc = info->bitmap_cache;
	}
	else {
		bdesc_release(&info->bitmap_cache);
		info->bitmap_cache = NULL;
		bdesc = CALL(info->ubd, read_block, target);

		if (!bdesc || bdesc->ddesc->length != JOSFS_BLKSIZE) {
			printf("josfs_base: trouble reading bitmap!\n");
			return -1;
		}

		bdesc_retain(bdesc);
		info->bitmap_cache = bdesc;
	}

	if (((uint32_t *) bdesc->ddesc->data)[(blockno % JOSFS_BLKBITSIZE) / 32] >> (blockno % 32) == value) {
		/* already has the right value */
		return 0;
	}
	/* bit chdescs take offset in increments of 32 bits */
	ch = chdesc_create_bit(bdesc, info->ubd, (blockno % JOSFS_BLKBITSIZE) / 32, 1 << (blockno % 32));
	if (!ch)
		return -1;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*tail = ch;
	*head = ch;

	r = CALL(info->ubd, write_block, bdesc);

	return r;
}

int count_free_space(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	const uint32_t s_nblocks = super->s_nblocks;
	int i, count = 0;

	for (i = 0; i < s_nblocks; i++)
		if (read_bitmap(object, i))
			count++;
	return count;
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

	for (i = len - 1; i >= 0; i--)
		if (parent[i] == '/')
			break;

	while (parent[i] == '/')
		parent[i--] = 0;

}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, JOSFS_File_t* dir, const char* name, JOSFS_File_t** file, uint32_t * dirb, int *index)
{
	Dprintf("JOSFSDEBUG: dir_lookup %s\n", name);
	uint32_t i, basep = 0;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	int r = 0;

	if (!temp_fdesc) {
		Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM\n");
		return -E_NO_MEM;
	}

	strncpy(temp_fdesc->fullpath, name, JOSFS_MAXPATHLEN - 1);
	temp_fdesc->fullpath[JOSFS_MAXPATHLEN - 1] = 0;
	temp_fdesc->file = dir;
	for (i = 0; r >= 0; i++)
	{
		struct dirent entry;
		r = josfs_get_dirent(object, (fdesc_t *) temp_fdesc, &entry, sizeof(struct dirent), &basep);
		if (r == 0 && !strcmp(entry.d_name, name)) {
			bdesc_t * dirblock = NULL;
			uint32_t blockno = i / JOSFS_BLKFILES;
			*dirb = josfs_get_file_block(object, (fdesc_t *) temp_fdesc, blockno * JOSFS_BLKSIZE);
			if (*dirb != INVALID_BLOCK)
				dirblock = josfs_lookup_block(object, *dirb);
			if (dirblock) {
				uint8_t * target = (uint8_t *) dirblock->ddesc->data;
				*index = (i % JOSFS_BLKFILES) * sizeof(JOSFS_File_t);
				target += *index;
				*file = malloc(sizeof(JOSFS_File_t));
				if (*file) {
					memcpy(*file, target, sizeof(JOSFS_File_t));
					(*file)->f_dir = dir;

					free(temp_fdesc);
					Dprintf("JOSFSDEBUG: dir_lookup done: FOUND\n");
					return 0;
				}
				else {
					free(temp_fdesc);
					Dprintf("JOSFSDEBUG: dir_lookup done: NO MEM2\n");
					return -E_NO_MEM;
				}
			}
		}
	}

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
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
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

		if (dir->f_type != JOSFS_TYPE_DIR)
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

static int josfs_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int josfs_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != JOSFS_FS_MAGIC)
		return -E_INVAL;
	
	snprintf(string, length, "");
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

// purpose parameter is ignored
static uint32_t josfs_allocate_block(LFS_t * object, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t blockno, bitmap_size, s_nblocks;
	bool synthetic = 0;
	chdesc_t *newtail, *curhead;
	int r;

	if (!head || !tail)
		return INVALID_BLOCK;

	*tail = NULL;

	s_nblocks = super->s_nblocks;
	bitmap_size = (s_nblocks + JOSFS_BLKBITSIZE + 1) / JOSFS_BLKBITSIZE;

	for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
		if (block_is_free(object, blockno)) {
			bdesc_t * bdesc;

			r = write_bitmap(object, blockno, 0, head, tail);
			if (r < 0)
				return INVALID_BLOCK;

			assert(!block_is_free(object, blockno));
			bdesc = CALL(info->ubd, synthetic_read_block, blockno, &synthetic);
			if (!bdesc)
			{
				r = write_bitmap(object, blockno, 1, head, tail);
				assert(r >= 0);
				return INVALID_BLOCK;
			}

			// FIXME error checks
			curhead = *head;
			r = chdesc_create_init(bdesc, info->ubd, head, &newtail);
			r |= chdesc_add_depend(newtail, curhead);

			r |= CALL(info->ubd, write_block, bdesc);
			assert(r >= 0);
			return blockno;
		}
	}

	return INVALID_BLOCK;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_lookup_block %d\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, read_block, number);
}

static bdesc_t * josfs_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	Dprintf("JOSFSDEBUG: josfs_synthetic_lookup_block %d\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, synthetic_read_block, number, synthetic);
}

static int josfs_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	Dprintf("JOSFSDEBUG: josfs_cancel_synthetic_block %d\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, cancel_block, number);
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
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) fdesc;;

	if (f) {
		if (f->file && f->file != &super->s_root)
			free(f->file);
		free(f);
	}
}

static uint32_t josfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	//Dprintf("JOSFSDEBUG: josfs_get_file_numblocks %s\n", f->file->f_name);
	bdesc_t * indirect;
	uint32_t nblocks = 0;
	int i;

	for (i = 0; i < JOSFS_NDIRECT; i++) {
		if (!f->file->f_direct[i])
			break;
		nblocks++;
	}

	// f->file->f_indirect -> i == JOSFS_NDIRECT
	assert(!f->file->f_indirect || i == JOSFS_NDIRECT);

	if (f->file->f_indirect) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect) {
			uint32_t * j = (uint32_t *) indirect->ddesc->data;
			for (i = JOSFS_NDIRECT; i < JOSFS_NINDIRECT; i++) {
				if (!j[i])
					break;
				nblocks++;
			}
		}
	}

	//Dprintf("JOSFSDEBUG: josfs_get_file_numblocks returns %d\n", nblocks);
	return nblocks;
}

// Offset is a byte offset
static uint32_t josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * indirect;
	uint32_t blockno, nblocks;

	//Dprintf("JOSFSDEBUG: josfs_get_file_block_num %s, %d\n", f->file->f_name, offset);

	nblocks = josfs_get_file_numblocks(object, file);
	if (offset % JOSFS_BLKSIZE != 0 || offset >= nblocks * JOSFS_BLKSIZE)
		return INVALID_BLOCK;

	if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (!indirect)
			return INVALID_BLOCK;
		blockno = ((uint32_t *) indirect->ddesc->data)[offset / JOSFS_BLKSIZE];
	}
	else
		blockno = f->file->f_direct[offset / JOSFS_BLKSIZE];
	return blockno;
}

static int josfs_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("JOSFSDEBUG: josfs_get_dirent %x, %d\n", basep, *basep);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock = NULL;
	JOSFS_File_t * dirfile;
	uint32_t blockno;
	uint16_t namelen, reclen;
	int i;

	// Make sure it's a directory and we can read from it
	if (f->file->f_type != JOSFS_TYPE_DIR)
		return -E_NOT_DIR;

	blockno = *basep / JOSFS_BLKFILES;

	if (blockno >= josfs_get_file_numblocks(object, file))
		return -E_UNSPECIFIED;

	blockno = josfs_get_file_block(object, file, blockno * JOSFS_BLKSIZE);
	if (blockno != INVALID_BLOCK)
		dirblock = josfs_lookup_block(object, blockno);
	if (!dirblock)
		return - E_NOT_FOUND;
	dirfile = (JOSFS_File_t *) dirblock->ddesc->data + (*basep % JOSFS_BLKFILES);

	namelen = strlen(dirfile->f_name);
	namelen = MIN(namelen, sizeof(entry->d_name) - 1);

	// If the name length is 0 (or less?) then we assume it's an empty slot
	if (namelen < 1) {
		entry->d_reclen = 0;
		*basep += 1;
		return 1;
	}

	reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;

	if (size < reclen)
		return -E_INVAL;

	// Pseudo unique fileno generator
	entry->d_fileno = 0;
	for (i = 0; f->fullpath[i]; i++) {
		entry->d_fileno *= 5;
		entry->d_fileno += f->fullpath[i];
	}

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
	strncpy(entry->d_name, dirfile->f_name, sizeof(entry->d_name));

	*basep += 1;
	return 0;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_append_file_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = josfs_get_file_numblocks(object, file);
	bdesc_t * indirect = NULL, * dirblock = NULL;
	int r, offset;
	chdesc_t *newtail, * curhead, *oldhead = NULL;

	if (!head || !tail || nblocks >= JOSFS_NINDIRECT || nblocks < 0)
		return -E_INVAL;

	oldhead = *head;
	*tail = NULL;

	if (nblocks > JOSFS_NDIRECT) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (!indirect)
			return -E_NO_DISK;

		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head, tail)) < 0)
			return r;

		if (oldhead)
			if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
				return r;

		return CALL(info->ubd, write_block, indirect);
	}
	else if (nblocks == JOSFS_NDIRECT) {
		uint32_t inumber = josfs_allocate_block(object, 0, head, tail);
		bdesc_t * indirect;
		if (inumber == INVALID_BLOCK)
			return -E_NO_DISK;
		indirect = josfs_lookup_block(object, inumber);

		// Initialize the structure, then point to it
		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return -E_NO_DISK;

		// this head is from josfs_allocate_block() above
		curhead = *head;
		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head, &newtail)) < 0)
			return r;

		if ((r = chdesc_add_depend(newtail, curhead)) < 0)
			return r;

		curhead = *head;
		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &inumber, head, &newtail)) < 0)
			return r;

		if ((r = chdesc_add_depend(newtail, curhead)) < 0)
			return r;

		/* FIXME handle the return values better? */
		r = CALL(info->ubd, write_block, indirect);
		r |= CALL(info->ubd, write_block, dirblock);

		if (r >= 0)
			f->file->f_indirect = inumber;

		return r;
	}
	else {
		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return -E_NO_DISK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &block, head, tail)) < 0)
			return r;

		if (oldhead)
			if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
				return r;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_direct[nblocks] = block;
		return 0;
	}
}

static fdesc_t * josfs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_allocate_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	char filename[JOSFS_MAXNAMELEN];
	char pname[JOSFS_MAXNAMELEN];
	JOSFS_File_t *dir = NULL, *f = NULL;
	JOSFS_File_t temp_file;
	struct josfs_fdesc * new_fdesc;
	bdesc_t * blk = NULL;
	fdesc_t * pdir_fdesc;
	int i, r, index;
	uint16_t offset;
	uint32_t nblock, dirblock, number;
	chdesc_t * oldhead = NULL, * newtail, * curhead;

	if (!head || !tail || link)
		return NULL;

	*tail = NULL;

	new_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (!new_fdesc)
		return NULL;

	strncpy(new_fdesc->fullpath, name, JOSFS_MAXPATHLEN);
	new_fdesc->fullpath[JOSFS_MAXPATHLEN - 1] = 0;

	if (walk_path(object, name, &dir, &f, filename, &dirblock, &index) != -E_NOT_FOUND)
		goto allocate_name_exit;

	if (!dir)
		goto allocate_name_exit;

	get_parent_path(name, pname);
	pdir_fdesc = josfs_lookup_name(object, pname);
	if (!pdir_fdesc)
		goto allocate_name_exit;

	// Modified dir_alloc_file() from JOS
	nblock = josfs_get_file_numblocks(object, pdir_fdesc);

	// Search existing blocks for empty spot
	for (i = 0; i < nblock; i++) {
		int j;
		number = josfs_get_file_block(object, pdir_fdesc, i * JOSFS_BLKSIZE);
		if (number != INVALID_BLOCK)
			blk = josfs_lookup_block(object, number);
		if (!blk) {
			josfs_free_fdesc(object, pdir_fdesc);
			free(new_fdesc);
			return NULL;
		}
		f = (JOSFS_File_t *) blk->ddesc->data;
		// Search for an empty slot
		for (j = 0; j < JOSFS_BLKFILES; j++) {
			if (!f[j].f_name[0]) {
				memset(&temp_file, 0, sizeof(JOSFS_File_t));
				strcpy(temp_file.f_name, filename);
				temp_file.f_type = type;

				oldhead = *head;
				offset = j * sizeof(JOSFS_File_t);
				if ((r = chdesc_create_byte(blk, info->ubd, offset, sizeof(JOSFS_File_t), &temp_file, head, tail)) < 0) {
					free(new_fdesc);
					josfs_free_fdesc(object, pdir_fdesc);
					return NULL;
				}

				if (oldhead) {
					if ((chdesc_add_depend(*tail, oldhead)) < 0) {
						free(new_fdesc);
						josfs_free_fdesc(object, pdir_fdesc);
						return NULL;
					}
				}

				r = CALL(info->ubd, write_block, blk);

				if (r < 0) {
					free(new_fdesc);
					josfs_free_fdesc(object, pdir_fdesc);
					return NULL;
				}
				new_fdesc->file = malloc(sizeof(JOSFS_File_t));
				memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
				new_fdesc->file->f_dir = dir;
				new_fdesc->dirb = blk->number;
				new_fdesc->index = j * sizeof(JOSFS_File_t);
				josfs_free_fdesc(object, pdir_fdesc);
				return (fdesc_t *) new_fdesc;
			}
		}
		blk = NULL;
	}

	// No empty slots, gotta allocate a new block
	number = josfs_allocate_block(object, 0, head, tail);
	if (number != INVALID_BLOCK)
		blk = josfs_lookup_block(object, number);
	if (!blk)
		goto allocate_name_exit2;

	dir->f_size += JOSFS_BLKSIZE;
	r = josfs_set_metadata(object, (struct josfs_fdesc *) pdir_fdesc, KFS_feature_size.id, sizeof(uint32_t), &(dir->f_size), head, tail);
	if (r < 0) {
		josfs_free_block(object, number, head, tail);
		goto allocate_name_exit2;
	}

	memset(&temp_file, 0, sizeof(JOSFS_File_t));
	strcpy(temp_file.f_name, filename);
	temp_file.f_type = type;

	curhead = *head;
	if ((r = chdesc_create_byte(blk, info->ubd, 0, sizeof(JOSFS_File_t), &temp_file, head, &newtail)) < 0)
		goto allocate_name_exit2;

	if ((r = chdesc_add_depend(newtail, curhead)) < 0)
		goto allocate_name_exit2;

	r = CALL(info->ubd, write_block, blk);

	if (r < 0)
		goto allocate_name_exit2;
		
	if (josfs_append_file_block(object, pdir_fdesc, number, head, tail) >= 0) {
		new_fdesc->file = malloc(sizeof(JOSFS_File_t));
		memcpy(new_fdesc->file, &temp_file, sizeof(JOSFS_File_t));
		new_fdesc->file->f_dir = dir;
		new_fdesc->dirb = blk->number;
		new_fdesc->index = 0;
		josfs_free_fdesc(object, pdir_fdesc);
		return (fdesc_t *) new_fdesc;
	}

allocate_name_exit2:
	josfs_free_fdesc(object, pdir_fdesc);
allocate_name_exit:
	free(new_fdesc);
	return NULL;
}

static int josfs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
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
	chdesc_t * oldhead = NULL, * newtail, * curhead;

	if (!head || !tail)
		return -E_INVAL;

	oldhead = *head;
	*tail = NULL;

	oldfdesc = josfs_lookup_name(object, oldname);
	if (!oldfdesc)
		return -E_NOT_FOUND;

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
	if (!newfdesc)
		return -E_FILE_EXISTS;

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

	curhead = *head;
	offset = new->index;
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(JOSFS_File_t), &temp_file, head, &newtail)) < 0) {
		josfs_free_fdesc(object, newfdesc);
		return r;
	}

	if ((r = chdesc_add_depend(newtail, curhead)) < 0) {
		josfs_free_fdesc(object, newfdesc);
		return r;
	}

	r = CALL(info->ubd, write_block, dirblock);
	josfs_free_fdesc(object, newfdesc);

	if (r < 0)
		return r;

	if (josfs_remove_name(object, oldname, head, tail) < 0)
		return josfs_remove_name(object, newname, head, tail);

	return 0;
}

static uint32_t josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_truncate_file_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = josfs_get_file_numblocks(object, file);
	bdesc_t * indirect = NULL, *dirblock = NULL;
	uint32_t blockno, data = 0;
	chdesc_t * oldhead = NULL;
	uint16_t offset;
	int r;

	if (!head || !tail || nblocks > JOSFS_NINDIRECT || nblocks < 1)
		return INVALID_BLOCK;

	oldhead = *head;
	*tail = NULL;

	if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);
		offset = (nblocks - 1) * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &data, head, tail)) < 0)
			return INVALID_BLOCK;

		if (oldhead)
			if (chdesc_add_depend(*tail, oldhead) < 0)
				return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, indirect);

		return blockno;
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);

		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head, tail)) < 0)
			return INVALID_BLOCK;

		if (oldhead)
			if (chdesc_add_depend(*tail, oldhead) < 0)
				return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_indirect = 0;
		r = josfs_free_block(object, indirect->number, head, tail);

		return blockno;
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_direct[nblocks - 1];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head, tail)) < 0)
			return INVALID_BLOCK;

		if (oldhead)
			if (chdesc_add_depend(*tail, oldhead) < 0)
				return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_direct[nblocks - 1] = 0;

		return blockno;
	}
}

static int josfs_free_block(LFS_t * object, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_free_block\n");
	return write_bitmap(object, block, 1, head, tail);
}

static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_remove_name %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * file;
	bdesc_t * dirblock = NULL;
	struct josfs_fdesc * f;
	int r, offset;
	uint8_t data = 0;
	chdesc_t * oldhead;

	if (!head || !tail)
		return -E_INVAL;

	file = josfs_lookup_name(object, name);
	if (!file)
		return -E_INVAL;

	f = (struct josfs_fdesc *) file;
	*tail = NULL;

	dirblock = CALL(info->ubd, read_block, f->dirb);
	if (!dirblock) {
		r = -E_NO_DISK;
		goto remove_name_exit;
	}

	oldhead = *head;
	offset = f->index;
	offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_name[0];
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, 1, &data, head, tail)) < 0)
		goto remove_name_exit;

	if (oldhead)
		if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
			goto remove_name_exit;

	r = CALL(info->ubd, write_block, dirblock);

	if (r >= 0)
		f->file->f_name[0] = '\0';

remove_name_exit:
	josfs_free_fdesc(object, file);
	return r;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	Dprintf("JOSFSDEBUG: josfs_write_block\n");
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (!head || !tail)
		return -E_INVAL;

	*tail = NULL;

	/* XXX: with blockman, I don't think this can happen anymore... */
	if (info->bitmap_cache && info->bitmap_cache->number == block->number)
		bdesc_release(&info->bitmap_cache);

	return CALL(info->ubd, write_block, block);
}

static const feature_t * josfs_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_filetype, &KFS_feature_file_lfs, &KFS_feature_file_lfs_name};

static size_t josfs_get_num_features(LFS_t * object, const char * name)
{
	return sizeof(josfs_features) / sizeof(josfs_features[0]);
}

static const feature_t * josfs_get_feature(LFS_t * object, const char * name, size_t num)
{
	if(num < 0 || num >= sizeof(josfs_features) / sizeof(josfs_features[0]))
		return NULL;
	return josfs_features[num];
}

static int josfs_get_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata\n");
	if (id == KFS_feature_size.id) {
		*data = malloc(sizeof(off_t));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(off_t);
		memcpy(*data, &(f->file->f_size), sizeof(off_t));
	}
	else if (id == KFS_feature_filetype.id) {
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
		int free_space;
		*data = malloc(sizeof(int));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(int);
		free_space = count_free_space(object) * JOSFS_BLKSIZE / 1024;
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

static int josfs_get_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("JOSFSDEBUG: josfs_get_metadata_name\n");
	int r;
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_name(object, name);
	if (!f)
		return -E_NOT_FOUND;

	if (id == KFS_feature_file_lfs_name.id) {
		// Implement KFS_feature_file_lfs_name here because we need name
		*data = strdup(name);
		if (!*data) {
			r = -E_NO_MEM;
			goto josfs_get_metadata_name_exit;
		}

		*size = strlen(*data);
		r = 0;
	}
	else
		r = josfs_get_metadata(object, f, id, size, data);

josfs_get_metadata_name_exit:
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
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * dirblock = NULL;
	int r, offset;
	chdesc_t * oldhead;

	if (!head || !tail)
		return -E_INVAL;

	*tail = NULL;

	if (id == KFS_feature_size.id) {
		if (sizeof(off_t) != size || *((off_t *) data) < 0 || *((off_t *) data) >= JOSFS_MAXFILESIZE)
			return -E_INVAL;

		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return -E_INVAL;

		oldhead = *head;
		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_size;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(off_t), data, head, tail)) < 0)
			return r;

		if (oldhead)
			if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
				return r;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return r;

		f->file->f_size = *((off_t *) data);
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

		dirblock = CALL(info->ubd, read_block, f->dirb);
		if (!dirblock)
			return -E_INVAL;

		oldhead = *head;
		offset = f->index;
		offset += (uint32_t) &((JOSFS_File_t *) NULL)->f_type;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &fs_type, head, tail)) < 0)
			return r;

		if (oldhead)
			if ((r = chdesc_add_depend(*tail, oldhead)) < 0)
				return r;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return r;

		f->file->f_type = fs_type;
		return 0;
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
	Dprintf("JOSFSDEBUG: josfs_sync %s\n", name);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * f;
	int i, r, nblocks;
	char * parent;

	if(!name || !name[0])
		return CALL(info->ubd, sync, SYNC_FULL_DEVICE, NULL);

	f = josfs_lookup_name(object, name);
	if (!f)
		return -E_NOT_FOUND;

	nblocks = josfs_get_file_numblocks(object, f);
	for (i = 0 ; i < nblocks; i++)
		if ((r = CALL(info->ubd, sync, josfs_get_file_block(object, f, i * JOSFS_BLKSIZE), NULL)) < 0)
			return r;

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
