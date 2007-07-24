#include <lib/platform.h>
#include <lib/hash_set.h>
#include <lib/jiffies.h>
#include <lib/pool.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/ext2_base.h>
#include <kfs/feature.h>

#define EXT2_BASE_DEBUG 0

#define ROUND_ROBIN_ALLOC 1

#if EXT2_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* values for the "purpose" parameter */
#define PURPOSE_FILEDATA 0
#define PURPOSE_DIRDATA 1
#define PURPOSE_INDIRECT 2
#define PURPOSE_DINDIRECT 3

struct ext2_mdirent;
struct mdirent_dlist {
	struct ext2_mdirent ** pprev, * next;
};
typedef struct mdirent_dlist mdirent_dlist_t;

/* in-memory dirent */
struct ext2_mdirent {
	EXT2_Dir_entry_t dirent;
	char name_term; /* ensure room for dirent.name null termination */
	uint32_t offset;
	mdirent_dlist_t offsetl;
	mdirent_dlist_t freel;
};
typedef struct ext2_mdirent ext2_mdirent_t;

/* in-memory directory */
struct ext2_mdir {
	inode_t ino; /* inode of this directory */
	hash_map_t * mdirents; /* file name -> ext2_mdirent */
	ext2_mdirent_t * offset_first, * offset_last;
	ext2_mdirent_t * free_first, * free_last;
	struct ext2_mdir ** lru_polder, * lru_newer;
};
typedef struct ext2_mdir ext2_mdir_t;

/* Perhaps this is a good number? */
#define MAXCACHEDDIRS 1024

struct ext2_mdir_cache {
	hash_map_t * mdirs_map;
	ext2_mdir_t mdirs_table[MAXCACHEDDIRS];
	ext2_mdir_t * lru_oldest, * lru_newest;
};
typedef struct ext2_mdir_cache ext2_mdir_cache_t;


struct ext2_info {
	LFS_t lfs;
	
	BD_t * ubd;
	chdesc_t ** write_head;
	const EXT2_Super_t *super; /* const to limit who can change it */
	const EXT2_group_desc_t *groups; /* const to limit who can change it */
	hash_map_t * filemap;
	ext2_mdir_cache_t mdir_cache;
	bdesc_t ** gdescs;
	bdesc_t * super_cache;
	bdesc_t * bitmap_cache;
	bdesc_t * inode_cache;
	uint32_t ngroups, gnum;
	uint32_t ngroupblocks;
	uint32_t inode_gdesc;
	uint16_t block_descs;
#if ROUND_ROBIN_ALLOC
	/* the last block number allocated for each of file
	 * data, directory data, and [d]indirect pointers */
	uint32_t last_fblock, last_dblock, last_iblock;
#endif
	uint32_t _blocksize_;
};
typedef struct ext2_info ext2_info_t;

struct ext2_fdesc {
	/* extend struct fdesc */
	struct fdesc_common * common;
	struct fdesc_common base;

	bdesc_t *f_inode_cache;
	EXT2_inode_t f_inode;
	uint8_t f_type;
	inode_t	f_ino;
	uint32_t f_nopen;
#if !ROUND_ROBIN_ALLOC
	uint32_t f_lastblock;
#endif
};
typedef struct ext2_fdesc ext2_fdesc_t;

/* some prototypes */
static int ext2_read_block_bitmap(LFS_t * object, uint32_t blockno);
static int _ext2_free_block(LFS_t * object, uint32_t block, chdesc_t ** head);
static uint32_t get_file_block(LFS_t * object, ext2_fdesc_t * file, uint32_t offset);
static int ext2_set_metadata(LFS_t * object, ext2_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head);

static int ext2_super_report(LFS_t * lfs, uint32_t group, int32_t blocks, int32_t inodes, int32_t dirs);
static int ext2_get_inode(ext2_info_t * info, ext2_fdesc_t *f, int copy);
static uint8_t ext2_to_kfs_type(uint16_t type);
static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t * dir, ext2_mdirent_t * mdirent, chdesc_t ** head);

static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent);
int ext2_write_inode(struct ext2_info *info, ext2_fdesc_t *f, chdesc_t ** head);
int ext2_write_inode_set(struct ext2_info *info, ext2_fdesc_t *f, chdesc_t ** tail, chdesc_pass_set_t * befores);

DECLARE_POOL(ext2_mdirent, ext2_mdirent_t);
DECLARE_POOL(ext2_fdesc, ext2_fdesc_t);
static int n_ext2_instances;

static int check_super(LFS_t * object)
{
	struct ext2_info * info = (struct ext2_info *) object;
		
	/* the superblock is in block 1 */
	printf("\tMagic Number 0x%x \n", info->super->s_magic);
	printf("\tBlocksize might be %i\n", info->ubd->blocksize);
	printf("\tNumber of inodes %i\n", info->super->s_inodes_count);
	printf("\tSize of inode sturcture %i\n", info->super->s_inode_size);
	printf("\tNumber of free inodes %i\n", info->super->s_free_inodes_count);
	printf("\tNumber of blocks %i\n", info->super->s_blocks_count);
	printf("\tEXT2 Block size %i\n", 1024 << info->super->s_log_block_size);
	printf("\tNumber of free blocks %i\n", info->super->s_free_blocks_count);
	printf("\tSize of block group is %i\n", sizeof(EXT2_group_desc_t));
	printf("\tNumber of blocks per group %i\n", info->super->s_blocks_per_group);
	printf("\tNumber of inodes per group %i\n", info->super->s_inodes_per_group);
	
	if(info->super->s_magic != EXT2_FS_MAGIC)
	{
		printf("ext2_base: bad file system magic number\n");
		return -1;
	}
	
	return 0;
}

static uint16_t dirent_rec_len(uint16_t name_len)
{
	return 8 + ((name_len - 1) / 4 + 1) * 4;
}

static bool dirent_has_free_space(const EXT2_Dir_entry_t * entry)
{
	if(!entry->inode)
		return 1;
	if(entry->rec_len > dirent_rec_len(entry->name_len))
		return 1;
	return 0;
}

// Return the previous (offset-wise) mdirent
static ext2_mdirent_t * ext2_mdirent_offset_prev(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent)
{
	if(mdir->offset_first == mdirent)
		return NULL;
	return container_of(mdirent->offsetl.pprev, ext2_mdirent_t, offsetl.next);
}

// Return the next mdirent with free space
static ext2_mdirent_t * ext2_mdirent_free_next(const ext2_mdir_t * mdir, const ext2_mdirent_t * used)
{
	ext2_mdirent_t * mdirent;
	if(!mdir->free_last || mdir->free_last->offset < used->offset)
		return NULL;
	for(mdirent = used->offsetl.next; mdirent; mdirent = mdirent->offsetl.next)
		if(mdirent->freel.pprev)
			return mdirent;
	assert(0);
	return NULL;
}

// Insert mdirent into the free list
static void ext2_mdirent_insert_free_list(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent)
{
	ext2_mdirent_t * next = ext2_mdirent_free_next(mdir, mdirent);
	if(next)
	{
		mdirent->freel.next = next;
		mdirent->freel.pprev = next->freel.pprev;
		*mdirent->freel.pprev = mdirent;
		next->freel.pprev = &mdirent->freel.next;
	}
	else
	{
		mdirent->freel.pprev = &mdir->free_last->freel.next;
		*mdirent->freel.pprev = mdirent;
		mdirent->freel.next = NULL;
		mdir->free_last = mdirent;
	}
}

// Remove mdirent from the free list
static void ext2_mdirent_remove_free_list(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent)
{
	*mdirent->freel.pprev = mdirent->freel.next;
	if(mdirent->freel.next)
		mdirent->freel.next->freel.pprev = mdirent->freel.pprev;
	else if(mdir->free_first != mdirent)
		mdir->free_last = container_of(mdirent->freel.pprev, ext2_mdirent_t, freel.next);
	else
		mdir->free_last = NULL;
	mdirent->freel.pprev = NULL;
	mdirent->freel.next = NULL;
}

// Return the mdirent in mdir named 'name'
static ext2_mdirent_t * ext2_mdirent_get(ext2_mdir_t * mdir, const char * name)
{
	return hash_map_find_val(mdir->mdirents, name);
}

// Free the contents of mdir
static void ext2_mdirents_free(ext2_mdir_t * mdir)
{
	ext2_mdirent_t * mdirent = mdir->offset_first;
	hash_map_clear(mdir->mdirents);
	while(mdirent)
	{
		ext2_mdirent_t * next = mdirent->offsetl.next;
		ext2_mdirent_free(mdirent);
		mdirent = next;
	}
	mdir->offset_first = mdir->offset_last = NULL;
	mdir->free_first = mdir->free_last = NULL;
}

// Add a new mdirent to mdir
static int ext2_mdirent_add(ext2_mdir_t * mdir, const EXT2_Dir_entry_t * entry, uint32_t offset)
{
	ext2_mdirent_t * mdirent = ext2_mdirent_alloc();
	int r;
	if(!mdirent)
		return -ENOMEM;

	memcpy(&mdirent->dirent, entry, MIN(entry->rec_len, sizeof(*entry)));
	mdirent->dirent.name[mdirent->dirent.name_len] = 0;
	mdirent->offset = offset;

	r = hash_map_insert(mdir->mdirents, mdirent->dirent.name, mdirent);
	if(r < 0)
	{
		ext2_mdirent_free(mdirent);
		return r;
	}
	assert(!r);

	if(!mdir->offset_first)
		mdirent->offsetl.pprev = &mdir->offset_first;
	else
	{
		assert(mdir->offset_last->offset + mdir->offset_last->dirent.rec_len == offset);
		mdirent->offsetl.pprev = &mdir->offset_last->offsetl.next;
	}
	*mdirent->offsetl.pprev = mdirent;
	mdirent->offsetl.next = NULL;
	mdir->offset_last = mdirent;

	if(dirent_has_free_space(entry))
	{
		if(!mdir->free_last)
			mdirent->freel.pprev = &mdir->free_first;
		else
			mdirent->freel.pprev = &mdir->free_last->freel.next;
		*mdirent->freel.pprev = mdirent;
		mdirent->freel.next = NULL;
		mdir->free_last = mdirent;
	}
	else
	{
		mdirent->freel.pprev = NULL;
		mdirent->freel.next = NULL;
	}

	return 0;
}

// Mark mdirent as used
static int ext2_mdirent_use(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, const EXT2_Dir_entry_t * entry)
{
	int r;
	assert(!mdirent->dirent.inode);
	assert(mdirent->dirent.rec_len == entry->rec_len);

	memcpy(&mdirent->dirent, entry, MIN(entry->rec_len, sizeof(*entry)));
	mdirent->dirent.name[entry->name_len] = 0;
	r = hash_map_insert(mdir->mdirents, mdirent->dirent.name, mdirent);
	if(r < 0)
		return r;

	if(!dirent_has_free_space(entry))
		ext2_mdirent_remove_free_list(mdir, mdirent);

	return 0;
}

// Mark mdirent as unused
static void ext2_mdirent_clear(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, uint32_t blocksize)
{
	ext2_mdirent_t * mde = hash_map_erase(mdir->mdirents, mdirent->dirent.name);
	assert(mde == mdirent);

	if(!(mdirent->offset % blocksize))
	{
		// convert to a jump (empty) dirent
		mdirent->dirent.inode = 0;
		if(!mdirent->freel.pprev)
			ext2_mdirent_insert_free_list(mdir, mdirent);
	}
	else
	{
		// merge into the previous dirent
		ext2_mdirent_t * oprev = ext2_mdirent_offset_prev(mdir, mdirent);
		oprev->dirent.rec_len += mdirent->dirent.rec_len;

		oprev->offsetl.next = mdirent->offsetl.next;
		if(mdirent->offsetl.next)
			mdirent->offsetl.next->offsetl.pprev = &oprev->offsetl.next;
		else
			mdir->offset_last = oprev;

		if(oprev->freel.pprev)
		{
			if(mdirent->freel.pprev)
			{
				oprev->freel.next = mdirent->freel.next;
				if(oprev->freel.next)
					oprev->freel.next->freel.pprev = &oprev->freel.next;
				else
					mdir->free_last = oprev;
			}
		}
		else
		{
			if(mdirent->freel.pprev)
			{
				oprev->freel.pprev = mdirent->freel.pprev;
				*oprev->freel.pprev = oprev;
				oprev->freel.next = mdirent->freel.next;
				if(oprev->freel.next)
					oprev->freel.next->freel.pprev = &oprev->freel.next;
				else
					mdir->free_last = oprev;
			}
			else
				ext2_mdirent_insert_free_list(mdir, oprev);
		}

		ext2_mdirent_free(mdirent);
	}
}

