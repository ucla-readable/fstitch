/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/error.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/josfs_base.h>

#define block_is_free read_bitmap

int read_bitmap(LFS_t * object, uint32_t blockno);
int write_bitmap(LFS_t * object, uint32_t blockno, bool value);
int dir_lookup(LFS_t * object, struct JOSFS_File* dir, const char* name, struct JOSFS_File** file);

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
};

struct josfs_fdesc {
//	bdesc_t * dirb;
//	int index;
	struct JOSFS_File * file;
};

#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)

// Equivalent to JOS's read_super
int check_super(LFS_t * object)
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

	if (super->s_nblocks > numblocks) {
		printf("josfs_base: file system is too large\n");
		bdesc_release(&info->super_block);
		return -1;
	}

	return 0;
}

// Equivalent to JOS's read_bitmap
int check_bitmap(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	int i, blocks_to_read;

	blocks_to_read = super->s_nblocks / JOSFS_BLKBITSIZE;

	if (super->s_nblocks % JOSFS_BLKBITSIZE) {
		blocks_to_read++;
	}

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

// Return 1 if block is free
int read_bitmap(LFS_t * object, uint32_t blockno)
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

int write_bitmap(LFS_t * object, uint32_t blockno, bool value)
{
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

	ptr = ((uint32_t *) bdesc->ddesc->data) + (blockno / 32);
	bdesc_touch(bdesc);
	if (value)
		*ptr |= (1 << (blockno % 32));
	else
		*ptr &= ~(1 << (blockno % 32));

	if ((r = CALL(info->ubd, write_block, bdesc)) < 0)
		return r;

	return 0;
}

// Skip over slashes.
inline const char* skip_slash(const char* p)
{
	while (*p == '/')
		p++;
	return p;
}

// Evaluate a path name, starting at the root.
// On success, set *pfile to the file we found
// and set *pdir to the directory the file is in.
// If we cannot find the file but find the directory
// it should be in, set *pdir and copy the final path
// element into lastelem.
int walk_path(LFS_t * object, const char* path, struct JOSFS_File** pdir, struct JOSFS_File** pfile, char* lastelem)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	const char* p;
	char name[JOSFS_MAXNAMELEN];
	struct JOSFS_File *dir, *file;
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

		if ((r = dir_lookup(object, dir, name, &file)) < 0) {
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

//FIXME bdesc
// Try to find a file named "name" in dir.  If so, set *file to it.
int dir_lookup(LFS_t * object, struct JOSFS_File* dir, const char* name, struct JOSFS_File** file)
{
	int r;
	uint32_t i = 0;
	struct dirent entry;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	bdesc_t * dirblock;
	int blockno;

	temp_fdesc->file = dir;
	do {
		r = josfs_get_dirent(object, (fdesc_t *) temp_fdesc, i, &entry, sizeof(struct dirent), NULL);
		if (r == 0 && strcmp(entry.d_name, name) == 0) {
			blockno = i * sizeof(struct JOSFS_File) / JOSFS_BLKSIZE;
			dirblock = josfs_get_file_block(object, (fdesc_t *) temp_fdesc, blockno * JOSFS_BLKSIZE);
			if (dirblock != NULL) {
				*file = (struct JOSFS_File *) dirblock->ddesc->data + (i % (JOSFS_BLKSIZE / sizeof(struct JOSFS_File)));
				(*file)->f_dir = dir;
				free(temp_fdesc);
				return 0;
			}
		}
		i++;
	} while (r == 0);

	free(temp_fdesc);

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

// FIXME what to do with purpose parameter?
static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	int blockno;
	int bitmap_size;
	int s_nblocks;

	s_nblocks = super->s_nblocks;
	bitmap_size = s_nblocks / JOSFS_BLKBITSIZE;

	if (s_nblocks % JOSFS_BLKBITSIZE) {
		bitmap_size++;
	}

	for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
		if (block_is_free(object, blockno) == 1) {
			write_bitmap(object, blockno, 0);
			return bdesc_alloc(info->ubd, blockno, 0, JOSFS_BLKSIZE);
		}
	}

	return NULL;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
	if (offset != 0 || size != JOSFS_BLKSIZE)
		return NULL;
	
	return bdesc_alloc(((struct lfs_info *) object->instance)->ubd, number, 0, JOSFS_BLKSIZE);
}

static fdesc_t * josfs_lookup_name(LFS_t * object, const char * name)
{
	char filename[JOSFS_MAXNAMELEN];
	struct JOSFS_File *dir, *f;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	if (walk_path(object, name, &dir, &f, filename) == 0) {
		temp_fdesc->file = f;
		return (fdesc_t *) temp_fdesc;
	}
	return NULL;
}

// TODO
static void josfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
}