// Split a new dirent out of mdirent's unused space
static int ext2_mdirent_split(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, const EXT2_Dir_entry_t * existing_dirent, const EXT2_Dir_entry_t * new_dirent)
{
	ext2_mdirent_t * nmdirent = ext2_mdirent_alloc();
	int r;
	if(!nmdirent)
		return -ENOMEM;

	memcpy(&nmdirent->dirent, new_dirent, MIN(new_dirent->rec_len, sizeof(*new_dirent)));
	nmdirent->dirent.name[nmdirent->dirent.name_len] = 0;

	r = hash_map_insert(mdir->mdirents, nmdirent->dirent.name, nmdirent);
	if(r < 0)
	{
		ext2_mdirent_free(nmdirent);
		return r;
	}
	assert(!r);

	mdirent->dirent.rec_len = existing_dirent->rec_len;
	nmdirent->offset = mdirent->offset + mdirent->dirent.rec_len;

	nmdirent->offsetl.next = mdirent->offsetl.next;
	nmdirent->offsetl.pprev = &mdirent->offsetl.next;
	*nmdirent->offsetl.pprev = nmdirent;
	if(nmdirent->offsetl.next)
		nmdirent->offsetl.next->offsetl.pprev = &nmdirent->offsetl.next;
	else
		mdir->offset_last = nmdirent;

	if(dirent_has_free_space(new_dirent))
	{
		nmdirent->freel.pprev = mdirent->freel.pprev;
		*nmdirent->freel.pprev = nmdirent;
		nmdirent->freel.next = mdirent->freel.next;
		if(nmdirent->freel.next)
			nmdirent->freel.next->freel.pprev = &nmdirent->freel.next;
		else
			mdir->free_last = nmdirent;
		mdirent->freel.pprev = NULL;
		mdirent->freel.next = NULL;
	}
	else
	{
		ext2_mdirent_remove_free_list(mdir, mdirent);
		nmdirent->freel.pprev = NULL;
		nmdirent->freel.next = NULL;
	}

	return 0;
}

static void ext2_mdir_remove(LFS_t * object, inode_t ino)
{
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_mdir_cache_t * cache = &info->mdir_cache;
	ext2_mdir_t * mdir = hash_map_find_val(cache->mdirs_map, (void *) ino);

	if(!mdir)
		return;

	ext2_mdirents_free(mdir);
	hash_map_erase(cache->mdirs_map, (void *) ino);
	mdir->ino = INODE_NONE;

	// Update mdir lru list to make mdir the oldest
	if(mdir->lru_newer)
		mdir->lru_newer->lru_polder = mdir->lru_polder;
	else
		info->mdir_cache.lru_newest = container_of(mdir->lru_polder, ext2_mdir_t, lru_newer);
	*mdir->lru_polder = mdir->lru_newer;
	mdir->lru_newer = info->mdir_cache.lru_oldest;
	mdir->lru_polder = &info->mdir_cache.lru_oldest->lru_newer;
	*mdir->lru_polder = mdir;
}

// Add a directory to the directory cache
static int ext2_mdir_add(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t ** pmdir)
{
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_mdir_cache_t * cache = &info->mdir_cache;
	ext2_mdir_t * mdir = cache->lru_oldest;
	uint32_t cur_base = 0, next_base = 0;
	int r;

	if(mdir->ino != INODE_NONE)
	{
		ext2_mdirents_free(mdir); // Oldest mdir is still alive. Free it.
		hash_map_erase(cache->mdirs_map, (void *) mdir->ino);
	}
	mdir->ino = dir_file->f_ino;
	mdir->offset_first = mdir->offset_last = NULL;
	mdir->free_first = mdir->free_last = NULL;
	r = hash_map_insert(info->mdir_cache.mdirs_map, (void *) mdir->ino, mdir);
	if(r < 0)
		return r;
	
	// This reads the entire directory. Would it be better to read on demand?
	for(; cur_base < dir_file->f_inode.i_size; cur_base = next_base)
	{
		const EXT2_Dir_entry_t * entry;
		// TODO: pass disk block to ext2_get_disk_dirent()?
		// or have it use an internal single item cache?
		r = ext2_get_disk_dirent(object, dir_file, &next_base, &entry);
		if(r < 0)
			goto fail;
		r = ext2_mdirent_add(mdir, entry, cur_base);
		if(r < 0)
			goto fail;
	}

	if(mdir->lru_newer)
	{
		// Update mdir lru list to make mdir the most recent
		mdir->lru_newer->lru_polder = mdir->lru_polder;
		*mdir->lru_polder = mdir->lru_newer;
		mdir->lru_polder = &info->mdir_cache.lru_newest->lru_newer;
		*mdir->lru_polder = mdir;
		mdir->lru_newer = NULL;
		info->mdir_cache.lru_newest = mdir;
	}
	
	*pmdir = mdir;
	return 0;

  fail:
	mdir->ino = INODE_NONE;
	ext2_mdirents_free(mdir);
	return r;
}

// Get (and create, if it does not exist) a directory from the mdir cache
static int ext2_mdir_get(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t ** pmdir)
{
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_mdir_cache_t * cache = &info->mdir_cache;
	ext2_mdir_t * mdir = hash_map_find_val(cache->mdirs_map, (void *) dir_file->f_ino);

	if(mdir)
	{
		if(mdir->lru_newer)
		{
			// Update lru list to make mdir the most recent
			mdir->lru_newer->lru_polder = mdir->lru_polder;
			*mdir->lru_polder = mdir->lru_newer;
			mdir->lru_polder = &cache->lru_newest->lru_newer;
			*mdir->lru_polder = mdir;
			mdir->lru_newer = NULL;
			cache->lru_newest = mdir;
		}
		*pmdir = mdir;
		return 0;
	}

	return ext2_mdir_add(object, dir_file, pmdir);
}

static void ext2_mdir_cache_deinit(ext2_mdir_cache_t * cache)
{
	size_t i;
	hash_map_destroy(cache->mdirs_map);
	for(i = 0; i < MAXCACHEDDIRS; i++)
	{
		ext2_mdirents_free(&cache->mdirs_table[i]);
		hash_map_destroy(cache->mdirs_table[i].mdirents);
	}
}

static int ext2_mdir_cache_init(ext2_mdir_cache_t * cache)
{
	size_t i;

	cache->mdirs_map = hash_map_create_size(MAXCACHEDDIRS, 0);
	if(!cache->mdirs_map)
		return -ENOMEM;

	for(i = 0; i < MAXCACHEDDIRS; i++)
	{
		cache->mdirs_table[i].ino = INODE_NONE;
		cache->mdirs_table[i].mdirents = hash_map_create_str();
		cache->mdirs_table[i].offset_first = NULL;
		cache->mdirs_table[i].offset_last = NULL;
		cache->mdirs_table[i].free_first = NULL;
		cache->mdirs_table[i].free_last = NULL;
		if(!cache->mdirs_table[i].mdirents)
		{
			ext2_mdir_cache_deinit(cache);
			return -ENOMEM;
		}
	}
	
	cache->lru_oldest = &cache->mdirs_table[0];
	cache->lru_oldest->lru_polder = &cache->lru_oldest;
	cache->lru_oldest->lru_newer = &cache->mdirs_table[1];
	for(i = 1; i < MAXCACHEDDIRS - 1; i++)
	{
		cache->mdirs_table[i].lru_polder = &cache->mdirs_table[i - 1].lru_newer;
		cache->mdirs_table[i].lru_newer = &cache->mdirs_table[i + 1];
	}
	cache->lru_newest = &cache->mdirs_table[MAXCACHEDDIRS - 1];
	cache->lru_newest->lru_polder = &cache->mdirs_table[MAXCACHEDDIRS - 2].lru_newer;
	cache->lru_newest->lru_newer = NULL;
	
	return 0;
}


/* When round robin allocation is enabled, *blockno is used as the minimum block
 * number to allocate (unless we wrap around the end of the file system).
 * Otherwise, it is used only to determine which block group to look at first.
 * This is merely an optimization: unless round robin allocation is enabled, we
 * will never pass anything but the first block of a block group anyway. */
static int ext2_find_free_block(LFS_t * object, uint32_t * blockno)
{
	Dprintf("EXT2DEBUG: %s blockno is %u\n", __FUNCTION__, *blockno);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t start_group, block_group;
#if ROUND_ROBIN_ALLOC
	uint32_t minimum, offset, offset_bits;
#endif
	
	if(*blockno < info->super->s_first_data_block)
	{
		printf("%s(): requested status of reserved block %u!\n", __FUNCTION__, *blockno);
		return -EINVAL;
	}
	if(*blockno >= info->super->s_blocks_count)
	{
		printf("%s(): requested status of block %u past end of file system!\n", __FUNCTION__, *blockno);
		return -EINVAL;
	}
	
	start_group = *blockno / info->super->s_blocks_per_group;
	block_group = start_group;
#if ROUND_ROBIN_ALLOC
	minimum = *blockno % info->super->s_blocks_per_group;
	offset = minimum / (sizeof(unsigned long) * 8);
	offset_bits = offset * sizeof(unsigned long) * 8;
#endif
	do {
		bdesc_t * bitmap;
		const unsigned long * array;
		int index;
		
		/* Read in the block bitmap for this group */
		if(info->gnum != block_group || !info->bitmap_cache)
		{
			if(info->bitmap_cache)
				bdesc_release(&info->bitmap_cache);	
			info->gnum = block_group;
			bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
			if(!bitmap)
				return -ENOENT;
			bdesc_retain(bitmap);
			bitmap->ddesc->flags |= BDESC_FLAG_BITMAP;
			info->bitmap_cache = bitmap;
		}
		
		array = (const unsigned long *) info->bitmap_cache->ddesc->data;
#if ROUND_ROBIN_ALLOC
		/* adjust array for offset */
		array = &array[offset];
	retry:
		index = find_first_zero_bit(array, info->super->s_blocks_per_group - offset_bits);
		/* adjust result for offset */
		index += offset_bits;
		
		if(index < minimum)
		{
			/* one of the earlier bits in the same 32-bit
			 * word as the first allowed bit is zero... but
			 * we must choose a later bit than that */
			uint32_t block, limit = (*blockno + sizeof(unsigned long) * 8);
			limit &= ~(sizeof(unsigned long) * 8 - 1);
			for(block = *blockno; block < limit; block++)
				if(ext2_read_block_bitmap(object, block) == EXT2_FREE)
				{
					*blockno = block;
					return EXT2_FREE;
				}
			/* found nothing, go to next 32-bit word and retry */
			array = &array[1];
			offset++;
			offset_bits += sizeof(unsigned long) * 8;
			minimum = offset_bits;
			goto retry;
		}
#else
		index = find_first_zero_bit(array, info->super->s_blocks_per_group);
#endif
		
		if(index < info->super->s_blocks_per_group)
		{
			*blockno = block_group * info->super->s_blocks_per_group + index;
			return EXT2_FREE;
		}
		
		block_group = (block_group + 1) % info->ngroups;
	} while(block_group != start_group);
	
	return -ENOSPC;
}

static int ext2_read_block_bitmap(LFS_t * object, uint32_t blockno)
{
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t block_group, block_in_group;
	uint32_t * bitmap;
	
	if(blockno < info->super->s_first_data_block)
	{
		printf("%s(): requested status of reserved block %u!\n", __FUNCTION__, blockno);
		return -EINVAL;
	}
	if(blockno >= info->super->s_blocks_count)
	{
		printf("%s(): requested status of block %u past end of file system!\n", __FUNCTION__, blockno);
		return -EINVAL;
	}
	
	block_group = blockno / info->super->s_blocks_per_group;
	if(info->gnum != block_group || !info->bitmap_cache)
	{
		if(info->bitmap_cache)
			bdesc_release(&info->bitmap_cache);	
		info->gnum = block_group;
		info->bitmap_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
		if(!info->bitmap_cache)
			return -ENOENT;
		bdesc_retain(info->bitmap_cache);
		info->bitmap_cache->ddesc->flags |= BDESC_FLAG_BITMAP;
	}
	
	block_in_group = blockno % info->super->s_blocks_per_group;
	bitmap = ((uint32_t *) info->bitmap_cache->ddesc->data) + (block_in_group / 32);
	if(*bitmap & (1 << (block_in_group % 32)))
		return EXT2_USED;
	return EXT2_FREE;
}