static uint32_t josfs_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	int nblocks;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	nblocks = f->file->f_size / JOSFS_BLKSIZE;

	if (f->file->f_size % JOSFS_BLKSIZE) {
		nblocks++;
	}

	return nblocks;
}

static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * indirect;
	uint32_t * indirect_offset;
	uint32_t blockno;

	if (offset % JOSFS_BLKSIZE == 0 && offset < f->file->f_size) {
		if (offset >= JOSFS_NDIRECT * JOSFS_BLKSIZE) {
			indirect = CALL(info->ubd, read_block, f->file->f_indirect);
			if (indirect != NULL) {
				indirect_offset = (uint32_t *) (indirect->ddesc->data + (offset / JOSFS_BLKSIZE));
				blockno = *indirect_offset;
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

static int josfs_get_dirent(LFS_t * object, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	bdesc_t * dirblock;
	struct JOSFS_File * dirfile;
	int blockno;

	if (f->file->f_type == TYPE_DIR) {
		blockno = index * sizeof(struct JOSFS_File) / JOSFS_BLKSIZE;
		dirblock = josfs_get_file_block(object, file, blockno * JOSFS_BLKSIZE);
		if (dirblock != NULL) {
			dirfile = (struct JOSFS_File *) dirblock->ddesc->data + (index % (JOSFS_BLKSIZE / sizeof(struct JOSFS_File)));

			if (size >= 8 + strlen(dirfile->f_name) + 1) {
				// FIXME ???
				entry->d_fileno = 0;
				entry->d_type = 0;
				entry->d_reclen = 8 + strlen(dirfile->f_name) + 1;
				entry->d_namelen = strlen(dirfile->f_name);
				strcpy(entry->d_name, dirfile->f_name);

				bdesc_drop(&dirblock);
				return 0;
			}
		}
	}

	return -1;
}

// FIXME bdesc
static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = (f->file->f_size / JOSFS_BLKSIZE);
	bdesc_t * indirect;
	uint32_t * indirect_offset;
	
	if (f->file->f_size % JOSFS_BLKSIZE) {
		nblocks++;
	}

	if (nblocks > JOSFS_NINDIRECT) {
		return -1;
	}
	else if (nblocks > JOSFS_NDIRECT) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect != NULL) {
			indirect_offset = (uint32_t *) (indirect->ddesc->data + nblocks);
			*indirect_offset = block->number;
			return CALL(info->ubd, write_block, indirect);
		}
	}
	else if (nblocks == JOSFS_NDIRECT) {
		indirect = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, NULL, NULL);
		if (indirect != NULL) {
			// FIXME write to disk
			f->file->f_indirect = indirect->number;
			indirect_offset = (uint32_t *) (indirect->ddesc->data + nblocks);
			*indirect_offset = block->number;
			return CALL(info->ubd, write_block, indirect);
		}
	}
	else {
		// FIXME write to disk
		f->file->f_direct[nblocks] = block->number;
		return 0;
	}

	return -1;
}

//FIXME bdesc
//FIXME what to do with link parameter?
static fdesc_t * josfs_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	char filename[JOSFS_MAXNAMELEN];
	struct JOSFS_File *dir, *f;
	struct josfs_fdesc * temp_fdesc = malloc(sizeof(struct josfs_fdesc));
	bdesc_t * blk;
	int i, j, r, nblock;

	if ((r = walk_path(object, name, &dir, &f, filename)) != 0) {
		if (r == -E_NOT_FOUND && dir != NULL) {
			// Modified dir_alloc_file() from JOS
			assert((dir->f_size % JOSFS_BLKSIZE) == 0);
			nblock = dir->f_size / JOSFS_BLKSIZE;
			// Search existing blocks for empty spot
			for (i = 0; i < nblock; i++) {
				if ((blk = josfs_get_file_block(object, (fdesc_t *) dir, i)) == NULL) {
					return NULL;
				}
				f = (struct JOSFS_File *) blk->ddesc->data;
				// Search for an empty slot
				// FIXME write to disk
				for (j = 0; j < JOSFS_BLKFILES; j++) {
					if (f[j].f_name[0] == '\0') {
						temp_fdesc->file = &f[j];
						memset(temp_fdesc->file, 0, sizeof(struct JOSFS_File));
						strcpy(f[j].f_name, filename);
						f[j].f_type = type;
						temp_fdesc->file->f_dir = dir;
						return (fdesc_t *) temp_fdesc;
					}
				}
			}
			// No empty slots, gotta allocate a new block
			if ((blk = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, NULL, NULL)) != NULL) {
				// FIXME gotta commit this to disk
				dir->f_size += JOSFS_BLKSIZE;
				f = (struct JOSFS_File *) blk->ddesc->data;
				temp_fdesc->file = &f[0];
				memset(temp_fdesc->file, 0, sizeof(struct JOSFS_File));
				strcpy(f[0].f_name, filename);
				f[0].f_type = type;
				temp_fdesc->file->f_dir = dir;
				return (fdesc_t *) temp_fdesc;
			}
		}
	}
	return NULL;
}