static int ext2_write_block_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: write_bitmap %u -> %d\n", blockno, value);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t block_group, block_in_group;
	int r;
	
	if(!head)
		return -1;
	
	if(blockno < info->super->s_first_data_block || blockno == INVALID_BLOCK)
	{
		printf("%s(): requested status of reserved block %u!\n", __FUNCTION__, blockno);
		return -EINVAL;
	}
	else if(blockno >= info->super->s_blocks_count)
	{
		printf("%s(): requested status of block %u past end of file system!\n", __FUNCTION__, blockno);
		return -EINVAL;
	}
	
	block_group = blockno / info->super->s_blocks_per_group;
	if(info->gnum != block_group || !info->bitmap_cache)
	{
		if(info->bitmap_cache)
			bdesc_release(&info->bitmap_cache);	
		info->gnum = block_group;
		info->bitmap_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
		if(!info->bitmap_cache)
			return -ENOENT;
		bdesc_retain(info->bitmap_cache);
		info->bitmap_cache->ddesc->flags |= BDESC_FLAG_BITMAP;
	}
	
	block_in_group = blockno % info->super->s_blocks_per_group;	
	/* does it already have the right value? */
	if(((uint32_t *) info->bitmap_cache->ddesc->data)[block_in_group / 32] & (1 << (block_in_group % 32)))
	{
		if(value)
			return 0;
	}
	else if(!value)
		return 0;
	
	/* bit chdescs take offset in increments of 32 bits */
	r = chdesc_create_bit(info->bitmap_cache, info->ubd, block_in_group / 32, 1 << (block_in_group % 32), head);
	if(r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, value ? "allocate block" : "free block");
	
	r = CALL(info->ubd, write_block, info->bitmap_cache);
	if(r < 0)
		return r;
	
	return ext2_super_report(object, block_group, (value ? -1 : 1), 0, 0);
}

static int ext2_write_inode_bitmap(LFS_t * object, inode_t inode_no, bool value, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_write_inode_bitmap %u\n", inode_no);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t block_group, inode_in_group;
	int r;
	
	if(!head)
		return -1;
	
	if(inode_no >= info->super->s_inodes_count)
	{
		printf("%s(): inode %u past end of file system!\n", __FUNCTION__, inode_no);
		return -1;
	}
	
	block_group = (inode_no - 1) / info->super->s_inodes_per_group;
	if(info->inode_gdesc != block_group || !info->inode_cache)
	{
		if(info->inode_cache)
			bdesc_release(&info->inode_cache);	
		info->inode_gdesc = block_group;
		info->inode_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_inode_bitmap, 1);
		if(!info->inode_cache)
			return -ENOENT;
		bdesc_retain(info->inode_cache);
		info->inode_cache->ddesc->flags |= BDESC_FLAG_BITMAP;
	}
	
	inode_in_group = (inode_no - 1) % info->super->s_inodes_per_group;
	/* does it already have the right value? */
	if(((uint32_t *) info->inode_cache->ddesc->data)[inode_in_group / 32] & (1 << (inode_in_group % 32)))
	{
		if(value)
			return 0;
	}
	else if(!value)
		return 0;
	
	/* bit chdescs take offset in increments of 32 bits */
	r = chdesc_create_bit(info->inode_cache, info->ubd, inode_in_group / 32, 1 << (inode_in_group % 32), head);
	if (r < 0)
		return r;	
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, value ? "allocate inode" : "free inode");
	
	r = CALL(info->ubd, write_block, info->inode_cache);
	if (r < 0)
		return r;
	return ext2_super_report(object, block_group, 0, value ? -1 : 1, 0);
}

static uint32_t count_free_space(LFS_t * object)
{
	struct ext2_info * info = (struct ext2_info *) object;
	return info->super->s_free_blocks_count;
}

static int ext2_get_root(LFS_t * object, inode_t * ino)
{
	*ino = EXT2_ROOT_INO;
	return 0;
}

static uint32_t ext2_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** tail)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t blockno, lastblock = 0;
#if !ROUND_ROBIN_ALLOC
	uint32_t block_group;
#endif
	int r;
	
	if(!tail || !f)
		return INVALID_BLOCK;
	
#if !ROUND_ROBIN_ALLOC
	if(f->f_inode.i_size == 0 || purpose)
		goto inode_search;
	
	//Get the block number of the last block of the inode
	if(f->f_lastblock != 0)
		blockno = f->f_lastblock;
	else
		blockno = get_file_block(object, (ext2_fdesc_t *) f, f->f_inode.i_size - 1);	
	if(blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	lastblock = blockno;
	//FIXME this could affect performance
	//Look in the 32 block vicinity of the lastblock
	//There is no check to make sure that these blocks are all in the same block group
	while(blockno - lastblock < 32)
	{
		int r = ext2_read_block_bitmap(object, ++blockno);
		if(r == EXT2_FREE)
			goto claim_block;
		else if(r < 0)
			return INVALID_BLOCK;
	}
	
inode_search:	
	//Look for free blocks in same block group as the inode
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	if(purpose == PURPOSE_DIRDATA)
		block_group = (block_group + 2) % info->ngroups;
	else if(purpose)
		block_group = (block_group + 1) % info->ngroups;
	blockno = block_group * info->super->s_blocks_per_group;
#else
	if(purpose == PURPOSE_FILEDATA)
		blockno = info->last_fblock;
	else if(purpose == PURPOSE_DIRDATA)
		blockno = info->last_dblock;
	else
		blockno = info->last_iblock;
#endif
	//FIXME this should be slightly smarter
	while(blockno < info->super->s_blocks_count)
	{
		r = ext2_find_free_block(object, &blockno);
		if(r < 0)
			break;
		if(r == EXT2_FREE)
			goto claim_block;
		blockno += info->super->s_blocks_per_group;
	}
	
	return INVALID_BLOCK;
	
claim_block:
	*tail = info->write_head ? *info->write_head : NULL;
	if(ext2_write_block_bitmap(object, blockno, 1, tail) < 0)
	{
		ext2_write_block_bitmap(object, blockno, 0, tail);
		return INVALID_BLOCK;
	}
#if !ROUND_ROBIN_ALLOC
	if(purpose == PURPOSE_FILEDATA || purpose == PURPOSE_DIRDATA)
		f->f_lastblock = blockno;
#else
	lastblock = (blockno + 1) % info->super->s_blocks_count;
	if(purpose == PURPOSE_FILEDATA)
		info->last_fblock = lastblock;
	else if(purpose == PURPOSE_DIRDATA)
		info->last_dblock = lastblock;
	else
		info->last_iblock = lastblock;
#endif
	return blockno;
}

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) object;
	return CALL(info->ubd, read_block, number, 1);
}

static bdesc_t * ext2_synthetic_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_synthetic_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) object;
	return CALL(info->ubd, synthetic_read_block, number, 1);
}

static fdesc_t * ext2_lookup_inode(LFS_t * object, inode_t ino)
{
	ext2_fdesc_t * fd = NULL;
	struct ext2_info * info = (struct ext2_info *) object;
	int r;
	
	if(ino <= 0)
		return NULL;
	
	fd = hash_map_find_val(info->filemap, (void *) ino);
	if(fd)
	{
		fd->f_nopen++;
		return (fdesc_t *) fd;
	}
	
	fd = ext2_fdesc_alloc();
	if(!fd)
		goto ext2_lookup_inode_exit;
	
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_inode_cache = NULL;
	fd->f_ino = ino;
	fd->f_nopen = 1;
#if !ROUND_ROBIN_ALLOC
	fd->f_lastblock = 0;
#endif
	
	r = ext2_get_inode(info, fd, 1);
	if(r < 0)
		goto ext2_lookup_inode_exit;
	
	fd->f_type = ext2_to_kfs_type(fd->f_inode.i_mode);
	
	r = hash_map_insert(info->filemap, (void *) ino, fd);
	if(r < 0)
		goto ext2_lookup_inode_exit;
	assert(r == 0);
	
	return (fdesc_t*) fd;
	
ext2_lookup_inode_exit:
	ext2_fdesc_free(fd);
	return NULL;
}

static void ext2_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("EXT2DEBUG: ext2_free_fdesc %p\n", fdesc);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) fdesc;
	
	if(f)
	{
		if(f->f_nopen > 1)
		{
			f->f_nopen--;
			return;
		}
		if (f->f_inode_cache)
			bdesc_release(&f->f_inode_cache);
		hash_map_erase(info->filemap, (void *) f->f_ino);
		ext2_fdesc_free(f);
	}
}

static int ext2_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("EXT2DEBUG: ext2_lookup_name %s\n", name);
	ext2_fdesc_t * fd;
	ext2_fdesc_t * parent_file;
	ext2_mdir_t * mdir;
	ext2_mdirent_t * mdirent;
	int r = 0;
	
	//TODO do some sanity checks on name
	
	// "." and ".." are (at least right now) supported by code further up
	// (this seems hacky, but it would be hard to figure out parent's parent from here)
	
	fd = (ext2_fdesc_t *) ext2_lookup_inode(object, parent);
	if(!fd)
		return -ENOENT;
	if(fd->f_type != TYPE_DIR)
		return -ENOTDIR;
	parent_file = fd;
	
	r = ext2_mdir_get(object, parent_file, &mdir);
	if(r < 0)
		goto exit;
	mdirent = ext2_mdirent_get(mdir, name);
	if(mdirent)
	{
		fd = (ext2_fdesc_t *) ext2_lookup_inode(object, mdirent->dirent.inode);
		if(fd && ino)
			*ino = fd->f_ino;
	}
	else
		r = -ENOENT;
	
  exit:
	if(fd != parent_file)
		ext2_free_fdesc(object, (fdesc_t *) fd);
	ext2_free_fdesc(object, (fdesc_t *) parent_file);
	if(r < 0)
		return r;
	return 0;
}

static uint32_t ext2_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	
	if(f->f_type == TYPE_SYMLINK)
		return 0;
	
	return (f->f_inode.i_size + object->blocksize - 1) / object->blocksize;
}

static uint32_t get_file_block(LFS_t * object, ext2_fdesc_t * file, uint32_t offset)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno, n_per_block;
	bdesc_t * block_desc;
	uint32_t * inode_nums, blocknum;
	
	if (offset >= file->f_inode.i_size || file->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;
	
	n_per_block = object->blocksize / (sizeof(uint32_t));
	
	//non block aligned offsets suck (aka aren't supported)
	blocknum = offset / object->blocksize;
	
	//TODO: compress this code, but right now its much easier to understand...
	if (blocknum >= n_per_block * n_per_block + n_per_block + EXT2_NDIRECT)
	{
		// Lets not worry about tripley indirect for the momment
		return INVALID_BLOCK;
	}
	else if (blocknum >= n_per_block + EXT2_NDIRECT)
	{
		blocknum -= (EXT2_NDIRECT + n_per_block);
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_DINDIRECT], 1));
		if (!block_desc)
		{
			Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		blockno = inode_nums[blocknum / n_per_block];
		block_desc = CALL(info->ubd, read_block, blockno, 1);
		if (!block_desc)
		{
			Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		blocknum %= n_per_block;
		return inode_nums[blocknum];
	}	
	else if (blocknum >= EXT2_NDIRECT)
	{
		blocknum -= EXT2_NDIRECT;
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_INDIRECT], 1));
		if (!block_desc)
		{
			Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		return inode_nums[blocknum];
	}
	else
		return file->f_inode.i_block[blocknum];
}

// Offset is a byte offset
static uint32_t ext2_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("EXT2DEBUG: ext2_get_file_block %p, %u\n", file, offset);
	return get_file_block(object, (ext2_fdesc_t *) file, offset);
}

static int fill_dirent(ext2_info_t * info, const EXT2_Dir_entry_t * dirfile, inode_t ino, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("EXT2DEBUG: %s inode number %u, %u\n", __FUNCTION__, ino, *basep);
	uint16_t namelen = MIN(dirfile->name_len, sizeof(entry->d_name) - 1);
	uint16_t reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;
	
	if (size < reclen || !basep)
		return -EINVAL;

	if (dirfile->rec_len == 0)
		return -1;

	// If the name length is 0 (or less?) then we assume it's an empty slot
	if (namelen < 1)
		return -1;

	entry->d_type = ext2_to_kfs_type(dirfile->file_type);

	//EXT2_inode_t inode;
	//if (ext2_get_inode(info, ino, &inode) < 0)
	//	return -1;

	entry->d_fileno = ino;
	//entry->d_filesize = inode.i_size;
	entry->d_reclen = reclen;
	entry->d_namelen = namelen;
	strncpy(entry->d_name, dirfile->name, namelen);
	entry->d_name[namelen] = 0;
	
	Dprintf("EXT2DEBUG: %s, created %s\n", __FUNCTION__, entry->d_name);
	return 0;
}

//TODO really, this shouldnt return inode == 0, since its annoying, but then to iterate to find free space its more work =(
static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent)
{
	Dprintf("EXT2DEBUG: %s %u\n", __FUNCTION__, *basep);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, file_blockno, num_file_blocks, block_offset;

	num_file_blocks = f->f_inode.i_blocks / (object->blocksize / 512);
	block_offset = *basep % object->blocksize;

	if (*basep >= f->f_inode.i_size)
		return -1; // should be: -ENOENT;

	blockno = *basep / object->blocksize;
	file_blockno = get_file_block(object, f, *basep);
	
	if (file_blockno == INVALID_BLOCK)
		return -1;

	dirblock = CALL(info->ubd, read_block, file_blockno, 1);
	if (!dirblock)
		return -1;
	
	*dirent = (EXT2_Dir_entry_t *) (dirblock->ddesc->data + block_offset);
	*basep += (*dirent)->rec_len;
	return 0;
}

static int ext2_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("EXT2DEBUG: ext2_get_dirent %p, %u\n", basep, *basep);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	const EXT2_Dir_entry_t * dirent;
	int r = 0;

	if (!basep || !file || !entry)
		return -1;

	if (f->f_type != TYPE_DIR)
		return -ENOTDIR;

	do {
		r = ext2_get_disk_dirent(object, f, basep, &dirent);
		if (r < 0)
			return r;
	} while (!dirent->inode); /* rec_len is zero if a dirent is used to fill a large gap */

	return fill_dirent(info, dirent, dirent->inode, entry, size, basep);
}

/* FIXME: this function does not deallocate blocks on failures */
static int ext2_append_file_block_set(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: %s %d\n", __FUNCTION__, block);
	struct ext2_info * info = (struct ext2_info *) object;
	const uint32_t n_per_block = object->blocksize / sizeof(uint32_t);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t nblocks;
	
	DEFINE_CHDESC_PASS_SET(set, 2, NULL);
	chdesc_pass_set_t * inode_dep = PASS_CHDESC_SET(set);
	set.array[0] = info->write_head ? *info->write_head : NULL;
	set.array[1] = NULL;
	/* we only need size 2 in some cases */
	set.size = 1;
	
	if(!tail || !f || block == INVALID_BLOCK)
		return -EINVAL;
	
	if(f->f_type == TYPE_SYMLINK)
		return -EINVAL;
	
	/* calculate current number of blocks */
	nblocks = f->f_inode.i_blocks / (object->blocksize / 512);
	if(nblocks > EXT2_NDIRECT)
		/* subtract the indirect block */
		if(--nblocks > EXT2_NDIRECT + n_per_block)
		{
			/* subtract the doubly indirect block */
			nblocks--;
			/* subtract all the additional indirect blocks */
			nblocks -= (nblocks - EXT2_NDIRECT) / (n_per_block + 1);
			/* FIXME: as long as we only support doubly indirect blocks,
			 * this is the maximum number of blocks we can use */
			if(nblocks > EXT2_NDIRECT + 1 + (n_per_block + 1) * (n_per_block + 1))
				return -EINVAL;
		}
	
	if(nblocks < EXT2_NDIRECT)
	{
		f->f_inode.i_block[nblocks] = block;
		inode_dep = befores;
	}
	else if(nblocks < EXT2_NDIRECT + n_per_block)
	{
		int r;
		bdesc_t * indirect;
		
		nblocks -= EXT2_NDIRECT;
		
		if(!nblocks)
		{
			/* allocate the indirect block */
			uint32_t blockno = ext2_allocate_block(object, file, PURPOSE_INDIRECT, &set.array[0]);
			if(blockno == INVALID_BLOCK)
				return -ENOSPC;
			indirect = ext2_synthetic_lookup_block(object, blockno);
			if(!indirect)
				return -ENOSPC;
			r = chdesc_create_init(indirect, info->ubd, &set.array[0]);
			if(r < 0)
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, set.array[0], "init indirect block");
			
			/* there are no references to the indirect block yet, so we can update it without depending on befores */
			r = chdesc_create_byte(indirect, info->ubd, 0, sizeof(uint32_t), &block, &set.array[0]);
			if(r < 0)
				return r;
			/* however, updating the inode itself must then depend on befores */
			set.next = befores;
			
			/* these changes will be written later, depending on inode_dep (set) */
			f->f_inode.i_blocks += object->blocksize / 512;
			f->f_inode.i_block[EXT2_INDIRECT] = blockno;
		}
		else
		{
			int offset = nblocks * sizeof(uint32_t);
			indirect = ext2_lookup_block(object, f->f_inode.i_block[EXT2_INDIRECT]);
			if(!indirect)
				return -ENOSPC;
			/* the indirect block is already referenced, so updating it has to depend on befores */
			r = chdesc_create_byte_set(indirect, info->ubd, offset, sizeof(uint32_t), &block, &set.array[0], befores);
			if(r < 0)
				return r;
		}
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, set.array[0], "add block");
		indirect->ddesc->flags |= BDESC_FLAG_INDIR;

		r = CALL(info->ubd, write_block, indirect);
		if (r < 0)
			return r;
	}
	else
	{
		int r, offset;
		bdesc_t * indirect = NULL;
		chdesc_t * indir_init = set.array[0]; /* write_head */
		bdesc_t * dindirect = NULL;
		chdesc_t * dindir_init = set.array[0]; /* write_head */
		
		nblocks -= EXT2_NDIRECT + n_per_block;
		
		if(!nblocks)
		{
			/* allocate and init doubly indirect block */
			uint32_t blockno = ext2_allocate_block(object, file, PURPOSE_DINDIRECT, &dindir_init);
			if(blockno == INVALID_BLOCK)
				return -ENOSPC;
			dindirect = ext2_synthetic_lookup_block(object, blockno);
			if(!dindirect)
				return -ENOSPC;
			r = chdesc_create_init(dindirect, info->ubd, &dindir_init);
			if(r < 0)
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, dindir_init, "init double indirect block");
			
			/* these changes will be written later, depending on inode_dep (set) */
			f->f_inode.i_blocks += object->blocksize / 512;
			f->f_inode.i_block[EXT2_DINDIRECT] = blockno;
		}
		else
		{
			dindirect = ext2_lookup_block(object, f->f_inode.i_block[EXT2_DINDIRECT]);
			if(!dindirect)
				return -ENOSPC;
		}
		dindirect->ddesc->flags |= BDESC_FLAG_INDIR;
		
		if(!(nblocks % n_per_block))
		{
			/* allocate and init indirect block */
			uint32_t blockno = ext2_allocate_block(object, file, PURPOSE_INDIRECT, &indir_init);
			if(blockno == INVALID_BLOCK)
				return -ENOSPC;
			indirect = ext2_synthetic_lookup_block(object, blockno);
			if(!indirect)
				return -ENOSPC;
			r = chdesc_create_init(indirect, info->ubd, &indir_init);
			if(r < 0)
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, indir_init, "init indirect block");
			
			set.next = befores;
			if(!nblocks)
			{
				/* in the case where we are also allocating the doubly indirect
				 * block, the inode can depend directly on everything and no
				 * dependencies are necessary between the other changes involved */
				set.array[1] = dindir_init;
				r = chdesc_create_byte(dindirect, info->ubd, 0, sizeof(uint32_t), &blockno, &set.array[1]);
			}
			else
			{
				offset = (nblocks / n_per_block) * sizeof(uint32_t);
				set.array[0] = indir_init;
				r = chdesc_create_byte_set(dindirect, info->ubd, offset, sizeof(uint32_t), &blockno, &set.array[1], PASS_CHDESC_SET(set));
				set.next = NULL;
			}
			if(r < 0)
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, set.array[1], "add indirect block");
			
			/* the cases involving allocating an indirect block require a larger set */
			set.size = 2;
			
			/* this change will be written later, depending on inode_dep (set) */
			f->f_inode.i_blocks += object->blocksize / 512;
			
			set.array[0] = indir_init;
			r = chdesc_create_byte(indirect, info->ubd, 0, sizeof(uint32_t), &block, &set.array[0]);
			if(r < 0)
				return r;
		}
		else
		{
			offset = nblocks / n_per_block;
			indirect = ext2_lookup_block(object, ((uint32_t *) dindirect->ddesc->data)[offset]);
			if(!indirect)
				return -ENOSPC;
			offset = (nblocks % n_per_block) * sizeof(uint32_t);
			r = chdesc_create_byte_set(indirect, info->ubd, offset, sizeof(uint32_t), &block, &set.array[0], befores);
			if(r < 0)
				return r;
		}
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, set.array[0], "add block");
		indirect->ddesc->flags |= BDESC_FLAG_INDIR;
		
		r = CALL(info->ubd, write_block, indirect);
		if(r < 0)
			return r;
		
		if(!(nblocks % n_per_block))
		{
			/* we write this one second since it probably
			 * should be written second (to the disk) */
			r = CALL(info->ubd, write_block, dindirect);
			if(r < 0)
				return r;
		}
	}
	
	/* increment i_blocks for the block itself */
	f->f_inode.i_blocks += object->blocksize / 512;
	return ext2_write_inode_set(info, f, tail, inode_dep);
}

static int ext2_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return ext2_append_file_block_set(object, file, block, head, PASS_CHDESC_SET(set));
}

static int ext2_write_dirent_extend_set(LFS_t * object, ext2_fdesc_t * parent,
                                        EXT2_Dir_entry_t * dirent_exists,
                                        EXT2_Dir_entry_t * dirent_new, uint32_t basep,
                                        chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno;
	bdesc_t * dirblock;
	int r;
	//check: off end of file?
	if (!parent || !dirent_exists || !dirent_new || !tail)
		return -EINVAL;

	if (basep + dirent_exists->rec_len + dirent_new->rec_len > parent->f_inode.i_size)
		return -EINVAL;

	uint32_t exists_rec_len_actual = dirent_rec_len(dirent_exists->name_len);
	uint32_t new_rec_len_actual = dirent_rec_len(dirent_new->name_len);

	// dirents are in a single block:
	if (basep % object->blocksize + exists_rec_len_actual + new_rec_len_actual <= object->blocksize) {
		EXT2_Dir_entry_t entries[2];

		//it would be brilliant if we could cache this, and not call get_file_block, read_block =)
		blockno = get_file_block(object, parent, basep);
		if (blockno == INVALID_BLOCK)
			return -1;

		basep %= object->blocksize;

		dirblock = CALL(info->ubd, read_block, blockno, 1);
		if (!dirblock)
			return -1;

		memcpy(entries, dirent_exists, exists_rec_len_actual);
		memcpy((void *) entries + exists_rec_len_actual, dirent_new, new_rec_len_actual);

		if ((r = chdesc_create_byte_set(dirblock, info->ubd, basep, exists_rec_len_actual + new_rec_len_actual, (void *) entries, tail, befores )) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *tail, "write dirent '%s'", dirent_new->name);
		dirblock->ddesc->flags |= BDESC_FLAG_DIRENT;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;
	} else
		kpanic("overlapping dirent");
	return 0;
}