static int josfs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	fdesc_t * oldfdesc;
	fdesc_t * newfdesc;

	oldfdesc = josfs_lookup_name(object, oldname);
	if (oldfdesc != NULL) {
		newfdesc = josfs_allocate_name(object, newname, ((struct JOSFS_File *) oldfdesc)->f_type, NULL, NULL, NULL);
		if (newfdesc != NULL) {
			if (josfs_remove_name(object, oldname, NULL, NULL) == 0) {
				return 0;
			}
			else {
				josfs_remove_name(object, newname, NULL, NULL);
			}
		}
	}
	
	return -1;
}

//FIXME bdesc
static bdesc_t * josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) object->instance;
	struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	uint32_t nblocks = (f->file->f_size / JOSFS_BLKSIZE);
	bdesc_t * indirect;
	
	if (f->file->f_size % JOSFS_BLKSIZE) {
		nblocks++;
	}

	if (nblocks > JOSFS_NINDIRECT) {
		return NULL;
	}
	else if (nblocks > JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect != NULL) {
			return CALL(info->ubd, read_block, (uint32_t) (indirect->ddesc->data) + nblocks);
		}
	}
	else if (nblocks == JOSFS_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect);
		if (indirect != NULL) {
			if (josfs_free_block(object, indirect, NULL, NULL) == 0) {
				// FIXME write to disk
				f->file->f_indirect = 0;
				return CALL(info->ubd, read_block, (uint32_t) (indirect->ddesc->data) + nblocks);
			}
		}
	}
	else {
		return CALL(info->ubd, read_block, f->file->f_direct[nblocks]);
	}

	return NULL;
}

//FIXME bdesc
static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	if (block->number == 0)
		return -1;
	write_bitmap(object, block->number, 1);
	return 0;
}

static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	int r;
	struct JOSFS_File *f;

	if ((r = walk_path(object, name, 0, &f, 0)) < 0)
		return r;

	// FIXME write this out to disk
	f->f_name[0] = '\0';
	return 0;
}

//FIXME bdesc
static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data, chdesc_t ** head, chdesc_t ** tail)
{
	int r;
	struct lfs_info * info = (struct lfs_info *) object->instance;

	if (offset + size > JOSFS_BLKSIZE) {
		return -1;
	}

	memcpy(&block->ddesc->data[offset], data, size);
	if ((r = CALL(info->ubd, write_block, block)) < 0)
		return r;

	return 0;
}

static size_t josfs_get_num_features(LFS_t * object, const char * name)
{
	return 1;
}

static const feature_t * josfs_get_feature(LFS_t * object, const char * name, size_t num)
{
	if (num == 0) {
		return &KFS_feature_size;
	}
	return NULL;
}

static int josfs_get_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t * size, void ** data)
{
	if (id == KFS_feature_size.id) {
		if (sizeof(off_t) <= *size) {
			*data = (void *) f->file->f_size;
		}
	}

	return 0;
}

static int josfs_get_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) josfs_lookup_name(object, name);
	return josfs_get_metadata(object, f, id, size, data);
}

static int josfs_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_get_metadata(object, f, id, size, data);
}

static int josfs_set_metadata(LFS_t * object, const struct josfs_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	if (id == KFS_feature_size.id) {
		if (sizeof(off_t) >= size) {
			if ((off_t)data >= 0 && (off_t)data < 4194304) {
				// FIXME write to disk
				f->file->f_size = (off_t)data;
				return 0;
			}
		}
	}

	return -1;
}

static int josfs_set_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	const struct josfs_fdesc * file = (struct josfs_fdesc *) josfs_lookup_name(object, name);
	return josfs_set_metadata(object, file, id, size, data, head, tail);
}

static int josfs_set_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	const struct josfs_fdesc * f = (struct josfs_fdesc *) file;
	return josfs_set_metadata(object, f, id, size, data, head, tail);
}

// TODO
static int josfs_sync(LFS_t * object, const char * name)
{
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
	if (!info)
	{
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

	return lfs;
}