static int ext2_write_dirent_set(LFS_t * object, ext2_fdesc_t * parent, EXT2_Dir_entry_t * dirent,
                                 uint32_t basep, chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno;
	bdesc_t * dirblock;
	int r;
	//check: off end of file?
	if (!parent || !dirent || !tail)
		return -EINVAL;

	if (basep + dirent->rec_len > parent->f_inode.i_size)
		return -EINVAL;

	//dirent is in a single block:
	uint32_t actual_rec_len = dirent_rec_len(dirent->name_len);
	if (basep % object->blocksize + actual_rec_len <= object->blocksize) {
		//it would be brilliant if we could cache this, and not call get_file_block, read_block =)
		blockno = get_file_block(object, parent, basep);
		if (blockno == INVALID_BLOCK)
			return -1;

		basep %= object->blocksize;

		dirblock = CALL(info->ubd, read_block, blockno, 1);
		if (!dirblock)
			return -1;

		if ((r = chdesc_create_byte_set(dirblock, info->ubd, basep, actual_rec_len, (void *) dirent, tail, befores)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *tail, "write dirent '%s'", dirent->name);
		dirblock->ddesc->flags |= BDESC_FLAG_DIRENT;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;
	} else
		kpanic("overlapping dirent");
	return 0;
}

static int ext2_write_dirent(LFS_t * object, ext2_fdesc_t * parent, EXT2_Dir_entry_t * dirent,
				 uint32_t basep, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return ext2_write_dirent_set(object, parent, dirent, basep, head, PASS_CHDESC_SET(set));
}

static int ext2_insert_dirent_set(LFS_t * object, ext2_fdesc_t * parent, EXT2_Dir_entry_t * new_dirent, chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: ext2_insert_dirent %s\n", new_dirent->name);
	const EXT2_Dir_entry_t * entry;
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t prev_eof = parent->f_inode.i_size, new_block;
	ext2_mdir_t * mdir;
	int r = 0, newdir = 0;
	bdesc_t * block;
	chdesc_t * append_chdesc;
	DEFINE_CHDESC_PASS_SET(set, 1, befores);
	set.array[0] = NULL;
	
	if (parent->f_inode.i_size == 0)
		newdir = 1;
	
	r = ext2_mdir_get(object, parent, &mdir);
	if(r < 0)
		return r;
	
	if(!newdir)
	{
		ext2_mdirent_t * mdirent = mdir->free_first;
		for(; mdirent; mdirent = mdirent->freel.next)
		{
			if(!mdirent->dirent.inode && mdirent->dirent.rec_len >= new_dirent->rec_len)
			{
				uint32_t offset = mdirent->offset;
				new_dirent->rec_len = mdirent->dirent.rec_len;
				r = ext2_mdirent_use(mdir, mdirent, new_dirent);
				if(r < 0)
					return r;
				r = ext2_write_dirent_set(object, parent, new_dirent, offset, tail, befores);
				if(r < 0)
					ext2_mdirent_clear(mdir, mdirent, object->blocksize);
				return r;
			}
			if(mdirent->dirent.inode && mdirent->dirent.rec_len - (8 + mdirent->dirent.name_len) > new_dirent->rec_len)
			{
				EXT2_Dir_entry_t entry_updated;
				uint16_t entry_updated_len;
				uint32_t existing_offset = mdirent->offset;
				uint32_t new_offset;
				uint16_t backup_rec_len = new_dirent->rec_len;
				r = ext2_get_disk_dirent(object, parent, &existing_offset, &entry);
				if(r < 0)
					return r;
				existing_offset = mdirent->offset;
				memcpy(&entry_updated, entry, MIN(entry->rec_len, sizeof(entry_updated)));
				entry_updated_len = dirent_rec_len(entry_updated.name_len);
				new_dirent->rec_len = entry_updated.rec_len - entry_updated_len;
				entry_updated.rec_len = entry_updated_len;
				
				new_offset = existing_offset + entry_updated.rec_len;
				r = ext2_mdirent_split(mdir, mdirent, &entry_updated, new_dirent);
				if(r < 0)
				{
					new_dirent->rec_len = backup_rec_len;
					return r;
				}
				r = ext2_write_dirent_extend_set(object, parent, &entry_updated, new_dirent, existing_offset, tail, befores);
				if(r < 0)
					assert(0); // TODO: join the existing and new mdirents
				return r;
			}
		}
	}
	
	//test the aligned case! test by having a 16 whatever file
	new_block = ext2_allocate_block(object, (fdesc_t *) parent, PURPOSE_DIRDATA, &set.array[0]);
	if (new_block == INVALID_BLOCK)
		return -ENOSPC;
	/* FIXME: these errors should all free the block we allocated! */
	block = CALL(info->ubd, synthetic_read_block, new_block, 1);
	if (block == NULL)
		return -ENOSPC;
	r = chdesc_create_init(block, info->ubd, &set.array[0]);
	if (r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, set.array[0], "init new dirent block");
	block->ddesc->flags |= BDESC_FLAG_DIRENT;
	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;
	parent->f_inode.i_size += object->blocksize;
	r = ext2_append_file_block_set(object, (fdesc_t *) parent, new_block, &append_chdesc, PASS_CHDESC_SET(set));
	if (r < 0)
		return r;
	lfs_add_fork_head(append_chdesc);
	
	new_dirent->rec_len = object->blocksize;
	r = ext2_mdirent_add(mdir, new_dirent, prev_eof);
	if(r < 0)
		return r;
	r = ext2_write_dirent_set(object, parent, new_dirent, prev_eof, tail, PASS_CHDESC_SET(set));
	assert(r >= 0); // need to undo ext2_dir_add()
	return r;
}

static int find_free_inode_block_group(LFS_t * object, inode_t * ino) {
	Dprintf("EXT2DEBUG: %s inode number is %u\n", __FUNCTION__, *ino);
	struct ext2_info * info = (struct ext2_info *) object;
	bdesc_t * bitmap;
	inode_t curr = 0;

	if (*ino > info->super->s_inodes_count)
	{
		printf("%s requested status of inode %u too large!\n",__FUNCTION__, *ino);
		return -ENOSPC;
	}
	
	curr = *ino;
	
	uint32_t block_group = curr / info->super->s_inodes_per_group;
	
	
	short firstrun = 1;
	while(block_group != ( (*ino) / info->super->s_inodes_per_group) || firstrun) {
		if(info->inode_gdesc != block_group || info->inode_cache == NULL) {
			if (info->inode_cache != NULL)
				bdesc_release(&info->inode_cache);	
			info->inode_gdesc = block_group;
			bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_inode_bitmap, 1);
			if (!bitmap)
				return -ENOSPC;
			bdesc_retain(bitmap);
			bitmap->ddesc->flags |= BDESC_FLAG_BITMAP;
			info->inode_cache = bitmap;
		}
		
		const unsigned long * array = (unsigned long *) info->inode_cache->ddesc->data;
		//assert((curr % info->super->s_inodes_per_group) == 0);
		int index = find_first_zero_bit(array, info->super->s_inodes_per_group/*, (curr % info->super->s_inodes_per_group)*/ );
		if (index < (info->super->s_inodes_per_group)) {
			curr += index + 1;
			*ino = curr;
			//printf("returning inode number %d\n",*ino);
			return EXT2_FREE;
		}
		
		firstrun = 0;
		block_group = (block_group + 1) % info->ngroups;
		curr = block_group * info->super->s_inodes_per_group;	
	}
		

	return -ENOSPC;
}

static inode_t ext2_find_free_inode(LFS_t * object, inode_t parent) {
	Dprintf("EXT2DEBUG: %s parent is %u\n", __FUNCTION__, parent);
	struct ext2_info * info = (struct ext2_info *) object;
	inode_t ino = 0;
	int r;
	
	ino = (parent / info->super->s_inodes_per_group) * info->super->s_inodes_per_group;
	r = find_free_inode_block_group(object, &ino);
	if (r != -ENOSPC) {
		return ino;
	}
	
	return EXT2_BAD_INO;
}

static fdesc_t * ext2_allocate_name(LFS_t * object, inode_t parent_ino, const char * name,
                                    uint8_t type, fdesc_t * link, const metadata_set_t * initialmd,
                                    inode_t * new_ino, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_allocate_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * parent_file = NULL, * new_file = NULL;
	uint16_t mode;
	int r;
	ext2_fdesc_t * ln = (ext2_fdesc_t *) link;
	EXT2_Dir_entry_t new_dirent;
	char * link_buf = NULL;
	DEFINE_CHDESC_PASS_SET(head_set, 4, NULL);

	//what is link? link is a symlink fdesc. dont deal with it, yet.
	if (!head || strlen(name) > EXT2_NAME_LEN)
		return NULL;

	//TODO: we need some way to prevent regular users from creating . and ..

	switch (type)
	{
		case TYPE_FILE:
			mode = EXT2_S_IFREG;
			break;
		case TYPE_DIR:
			mode = EXT2_S_IFDIR;
			break;
		case TYPE_SYMLINK:
			mode = EXT2_S_IFLNK;
			break;
		default:
			return NULL;
	}

	// Don't link files of different types
	if (ln && type != ln->f_type)
		return NULL;

	//this might be redundent:
	parent_file = (ext2_fdesc_t *) ext2_lookup_inode(object, parent_ino);
	if (!parent_file)
		return NULL;
	
	if (!ln) {
		inode_t ino = ext2_find_free_inode(object, parent_ino);
		uint32_t x32;
		uint16_t x16;

		if (ino == EXT2_BAD_INO)
			goto allocate_name_exit;

		new_file = ext2_fdesc_alloc();
		if (!new_file)
			goto allocate_name_exit;

		new_file->common = &new_file->base;
		new_file->base.parent = INODE_NONE;
		new_file->f_inode_cache = NULL;
		new_file->f_nopen = 1;
#if !ROUND_ROBIN_ALLOC
		new_file->f_lastblock = 0;
#endif
		new_file->f_ino = ino;
		new_file->f_type = type;

		memset(&new_file->f_inode, 0, sizeof(struct EXT2_inode));
		
		r = hash_map_insert(info->filemap, (void *) ino, new_file);
		if(r < 0)
			goto allocate_name_exit2;
		assert(r == 0);

		r = initialmd->get(initialmd->arg, KFS_FEATURE_UID, sizeof(x32), &x32);
		if (r > 0)
			new_file->f_inode.i_uid = x32;
		else if (r == -ENOENT)
			new_file->f_inode.i_uid = 0;
		else
			assert(0);

		r = initialmd->get(initialmd->arg, KFS_FEATURE_GID, sizeof(x32), &x32);
		if (r > 0)
			new_file->f_inode.i_gid = x32;
		else if (r == -ENOENT)
			new_file->f_inode.i_gid = 0;
		else
			assert(0);

		new_file->f_inode.i_mode = mode | EXT2_S_IRUSR | EXT2_S_IWUSR;

		r = initialmd->get(initialmd->arg, KFS_FEATURE_UNIX_PERM, sizeof(x16), &x16);
		if (r > 0)
			new_file->f_inode.i_mode |= x16;
		else if (r != -ENOENT)
			assert(0);

		new_file->f_inode.i_links_count = 1;

		head_set.array[1] = info->write_head ? *info->write_head : NULL;
		r = ext2_write_inode_bitmap(object, ino, 1, &head_set.array[1]);
		if (r != 0)
			goto allocate_name_exit2;

		if (type == TYPE_SYMLINK) {
			link_buf = malloc(object->blocksize);
			if (!link_buf) {
				r = -ENOMEM;
				goto allocate_name_exit2;
			}
			r = initialmd->get(initialmd->arg, KFS_FEATURE_SYMLINK, object->blocksize, link_buf);
			if (r < 0)
				goto allocate_name_exit2;
			else {
				r = ext2_set_metadata(object, new_file, KFS_FEATURE_SYMLINK, r, link_buf, &head_set.array[1]);
				if (r < 0)
					goto allocate_name_exit2;
			}
			
		}
		else if (type == TYPE_DIR) {
			// Create . and ..
			uint32_t dirblock_no;
			bdesc_t * dirblock_bdesc;
			chdesc_t * init_head;
			EXT2_Dir_entry_t dir_dirent;
			uint32_t prev_basep;
			DEFINE_CHDESC_PASS_SET(dotdot_befores, 2, NULL);
			uint32_t group;

			// allocate and append first directory entry block
			dirblock_no = ext2_allocate_block(object, (fdesc_t *) new_file, 1, &init_head);
			dirblock_bdesc = CALL(info->ubd, synthetic_read_block, dirblock_no, 1);
			r = chdesc_create_init(dirblock_bdesc, info->ubd, &init_head);
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, init_head, "init new dirent block");
			new_file->f_inode.i_block[0] = dirblock_no;
			new_file->f_inode.i_size = object->blocksize;
			new_file->f_inode.i_blocks = object->blocksize / 512;

			// insert "."
			dir_dirent.inode = ino;
			strcpy(dir_dirent.name, ".");
			dir_dirent.name_len = strlen(dir_dirent.name);
			dir_dirent.rec_len = dirent_rec_len(dir_dirent.name_len);
			dir_dirent.file_type = EXT2_TYPE_DIR;
			head_set.array[2] = init_head;
			r = chdesc_create_byte(dirblock_bdesc, info->ubd, 0, dir_dirent.rec_len, &dir_dirent, &head_set.array[2]);
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head_set.array[2], "write dirent '.'");
			new_file->f_inode.i_links_count++;
			prev_basep = dir_dirent.rec_len;

			parent_file->f_inode.i_links_count++;
			head_set.array[3] = info->write_head ? *info->write_head : NULL;
			r = ext2_write_inode(info, parent_file, &head_set.array[3]);
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head_set.array[3], "linkcount++");

			// insert ".."
			dir_dirent.inode = parent_ino;
			strcpy(dir_dirent.name, "..");
			dir_dirent.name_len = strlen(dir_dirent.name);
			dir_dirent.rec_len = object->blocksize - prev_basep;
			dir_dirent.file_type = EXT2_TYPE_DIR;
			dotdot_befores.array[0] = init_head;
			// we needn't depend on links_count++, but any later files in this dir probably will
			// and having this dependency now makes merging easier
			dotdot_befores.array[1] = head_set.array[3];
			r = chdesc_create_byte_set(dirblock_bdesc, info->ubd, prev_basep, dirent_rec_len(dir_dirent.name_len), &dir_dirent, &head_set.array[4], PASS_CHDESC_SET(dotdot_befores));
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head_set.array[4], "write dirent '..'");
			prev_basep = dir_dirent.rec_len;

			dirblock_bdesc->ddesc->flags |= BDESC_FLAG_DIRENT;
			r = CALL(info->ubd, write_block, dirblock_bdesc);

			group = (new_file->f_ino - 1) / info->super->s_inodes_per_group;
			r = ext2_super_report(object, group, 0, 0, 1);
			if (r < 0)
				goto allocate_name_exit2;
		}

		r = ext2_write_inode(info, new_file, &head_set.array[1]);
		if (r < 0)
			goto allocate_name_exit2;

		*new_ino = ino;
	} else {
		new_file = (ext2_fdesc_t *) ext2_lookup_inode(object, ln->f_ino);
		
		assert(ln == new_file);
		if (!new_file)
			goto allocate_name_exit;
		*new_ino = ln->f_ino;

		// Increase link count
		ln->f_inode.i_links_count++;
		r = ext2_write_inode(info, ln, &head_set.array[0]);
		if (r < 0)
			goto allocate_name_exit2;
	}

	// create the directory entry
	new_dirent.inode = *new_ino;
	strncpy(new_dirent.name, name, EXT2_NAME_LEN);
	new_dirent.name_len = strlen(name);
	//round len up to multiple of 4 bytes:
	//(this value just computed for searching for a slot)
	new_dirent.rec_len = dirent_rec_len(new_dirent.name_len);
	switch(type) {
		case(TYPE_DIR):
			new_dirent.file_type = EXT2_TYPE_DIR;
			break;
		case(TYPE_FILE):
			new_dirent.file_type = EXT2_TYPE_FILE;
			break;
		case(TYPE_SYMLINK):
			new_dirent.file_type = EXT2_TYPE_SYMLINK;
			break;
		default: //TODO: add more types
			new_dirent.file_type = EXT2_TYPE_FILE;
	}

	head_set.array[0] = *head;
	r = ext2_insert_dirent_set(object, parent_file, &new_dirent, head, PASS_CHDESC_SET(head_set));
	if (r < 0) {
		printf("Inserting a dirent in allocate_name failed for \"%s\"!\n", name);
		goto allocate_name_exit2;
	}

	ext2_free_fdesc(object, (fdesc_t *)parent_file);
	return (fdesc_t *)new_file;

allocate_name_exit2:
	free(link_buf);
	ext2_free_fdesc(object, (fdesc_t *)new_file);

allocate_name_exit:
	ext2_free_fdesc(object, (fdesc_t *)parent_file);
	return NULL;
}

static uint32_t ext2_erase_block_ptr(LFS_t * object, EXT2_inode_t * inode, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, inode, inode->i_size);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blocknum, n_per_block;
	bdesc_t * block_desc, * double_block_desc;
	uint32_t * block_nums,* double_block_nums, indir_ptr, double_indir_ptr;
	int r;
	uint32_t target = INVALID_BLOCK;

	n_per_block = object->blocksize / (sizeof(uint32_t));

	//non block aligned offsets suck (aka aren't supported)

	if (inode->i_size <= object->blocksize)
		blocknum = 0;
	else if ( (inode->i_size % object->blocksize) == 0)
		blocknum = (inode->i_size / object->blocksize) - 1;
	else
		blocknum = inode->i_size / object->blocksize;

	if (blocknum < EXT2_NDIRECT)
	{
		target = inode->i_block[blocknum];
		inode->i_block[blocknum] = 0;
		if (inode->i_size > object->blocksize)
			inode->i_size = inode->i_size - object->blocksize;
		else
			inode->i_size = 0;

	}
	else if (blocknum < EXT2_NDIRECT + n_per_block)
	{
		blocknum -= EXT2_NDIRECT;
		block_desc = (CALL(info->ubd, read_block, inode->i_block[EXT2_INDIRECT], 1));
		if (!block_desc)
			return INVALID_BLOCK;
		block_nums = (uint32_t *)block_desc->ddesc->data;
		target = block_nums[blocknum];

		if (blocknum == 0)
		{
			indir_ptr = inode->i_block[EXT2_INDIRECT];
			if (inode->i_size > object->blocksize)
				inode->i_size = inode->i_size - object->blocksize;
			else
				inode->i_size = 0;
			r = _ext2_free_block(object, indir_ptr, head);
			if (r < 0)
				return INVALID_BLOCK;
			inode->i_blocks -= object->blocksize / 512;
			inode->i_block[EXT2_INDIRECT] = 0;
		} else {
			if (inode->i_size > object->blocksize)
				inode->i_size -= object->blocksize;
			else
				inode->i_size = 0;
			//r = chdesc_create_byte(block_desc, info->ubd, blocknum * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
			//if (r < 0)
			//	return INVALID_BLOCK;
			//r = CALL(info->ubd, write_block, block_desc);
			//if (r < 0)
			//	return INVALID_BLOCK;
		}
	}
	else if (blocknum < EXT2_NDIRECT + n_per_block + n_per_block * n_per_block)
	{
		blocknum -= (EXT2_NDIRECT + n_per_block);
		block_desc = (CALL(info->ubd, read_block, inode->i_block[EXT2_DINDIRECT], 1));
		if (!block_desc)
			return INVALID_BLOCK;
		block_nums = (uint32_t *)block_desc->ddesc->data;
		indir_ptr = block_nums[blocknum / n_per_block];
		double_block_desc = CALL(info->ubd, read_block, indir_ptr, 1);
		if (!block_desc)
			return INVALID_BLOCK;
		double_block_nums = (uint32_t *)double_block_desc->ddesc->data;
		double_indir_ptr = (blocknum % n_per_block);
		target = double_block_nums[double_indir_ptr];

		if (inode->i_size > object->blocksize)
			inode->i_size -= object->blocksize;
		else
			inode->i_size = 0;

		if (blocknum % n_per_block == 0)
		{
			if (blocknum == 0)
			{
				r = _ext2_free_block(object, inode->i_block[EXT2_DINDIRECT], head);
				if (r < 0)
					return INVALID_BLOCK;
				inode->i_blocks -= object->blocksize / 512;
				inode->i_block[EXT2_DINDIRECT] = 0;
			}
			else
			{
				//r = chdesc_create_byte(block_desc, info->ubd, (blocknum / n_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
				//if (r < 0)
				//	return INVALID_BLOCK;
				//r = CALL(info->ubd, write_block, block_desc);
				//if (r < 0)
				//	return INVALID_BLOCK;
			}
			r = _ext2_free_block(object, indir_ptr, head);
			if (r < 0)
				return INVALID_BLOCK;
			inode->i_blocks -= object->blocksize / 512;
		}
		else
		{
			//r = chdesc_create_byte(double_block_desc, info->ubd, (blocknum % n_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
			//if (r < 0)
			//	return INVALID_BLOCK;
			//r = CALL(info->ubd, write_block, double_block_desc);
			//if (r < 0)
			//	return INVALID_BLOCK;
		}
	}
	else
	{
		Dprintf("Triply indirect blocks are not implemented.\n");
		assert(0);
	}
	return target;
}

static uint32_t ext2_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_truncate_file_block\n");
	int r;
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;

	if (!f || f->f_inode.i_blocks == 0 || f->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;

	if (f->f_inode.i_size == 0)
		return INVALID_BLOCK;

	// Update ext2_mdir code if we want to directory truncation
	assert(f->f_type != TYPE_DIR);

	// FIXME: need to do [d]indirect block count decrement, and write it, here!
	f->f_inode.i_blocks -= object->blocksize / 512;
	r = ext2_write_inode(info, f, head);
	if (r < 0)
		return INVALID_BLOCK;

	//ext2_erase_block_ptr will either return INVALID_BLOCK, or the block that was truncated...
	return ext2_erase_block_ptr(object, &f->f_inode, head);
}

static int empty_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	return -ENOENT;
}

// FIXME: directory rename is incorrect (eg parent linkcounts are not updated)
static int ext2_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_rename %u:%s -> %u:%s\n", oldparent, oldname, newparent, newname);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_mdir_t * omdir, * nmdir;
	ext2_mdirent_t * omdirent, * nmdirent;
	ext2_fdesc_t * fold, * fnew, * foparent, * fnparent;
	bool existing = 0;
	inode_t newino;
	chdesc_t * prev_head = NULL;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };
	int r;
	
	if (!head)
		return -EINVAL;
	if (strlen(oldname) > EXT2_NAME_LEN || strlen(newname) > EXT2_NAME_LEN)
		return -EINVAL;

	if (oldparent == newparent && !strcmp(oldname, newname))
		return 0;

	foparent = (ext2_fdesc_t *) ext2_lookup_inode(object, oldparent);
	if (!foparent)
		return -ENOENT;
	r = ext2_mdir_get(object, foparent, &omdir);
	if (r < 0)
		goto exit_foparent;
	omdirent = ext2_mdirent_get(omdir, oldname);
	if (!omdirent)
	{
		r = -ENOENT;
		goto exit_foparent;
	}
	fold = (ext2_fdesc_t *) ext2_lookup_inode(object, omdirent->dirent.inode);
	if (!fold)
	{
		r = -ENOENT;
		goto exit_foparent;
	}

	fnparent = (ext2_fdesc_t *) ext2_lookup_inode(object, newparent);
	if (!fnparent)
	{
		r = -ENOENT;
		goto exit_fold;
	}
	r = ext2_mdir_get(object, fnparent, &nmdir);
	if (r < 0)
		goto exit_fold;
	nmdirent = ext2_mdirent_get(nmdir, newname);
	if (nmdirent)
		fnew = (ext2_fdesc_t *) ext2_lookup_inode(object, nmdirent->dirent.inode);
	else
		fnew = NULL;

	if (fnew)
	{
		EXT2_Dir_entry_t copy;

		// Overwriting a directory makes little sense
		if (fnew->f_type == TYPE_DIR)
		{
			r = -ENOTEMPTY;
			goto exit_fnew;
		}

		memcpy(&copy, &nmdirent->dirent, MIN(nmdirent->dirent.rec_len, sizeof(copy)));
		copy.inode = fold->f_ino;

		// File already exists
		existing = 1;

		r = ext2_write_dirent(object, fnparent, &copy, nmdirent->offset, head);
		if (r < 0)
			goto exit_fnew;
		prev_head = *head;
		nmdirent->dirent.inode = copy.inode;

		fold->f_inode.i_links_count++;
		r = ext2_write_inode(info, fold, head);
		assert(r >= 0); // recover mdir and mdirent changes; then exit_fnew
	}
	else
	{
		// Link files together
		fnew = (ext2_fdesc_t *) ext2_allocate_name(object, newparent, newname, fold->f_type, (fdesc_t *) fold, &emptymd, &newino, head);
		if (!fnew)
		{
			r = -1;
			goto exit_fnparent;
		}
		//assert(new_dirent->inode == newino);
	}

	r = ext2_delete_dirent(object, foparent, omdir, omdirent, head);
	if (r < 0)
		goto exit_fnew;

	fold->f_inode.i_links_count--;
	r = ext2_write_inode(info, fold, head);
	if (r < 0)
		goto exit_fnew;

	if (existing)
	{
		fnew->f_inode.i_links_count--;
		r = ext2_write_inode(info, fnew, &prev_head);
		if (r < 0)
			goto exit_fnew;

		if (fnew->f_inode.i_links_count == 0)
		{
			uint32_t i, n = ext2_get_file_numblocks(object, (fdesc_t *) fnew);
			for (i = 0; i < n; i++)
			{
				uint32_t block = ext2_truncate_file_block(object, (fdesc_t *) fnew, &prev_head);
				if (block == INVALID_BLOCK)
				{
					r = -1;
					goto exit_fnew;
				}
				r = _ext2_free_block(object, block, &prev_head);
				if (r < 0)
					goto exit_fnew;
			}

			memset(&fnew->f_inode, 0, sizeof(EXT2_inode_t));
			r = ext2_write_inode(info, fnew, &prev_head);
			if (r < 0)
				goto exit_fnew;

			r = ext2_write_inode_bitmap(object, fnew->f_ino, 0, &prev_head);
			if (r < 0)
				goto exit_fnew;
			lfs_add_fork_head(prev_head);
		}
	}

	r = 0;

  exit_fnew:
	ext2_free_fdesc(object, (fdesc_t *) fnew);
  exit_fnparent:
	ext2_free_fdesc(object, (fdesc_t *) fnparent);
  exit_fold:
	ext2_free_fdesc(object, (fdesc_t *) fold);
  exit_foparent:
	ext2_free_fdesc(object, (fdesc_t *) foparent);
	return r;
}

static int _ext2_free_block(LFS_t * object, uint32_t block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_free_block\n");
	int r;

	if(!head || block == INVALID_BLOCK)
		return -EINVAL;
	
	r = ext2_write_block_bitmap(object, block, 0, head);
	if (r < 0)
	{
		Dprintf("failed to free block %d in bitmap\n", block);
		return r;
	}
	
	return r;
}

static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	return _ext2_free_block(object, block, head);
}

static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_delete_dirent %u\n", mdirent->offset);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t base = mdirent->offset, prev_base;
	uint32_t base_blockno, prev_base_blockno;
	bdesc_t * dirblock;
	uint16_t len;
	int r;

	if(base % object->blocksize == 0)
	{
		//if the base is at the start of a block, zero it out
		EXT2_Dir_entry_t jump_dirent;
		const EXT2_Dir_entry_t * disk_dirent;
		base_blockno = get_file_block(object, dir_file, base);
		if(base_blockno == INVALID_BLOCK)
			return -1;
		dirblock = CALL(info->ubd, read_block, base_blockno, 1);
		if(!dirblock)
			return -EIO;
		disk_dirent = (const EXT2_Dir_entry_t *) dirblock->ddesc->data;
		jump_dirent.inode = 0;
		jump_dirent.rec_len = disk_dirent->rec_len;
		r = chdesc_create_byte(dirblock, info->ubd, 0, 6, &jump_dirent, head);
		if(r < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "delete dirent, add jump dirent");
		r = CALL(info->ubd, write_block, dirblock);
		if(r >= 0)
			ext2_mdirent_clear(mdir, mdirent, object->blocksize);
		else
		{
			assert(0); // must undo chdesc creation to recover
			return r;
		}
		return 0;
	}

	//else in the middle of a block, so increase length of prev dirent
	prev_base = ext2_mdirent_offset_prev(mdir, mdirent)->offset;
	prev_base_blockno = get_file_block(object, dir_file, prev_base);
	if(prev_base_blockno == INVALID_BLOCK)
		return -1;
	dirblock = CALL(info->ubd, read_block, prev_base_blockno, 1);
	if(!dirblock)
		return -1;

	//update the length of the previous dirent:
	len = mdirent->dirent.rec_len + ext2_mdirent_offset_prev(mdir, mdirent)->dirent.rec_len;
	r = chdesc_create_byte(dirblock, info->ubd, (prev_base + 4) % object->blocksize, sizeof(len), (void *) &len, head);
	if(r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "delete dirent");
	
	r = CALL(info->ubd, write_block, dirblock);
	if(r >= 0)
		ext2_mdirent_clear(mdir, mdirent, object->blocksize);
	else
	{
		assert(0); // must undo chdesc creation to recover
		return r;
	}
	return 0;
}

static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_remove_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) object;
	chdesc_t * prev_head;
	ext2_fdesc_t * pfile = NULL, * file = NULL;
	uint8_t minlinks = 1;
	ext2_mdir_t * mdir;
	ext2_mdirent_t * mdirent;
	int r;

	if (!head)
		return -EINVAL;

	pfile = (ext2_fdesc_t *) ext2_lookup_inode(object, parent);
	if (!pfile)
		return -EINVAL;

	if (pfile->f_type != TYPE_DIR) {
		r = -ENOTDIR;
		goto remove_name_exit;
	}	
	
	r = ext2_mdir_get(object, pfile, &mdir);
	if(r < 0)
		goto remove_name_exit;
	mdirent = ext2_mdirent_get(mdir, name);
	if(!mdirent)
		goto remove_name_exit;
	file = (ext2_fdesc_t *) ext2_lookup_inode(object, mdirent->dirent.inode);
	if(!file)
		goto remove_name_exit;

	if (file->f_type == TYPE_DIR) {
		if (file->f_inode.i_links_count > 2 && !strcmp(name, "..")) {
			r = -ENOTEMPTY;
			goto remove_name_exit;
		}
		else if (file->f_inode.i_links_count < 2) {
			Dprintf("%s warning, directory with %d links\n", __FUNCTION__, file->f_inode.i_links_count);
			minlinks = file->f_inode.i_links_count;
		}
		else
			minlinks = 2;
	}

	r = ext2_delete_dirent(object, pfile, mdir, mdirent, head);
	if (r < 0)
		goto remove_name_exit;
	assert(file->f_inode.i_links_count >= minlinks);

	/* remove link to parent directory */
	if (file->f_type == TYPE_DIR) {
		pfile->f_inode.i_links_count--;
		prev_head = *head;
		r = ext2_write_inode(info, pfile, &prev_head);
		if (r < 0)
			goto remove_name_exit;
		lfs_add_fork_head(prev_head);
	}
	
	if (file->f_inode.i_links_count == minlinks) {
		/* need to free the inode */
		uint32_t number, nblocks, j, group;
		EXT2_inode_t inode = file->f_inode;
		group = (file->f_ino - 1) / info->super->s_inodes_per_group;
		nblocks = ext2_get_file_numblocks(object, (fdesc_t *) file);

		if(file->f_type == TYPE_DIR)
			ext2_mdir_remove(object, file->f_ino);
		
		memset(&file->f_inode, 0, sizeof(EXT2_inode_t));
		r = ext2_write_inode(info, file, head);
		if (r < 0)
			goto remove_name_exit;

		prev_head = *head;
		r = ext2_write_inode_bitmap(object, file->f_ino, 0, &prev_head);
		if (r < 0)
			goto remove_name_exit;
		lfs_add_fork_head(prev_head);
		
		for (j = 0; j < nblocks; j++) {
			prev_head = *head;
			number = ext2_erase_block_ptr(object, &inode, &prev_head);
			if (number == INVALID_BLOCK) {
				r = -EINVAL;
				goto remove_name_exit;
			}
			lfs_add_fork_head(prev_head);

			prev_head = *head;
			r = _ext2_free_block(object, number, &prev_head);
			if (r < 0)
				goto remove_name_exit;
			lfs_add_fork_head(prev_head);
		}
		if(file->f_type == TYPE_DIR) {
			r = ext2_super_report(object, group, 0, 0, -1);
			if(r < 0)
				goto remove_name_exit;				
		}
	} else {
		file->f_inode.i_links_count--;
		r = ext2_write_inode(info, file, head);
		if (r < 0)
			goto remove_name_exit;
	}


	r = 0;

remove_name_exit:
	ext2_free_fdesc(object, (fdesc_t *) pfile);
	ext2_free_fdesc(object, (fdesc_t *) file);
	return r;
}

static int ext2_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_write_block\n");
	struct ext2_info * info = (struct ext2_info *) object;

	if (!head)
		return -EINVAL;

	return CALL(info->ubd, write_block, block);
}

static chdesc_t ** ext2_get_write_head(LFS_t * object)
{
	Dprintf("EXT2DEBUG: ext2_get_write_head\n");
	struct ext2_info * info = (struct ext2_info *) object;
	return info->write_head;
}

static int32_t ext2_get_block_space(LFS_t * object)
{
	Dprintf("EXT2DEBUG: ext2_get_block_space\n");
	struct ext2_info * info = (struct ext2_info *) object;
	return CALL(info->ubd, get_block_space);
}

static const bool ext2_features[] = {[KFS_FEATURE_SIZE] = 1, [KFS_FEATURE_FILETYPE] = 1, [KFS_FEATURE_FREESPACE] = 1, [KFS_FEATURE_FILE_LFS] = 1, [KFS_FEATURE_BLOCKSIZE] = 1, [KFS_FEATURE_DEVSIZE] = 1, [KFS_FEATURE_MTIME] = 1, [KFS_FEATURE_ATIME] = 1, [KFS_FEATURE_GID] = 1, [KFS_FEATURE_UID] = 1, [KFS_FEATURE_UNIX_PERM] = 1, [KFS_FEATURE_NLINKS] = 1, [KFS_FEATURE_SYMLINK] = 1, [KFS_FEATURE_DELETE] = 1};

static size_t ext2_get_max_feature_id(LFS_t * object)
{
	return sizeof(ext2_features) / sizeof(ext2_features[0]) - 1;
}

static const bool * ext2_get_feature_array(LFS_t * object)
{
	return ext2_features;
}

static int ext2_get_metadata(LFS_t * object, const ext2_fdesc_t * f, uint32_t id, size_t size, void * data)
{
	Dprintf("EXT2DEBUG: ext2_get_metadata\n");
	struct ext2_info * info = (struct ext2_info *) object;
	if (id == KFS_FEATURE_SIZE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_size;
	}
	else if (id == KFS_FEATURE_FILETYPE) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_type;
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

		*((uint32_t *) data) = object->blocksize;
	}
	else if (id == KFS_FEATURE_DEVSIZE) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = info->super->s_blocks_count;
	}
	else if (id == KFS_FEATURE_NLINKS) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = (uint32_t) f->f_inode.i_links_count;
	}
	else if (id == KFS_FEATURE_UID) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_uid;
	}
	else if (id == KFS_FEATURE_GID) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_gid;
	}
	else if (id == KFS_FEATURE_UNIX_PERM) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);

		*((uint16_t *) data) = f->f_inode.i_mode & ~EXT2_S_IFMT;
	}
	else if (id == KFS_FEATURE_MTIME) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_mtime;
	}
	else if (id == KFS_FEATURE_ATIME) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_atime;
	}
	else if (id == KFS_FEATURE_SYMLINK) {
		struct ext2_info * info = (struct ext2_info *) object;
		if (!f || f->f_type != TYPE_SYMLINK)
			return -EINVAL;

		//f->f_inode.i_size includes the zero byte!
		if (size < f->f_inode.i_size)
			return -ENOMEM;
		size = f->f_inode.i_size;

		//size of the block pointer array in bytes:
		if (size < EXT2_N_BLOCKS * sizeof(uint32_t))
			memcpy(data, (char *) f->f_inode.i_block, size);
		else {
			bdesc_t * symlink_block;
			symlink_block = CALL(info->ubd, read_block, f->f_inode.i_block[0], 1);
			if (!symlink_block)
				return -1;
			memcpy(data, symlink_block->ddesc->data, f->f_inode.i_size);
		}	
	}
	else
		return -EINVAL;

	return size;
}

static int ext2_get_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("EXT2DEBUG: ext2_get_metadata_inode %u\n", ino);
	int r;
	const ext2_fdesc_t * f = (ext2_fdesc_t *) ext2_lookup_inode(object, ino);
	r = ext2_get_metadata(object, f, id, size, data);
	if (f)
		ext2_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ext2_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	const ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	return ext2_get_metadata(object, f, id, size, data);
}

static int ext2_write_slow_symlink(LFS_t * object, ext2_fdesc_t * f, char * name, uint32_t name_len, chdesc_t ** head)
{
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t new_block_no;
	bdesc_t * new_block;
	int r;
	DEFINE_CHDESC_PASS_SET(set, 2, NULL);
	set.array[0] = *head;
	set.array[1] = NULL;
	
	if (name_len > object->blocksize)
		return -ENAMETOOLONG;

	new_block_no = ext2_allocate_block(object, (fdesc_t *) f, PURPOSE_FILEDATA, &set.array[1]);
	if (new_block_no == INVALID_BLOCK)
		 return -EINVAL;

	//TODO dont assume this is written after this function returns! (BAD!!)
	f->f_inode.i_block[0] = new_block_no;
	new_block = CALL(info->ubd, synthetic_read_block, new_block_no, 1);
	if (!new_block)
		return -1;

	r = chdesc_create_byte_set(new_block, info->ubd, 0, name_len, (void *) name, head, PASS_CHDESC_SET(set));
	if (r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add slow symlink");

	return CALL(info->ubd, write_block, new_block);
}

static int ext2_set_metadata(LFS_t * object, ext2_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_set_metadata %u, %u\n", id, size);
	struct ext2_info * info = (struct ext2_info *) object;
	
	if (!head || !f || !data)
		return -EINVAL;

	if (id == KFS_FEATURE_SIZE) {
		if (sizeof(uint32_t) != size || *((uint32_t *) data) < 0 || *((uint32_t *) data) >= EXT2_MAX_FILE_SIZE)
			return -EINVAL;
		f->f_inode.i_size = *((uint32_t *) data);
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_FILETYPE) {
		uint32_t fs_type;
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		switch(*((uint32_t *) data))
		{
			case TYPE_FILE:
				fs_type = EXT2_S_IFREG;
				break;
			case TYPE_DIR:
				fs_type = EXT2_S_IFDIR;
				break;
			default:
				return -EINVAL;
		}

		f->f_inode.i_mode = (f->f_inode.i_mode & ~EXT2_S_IFMT) | (fs_type);
		f->f_type = *((uint32_t *) data);
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_UID) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_uid = *(uint32_t *) data;
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_GID) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_gid = *(uint32_t *) data;
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_UNIX_PERM) {
		if (sizeof(uint16_t) != size)
			return -EINVAL;
		f->f_inode.i_mode = (f->f_inode.i_mode & EXT2_S_IFMT)
			| (*((uint16_t *) data) & ~EXT2_S_IFMT);
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_MTIME) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_mtime = *((uint32_t *) data);
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_ATIME) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_atime = *((uint32_t *) data);
		return ext2_write_inode(info, f, head);
	}
	else if (id == KFS_FEATURE_SYMLINK) {
		int r;
		if (!f || f->f_type != TYPE_SYMLINK)
			return -EINVAL;
		
		if (size < EXT2_N_BLOCKS * sizeof(uint32_t))
			memcpy((char *) f->f_inode.i_block, data, size);
		else {
			//allocate a block, link it into the inode, write the file, write the inodeo
			r = ext2_write_slow_symlink(object, f, (char *) data, size, head);
			if (r < 0)
				return r;
		}
		f->f_inode.i_size = size; //size must include zerobyte!
		return ext2_write_inode(info, f, head);
	}
	else
		return -EINVAL;
}

static int ext2_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	int r;
	ext2_fdesc_t * f = (ext2_fdesc_t *) ext2_lookup_inode(object, ino);
	if (!f)
		return -EINVAL;
	r = ext2_set_metadata(object, f, id, size, data, head);
	ext2_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ext2_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	return ext2_set_metadata(object, f, id, size, data, head);
}

static int ext2_destroy(LFS_t * lfs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	int i,r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);	
	hash_map_destroy(info->filemap);
	if(info->bitmap_cache != NULL)
		bdesc_release(&info->bitmap_cache);
	if(info->inode_cache != NULL)
		bdesc_release(&info->inode_cache);
	if(info->super_cache != NULL)
		bdesc_release(&info->super_cache);
	for(i = 0; i < info->ngroupblocks; i++)
		bdesc_release(&(info->gdescs[i]));
	
	ext2_mdir_cache_deinit(&info->mdir_cache);
	n_ext2_instances--;
	if(!n_ext2_instances)
	{
		ext2_mdirent_free_all();
		ext2_fdesc_free_all();
	}
	free(info->gdescs);
	free((EXT2_Super_t *) info->super);
	free((EXT2_group_desc_t *) info->groups);
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

/*
 * Reads group descriptor of inode number ino and sets inode to that inode
 */

static int ext2_get_inode(ext2_info_t * info, ext2_fdesc_t *f, int copy)
{
	uint32_t block_group, offset, block;
	assert(f);
	assert(f->f_ino == EXT2_ROOT_INO || (f->f_ino >= info->super->s_first_ino && f->f_ino <= info->super->s_inodes_count));
	assert(!f->f_inode_cache);
	
	//Get the group the inode belongs in
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	offset = ((f->f_ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (offset >> (10 + info->super->s_log_block_size));

	f->f_inode_cache = CALL(info->ubd, read_block, block, 1);
	if (!f->f_inode_cache)
		return -EINVAL;
	bdesc_retain(f->f_inode_cache);

	if (copy) {
		offset &= info->_blocksize_ - 1;
		memcpy(&f->f_inode, f->f_inode_cache->ddesc->data + offset, sizeof(EXT2_inode_t));
	}

	return f->f_ino;
}

//TODO Make this pretty and better
static uint8_t ext2_to_kfs_type(uint16_t type)
{
	switch(type & EXT2_S_IFMT) {
		case(EXT2_S_IFDIR):
			return TYPE_DIR;
		case(EXT2_S_IFREG):
			return TYPE_FILE;
		case(EXT2_S_IFLNK):
			return TYPE_SYMLINK;	
		default:
			return TYPE_INVAL;
	}
}

int ext2_write_inode_set(struct ext2_info * info, ext2_fdesc_t *f, chdesc_t ** tail, chdesc_pass_set_t * befores)
{
	uint32_t block_group, offset, block;
	int r;
	
	assert(tail);
	assert(f);
	assert(f->f_ino == EXT2_ROOT_INO || (f->f_ino >= info->super->s_first_ino && f->f_ino <= info->super->s_inodes_count));

	if (!f->f_inode_cache)
		if (ext2_get_inode(info, f, 0) < 0)
			return -1;

	//Get the group the inode belongs in
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	
	offset = ((f->f_ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (offset >> (10 + info->super->s_log_block_size));
	
	offset &= info->_blocksize_ - 1;
	r = chdesc_create_diff_set(f->f_inode_cache, info->ubd, offset, sizeof(EXT2_inode_t), &f->f_inode_cache->ddesc->data[offset], &f->f_inode, tail, befores);
	if (r < 0)
		return r;
	//chdesc_create_diff() returns 0 for "no change"
	if (*tail && r > 0)
	{
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *tail, "write inode");
		lfs_add_fork_head(*tail); // TODO: why do this?
		r = CALL(info->ubd, write_block, f->f_inode_cache);
	}

	return r;
}

int ext2_write_inode(struct ext2_info * info, ext2_fdesc_t *f, chdesc_t ** head)
{
	DEFINE_CHDESC_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return ext2_write_inode_set(info, f, head, PASS_CHDESC_SET(set));
}

static int ext2_super_report(LFS_t * lfs, uint32_t group, int32_t blocks, int32_t inodes, int32_t dirs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	int r = 0;
	chdesc_t * head = info->write_head ? *info->write_head : NULL;

	//Deal with the super block
	if (blocks || inodes) {
		EXT2_Super_t *super = (EXT2_Super_t *) info->super;
		super->s_free_blocks_count += blocks;
		super->s_free_inodes_count += inodes;

		int off1 = (blocks ? offsetof(EXT2_Super_t, s_free_blocks_count) : offsetof(EXT2_Super_t, s_free_inodes_count));
		int off2 = (inodes ? offsetof(EXT2_Super_t, s_free_inodes_count) + sizeof(super->s_free_inodes_count) : offsetof(EXT2_Super_t, s_free_blocks_count) + sizeof(super->s_free_blocks_count));

		r = chdesc_create_byte(info->super_cache, info->ubd,
				       off1, off2 - off1,
				       ((const uint8_t *) super) + off1, &head);
		if (r >= 0 && head) {
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "write superblock");
			lfs_add_fork_head(head);
			r = CALL(info->ubd, write_block, info->super_cache);
		}
	}

	if (r >= 0 && (blocks || inodes || dirs)) {
		//Deal with the group descriptors
		EXT2_group_desc_t *gd = (EXT2_group_desc_t *) &info->groups[group];
		gd->bg_free_blocks_count += blocks;
		gd->bg_free_inodes_count += inodes;
		gd->bg_used_dirs_count += dirs;
	
		head = info->write_head ? *info->write_head : NULL;
	
		int group_bdesc = group / info->block_descs;
		int group_offset = group % info->block_descs;
		group_offset *= sizeof(EXT2_group_desc_t);

		int off1 = offsetof(EXT2_group_desc_t, bg_free_blocks_count);
		int off2 = offsetof(EXT2_group_desc_t, bg_used_dirs_count) + sizeof(gd->bg_used_dirs_count);
		
		r = chdesc_create_byte(info->gdescs[group_bdesc], info->ubd,
				       group_offset + off1, off2 - off1,
				       ((const uint8_t *) gd) + off1, &head);
		if (r >= 0 && head) {
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, head, "write group desc");
			lfs_add_fork_head(head);
			r = CALL(info->ubd, write_block, info->gdescs[group_bdesc]);
		}
	}
	
	return r;
}

static int ext2_load_super(LFS_t * lfs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	
	//initialize the pointers
	info->bitmap_cache = NULL;
	info->inode_cache = NULL;
	info->groups = NULL;
	info->gnum = INVALID_BLOCK;	
	info->inode_gdesc = INVALID_BLOCK;	
#if ROUND_ROBIN_ALLOC
	info->last_fblock = 0;
	info->last_iblock = 0;
	info->last_dblock = 0;
#endif
	
	info->super_cache = CALL(info->ubd, read_block, 0, 1);
	if (info->super_cache == NULL)
	{
		printf("Unable to read superblock!\n");
		return 0;
	}
	bdesc_retain(info->super_cache);
	info->super = malloc(sizeof(struct EXT2_Super));
	memcpy((EXT2_Super_t *) info->super, info->super_cache->ddesc->data + 1024, sizeof(EXT2_Super_t));

#if ROUND_ROBIN_ALLOC
	/* start file data at the beginning, indirect blocks halfway through,
	 * and directory data one quarter from the end of the file system */
	info->last_fblock = 0;
	info->last_iblock = info->super->s_blocks_count / 2;
	info->last_dblock = 3 * (info->super->s_blocks_count / 4);
#endif
	
	//now load the gdescs
	uint32_t block, i;
	uint32_t ngroupblocks;
	lfs->blocksize = 1024 << info->super->s_log_block_size;
	info->_blocksize_ = lfs->blocksize;
	info->block_descs = lfs->blocksize / sizeof(EXT2_group_desc_t);
	int ngroups = (info->super->s_blocks_count / info->super->s_blocks_per_group);
	if (info->super->s_blocks_count % info->super->s_blocks_per_group != 0)
		ngroups++;
	info->ngroups = ngroups;
	info->groups = calloc(ngroups, sizeof(EXT2_group_desc_t));
	if (!info->groups)
		goto wb_fail1;
	block = 1; //Block 1 is where the gdescs are stored
	
	ngroupblocks = ngroups / info->block_descs;
	if (ngroups % info->block_descs != 0)
		ngroupblocks++;
	
	info->gdescs = malloc(ngroupblocks*sizeof(bdesc_t*));
	int nbytes = 0;
	for(i = 0; i < ngroupblocks; i++) {
		info->gdescs[i] = CALL(info->ubd, read_block, (block + i), 1);
		if(!info->gdescs[i])
			goto wb_fail2;
		
		if ( (sizeof(EXT2_group_desc_t) * ngroups) < (lfs->blocksize*(i+1)) )
			nbytes = (sizeof(EXT2_group_desc_t) * ngroups) % lfs->blocksize;
		else
			nbytes = lfs->blocksize;
		
		if (!memcpy((EXT2_group_desc_t *) info->groups + (i * info->block_descs),
		            info->gdescs[i]->ddesc->data, nbytes))
			goto wb_fail2;
		bdesc_retain(info->gdescs[i]);
	}
	info->ngroupblocks = ngroupblocks;	
	return 1;	

wb_fail2:
	for(i = 0; i < ngroupblocks; i++)
		bdesc_release(&(info->gdescs[i]));
	free(info->gdescs);
	free((EXT2_Super_t *) info->super);
	free((EXT2_group_desc_t *) info->groups);
 wb_fail1:
	bdesc_release(&info->super_cache);
	return 0;
	
}

LFS_t * ext2(BD_t * block_device)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	if (!block_device)
		return NULL;
	
	struct ext2_info * info;
	LFS_t * lfs;

	info = malloc(sizeof(*info));
	if (!info)
		return NULL;

	lfs = &info->lfs;
	LFS_INIT(lfs, ext2);
	OBJMAGIC(lfs) = EXT2_FS_MAGIC;

	info->ubd = lfs->blockdev = block_device;
	info->write_head = CALL(block_device, get_write_head);

	info->filemap = hash_map_create();
	if (!info->filemap) {
		free(info);
		return NULL;
	}

	ext2_mdir_cache_init(&info->mdir_cache);

	if (ext2_load_super(lfs) == 0) {
		ext2_mdir_cache_deinit(&info->mdir_cache);
		free(info);
		return NULL;
	}

	if (check_super(lfs)) {
		ext2_mdir_cache_deinit(&info->mdir_cache);
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
	
	n_ext2_instances++;
	
	return lfs;
}
