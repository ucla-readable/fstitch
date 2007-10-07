/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/pool.h>

#include <fscore/bd.h>
#include <fscore/lfs.h>
#include <fscore/modman.h>
#include <fscore/debug.h>
#include <fscore/feature.h>

#include <modules/ext2.h>
#include <modules/ext2_lfs.h>

#define EXT2_LFS_DEBUG 0

#ifndef NDEBUG
# define DELETE_MERGE_STATS 1
#else
# define DELETE_MERGE_STATS 0
#endif

#define ROUND_ROBIN_ALLOC 1

#if EXT2_LFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* values for the "purpose" parameter */
#define PURPOSE_FILEDATA 0
#define PURPOSE_DIRDATA 1
#define PURPOSE_INDIRECT 2
#define PURPOSE_DINDIRECT 3

/* well-known block numbers */
#define SUPER_BLOCKNO		0
#define GDESC_BLOCKNO(i)	(1 + (i))

/* typedefs */
typedef struct ext2_minode ext2_minode_t;
typedef struct ext2_minode_cache ext2_minode_cache_t;
typedef struct mdirent_dlist mdirent_dlist_t;
typedef struct ext2_mdirent ext2_mdirent_t;
typedef struct ext2_mdir ext2_mdir_t;
typedef struct ext2_mdir_cache ext2_mdir_cache_t;
typedef struct ext2_info ext2_info_t;
typedef struct ext2_fdesc ext2_fdesc_t;

/* directory entry cache */
struct ext2_minode {
	inode_t ino;
	patchweakref_t create;
	unsigned ref_count;
};

struct ext2_minode_cache {
	hash_map_t * minodes_map;
};

struct mdirent_dlist {
	struct ext2_mdirent ** pprev, * next;
};

/* in-memory dirent */
struct ext2_mdirent {
	EXT2_Dir_entry_t dirent;
	char name_term; /* ensure room for dirent.name null termination */
	uint32_t offset;
	patchweakref_t create; /* patch that created this dirent */
	ext2_minode_t * minode; /* the patch that created this dirent's inode */
	mdirent_dlist_t offsetl;
	mdirent_dlist_t freel;
};

/* in-memory directory */
struct ext2_mdir {
	inode_t ino; /* inode of this directory */
	hash_map_t * mdirents; /* file name -> ext2_mdirent */
	ext2_minode_cache_t * minode_cache;
	ext2_mdirent_t * offset_first, * offset_last;
	ext2_mdirent_t * free_first, * free_last;
	struct ext2_mdir ** lru_polder, * lru_newer;
};

/* Perhaps this is a good number? */
#define MAXCACHEDDIRS 1024

struct ext2_mdir_cache {
	hash_map_t * mdirs_map;
	ext2_mdir_t mdirs_table[MAXCACHEDDIRS];
	ext2_mdir_t * lru_oldest, * lru_newest;
};

/* ext2 LFS structure */
struct ext2_info {
	LFS_t lfs;
	
	BD_t * ubd;
	patch_t ** write_head;
	const EXT2_Super_t * super; /* const to limit who can change it */
	const EXT2_group_desc_t * groups; /* const to limit who can change it */
	ext2_fdesc_t * filecache;
	ext2_mdir_cache_t mdir_cache;
	ext2_minode_cache_t minode_cache;
	bdesc_t ** gdescs;
	bdesc_t * super_cache;
	bdesc_t * bitmap_cache;
	uint32_t bitmap_cache_number;
	bdesc_t * inode_cache;
	uint32_t inode_cache_number;
	uint32_t ngroups, gnum;
	uint32_t ngroupblocks;
	uint32_t inode_gdesc;
	uint16_t block_descs;
#if ROUND_ROBIN_ALLOC
	/* the last block number allocated for each of file
	 * data, directory data, and [d]indirect pointers */
	uint32_t last_fblock, last_dblock, last_iblock;
#endif
#if DELETE_MERGE_STATS
	struct {
		unsigned merged, uncommitted, total;
	} delete_dirent_stats, delete_inode_stats;
#endif
};

struct ext2_fdesc {
	/* extend struct fdesc */
	struct fdesc_common * common;
	struct fdesc_common base;
	
	ext2_fdesc_t ** f_cache_pprev;
	ext2_fdesc_t * f_cache_next;
	
	bdesc_t * f_inode_cache;
	const EXT2_inode_t * f_ip;
	EXT2_inode_t f_xinode;
	uint8_t f_type;
	inode_t	f_ino;
	uint32_t f_nopen;
#if !ROUND_ROBIN_ALLOC
	uint32_t f_lastblock;
#endif
	uint32_t f_age;
};

#define DECL_INODE_MOD(f) \
	int ioff1 = sizeof(EXT2_inode_t), ioff2 = 0;                     \
	if((f)->f_ip != &(f)->f_xinode)                                  \
	{                                                                \
		memcpy(&(f)->f_xinode, (f)->f_ip, sizeof(EXT2_inode_t)); \
		(f)->f_ip = &(f)->f_xinode;                              \
	}

#define INODE_CLEAR(f) \
	do {                                                             \
		(f)->f_ip = &(f)->f_xinode;                              \
		memset(&(f)->f_xinode, 0, sizeof(EXT2_inode_t));         \
	} while(0)

#define INODE_SET(f, field, value) \
	do {                                                             \
		assert((f)->f_ip == &(f)->f_xinode);                     \
		if((f)->f_ip->field != (value))                          \
		{                                                        \
			if(ioff1 > offsetof(EXT2_inode_t, field))        \
				ioff1 = offsetof(EXT2_inode_t, field);   \
			if(ioff2 < offsetof(EXT2_inode_t, field) + sizeof((f)->f_xinode.field)) \
				ioff2 = offsetof(EXT2_inode_t, field) + sizeof((f)->f_xinode.field); \
			(f)->f_xinode.field = (value);                   \
		}                                                        \
	} while(0)

#define INODE_ADD(f, field, delta) \
	do {                                                             \
		INODE_SET(f, field, (f)->f_ip->field + (delta));         \
	} while(0)


/* some prototypes */
static int ext2_read_block_bitmap(LFS_t * object, uint32_t blockno);
static int _ext2_free_block(LFS_t * object, uint32_t block, patch_t ** head);
static uint32_t get_file_block(LFS_t * object, ext2_fdesc_t * file, uint32_t offset);

static int ext2_super_report(LFS_t * lfs, uint32_t group, int32_t blocks, int32_t inodes, int32_t dirs);
static int ext2_get_inode(ext2_info_t * info, ext2_fdesc_t * f, int copy);
static inline uint8_t ext2_to_fstitch_type(uint16_t type);
static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t * dir, ext2_mdirent_t * mdirent, patch_t ** head);

static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent);
static int ext2_write_inode(struct ext2_info * info, ext2_fdesc_t * f, patch_t ** head, int ioff1, int ioff2);
static int ext2_write_inode_set(struct ext2_info * info, ext2_fdesc_t * f, patch_t ** tail, patch_pass_set_t * befores, int ioff1, int ioff2);

DECLARE_POOL(ext2_minode, ext2_minode_t);
DECLARE_POOL(ext2_mdirent, ext2_mdirent_t);
DECLARE_POOL(ext2_fdesc_pool, ext2_fdesc_t);
static int n_ext2_instances;

// TODO Make this pretty and better
static inline uint8_t ext2_to_fstitch_type(uint16_t type)
{
	switch(type & EXT2_S_IFMT)
	{
		case EXT2_S_IFDIR:
			return TYPE_DIR;
		case EXT2_S_IFREG:
			return TYPE_FILE;
		case EXT2_S_IFLNK:
			return TYPE_SYMLINK;	
		default:
			return TYPE_INVAL;
	}
}

static inline int ext2_write_inode(struct ext2_info * info, ext2_fdesc_t * f, patch_t ** head, int ioff1, int ioff2)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return ext2_write_inode_set(info, f, head, PASS_PATCH_SET(set), ioff1, ioff2);
}

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


static ext2_minode_t * ext2_minode_create(ext2_minode_cache_t * cache, inode_t ino)
{
	ext2_minode_t * minode;
	int r;
	
	minode = ext2_minode_alloc();
	if(!minode)
		return NULL;
	
	r = hash_map_insert(cache->minodes_map, (void *) ino, minode);
	if(r < 0)
	{
		ext2_minode_free(minode);
		return NULL;
	}
	assert(!r);
	
	minode->ino = ino;
	WEAK_INIT(minode->create);
	minode->ref_count = 0;
	
	return minode;
}

static void ext2_minode_destroy(ext2_minode_cache_t * cache, ext2_minode_t * minode)
{
	ext2_minode_t * mi = hash_map_erase(cache->minodes_map, (void *) minode->ino);
	assert(mi == minode); (void) mi;
	assert(!minode->ref_count);
	if(WEAK(minode->create))
		patch_weak_release(&minode->create, 0);
	ext2_minode_free(minode);
}

// Increase the reference count for an minode
static void ext2_minode_retain(ext2_minode_t * minode)
{
	minode->ref_count++;
	assert(minode->ref_count);
}

// Decrement the reference count for an minode. Free it if no longer in use.
static void ext2_minode_release(ext2_minode_cache_t * cache, ext2_minode_t * minode)
{
	assert(minode->ref_count);
	minode->ref_count--;
	if(!minode->ref_count)
		ext2_minode_destroy(cache, minode);
}

static ext2_minode_t * ext2_minode_get(ext2_minode_cache_t * cache, inode_t ino)
{
	return hash_map_find_val(cache->minodes_map, (void *) ino);
}

static void ext2_minode_cache_deinit(ext2_minode_cache_t * cache)
{
	assert(hash_map_empty(cache->minodes_map));
	hash_map_destroy(cache->minodes_map);
}

static int ext2_minode_cache_init(ext2_minode_cache_t * cache)
{
	cache->minodes_map = hash_map_create();
	if(!cache->minodes_map)
		return -ENOMEM;
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
		if(WEAK(mdirent->create))
			patch_weak_release(&mdirent->create, 0);
		if(mdirent->minode)
			ext2_minode_release(mdir->minode_cache, mdirent->minode);
		ext2_mdirent_free(mdirent);
		mdirent = next;
	}
	mdir->offset_first = mdir->offset_last = NULL;
	mdir->free_first = mdir->free_last = NULL;
}

// Add a new mdirent to mdir
static int ext2_mdirent_add(ext2_mdir_t * mdir, const EXT2_Dir_entry_t * entry, uint32_t offset, ext2_mdirent_t ** pmdirent)
{
	ext2_mdirent_t * mdirent = ext2_mdirent_alloc();
	int r;
	if(!mdirent)
		return -ENOMEM;
	
	memcpy(&mdirent->dirent, entry, MIN(entry->rec_len, sizeof(*entry)));
	mdirent->dirent.name[mdirent->dirent.name_len] = 0;
	mdirent->offset = offset;
	WEAK_INIT(mdirent->create);
	mdirent->minode = NULL;
	
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
	
	if(pmdirent)
		*pmdirent = mdirent;
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
	assert(!WEAK(mdirent->create));
	assert(!mdirent->minode);
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
	assert(mde == mdirent); (void) mde;
	
	if(!(mdirent->offset % blocksize))
	{
		// convert to a jump (empty) dirent
		mdirent->dirent.inode = 0;
		if(WEAK(mdirent->create))
			patch_weak_release(&mdirent->create, 0);
		if(mdirent->minode)
		{
			ext2_minode_release(mdir->minode_cache, mdirent->minode);
			mdirent->minode = NULL;
		}
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
		
		if(WEAK(mdirent->create))
			patch_weak_release(&mdirent->create, 0);
		if(mdirent->minode)
			ext2_minode_release(mdir->minode_cache, mdirent->minode);
		ext2_mdirent_free(mdirent);
	}
}

// Split a new dirent out of mdirent's unused space
static int ext2_mdirent_split(ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, const EXT2_Dir_entry_t * existing_dirent, const EXT2_Dir_entry_t * new_dirent, ext2_mdirent_t ** pnmdirent)
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
	WEAK_INIT(nmdirent->create);
	nmdirent->minode = NULL;
	
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
	
	if(pnmdirent)
		*pnmdirent = nmdirent;
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
	mdir->lru_polder = &info->mdir_cache.lru_oldest;
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
	for(; cur_base < dir_file->f_ip->i_size; cur_base = next_base)
	{
		const EXT2_Dir_entry_t * entry;
		// TODO: pass disk block to ext2_get_disk_dirent()?
		// or have it use an internal single item cache?
		r = ext2_get_disk_dirent(object, dir_file, &next_base, &entry);
		if(r < 0)
			goto fail;
		r = ext2_mdirent_add(mdir, entry, cur_base, NULL);
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

static int ext2_mdir_cache_init(ext2_mdir_cache_t * cache, ext2_minode_cache_t * minode_cache)
{
	size_t i;
	
	cache->mdirs_map = hash_map_create_size(MAXCACHEDDIRS, 0);
	if(!cache->mdirs_map)
		return -ENOMEM;
	
	for(i = 0; i < MAXCACHEDDIRS; i++)
	{
		cache->mdirs_table[i].ino = INODE_NONE;
		cache->mdirs_table[i].mdirents = hash_map_create_str();
		cache->mdirs_table[i].minode_cache = minode_cache;
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
			bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1, NULL);
			if(!bitmap)
				return -ENOENT;
			bdesc_retain(bitmap);
			bitmap->flags |= BDESC_FLAG_BITMAP;
			info->bitmap_cache = bitmap;
			info->bitmap_cache_number = info->groups[block_group].bg_block_bitmap;
		}
		
		array = (const unsigned long *) bdesc_data(info->bitmap_cache);
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
		info->bitmap_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1, NULL);
		if(!info->bitmap_cache)
			return -ENOENT;
		bdesc_retain(info->bitmap_cache);
		info->bitmap_cache->flags |= BDESC_FLAG_BITMAP;
		info->bitmap_cache_number = info->groups[block_group].bg_block_bitmap;
	}
	
	block_in_group = blockno % info->super->s_blocks_per_group;
	bitmap = ((uint32_t *) bdesc_data(info->bitmap_cache)) + (block_in_group / 32);
	if(*bitmap & (1 << (block_in_group % 32)))
		return EXT2_USED;
	return EXT2_FREE;
}

static int ext2_write_block_bitmap(LFS_t * object, uint32_t blockno, bool value, patch_t ** head)
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
		info->bitmap_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1, NULL);
		if(!info->bitmap_cache)
			return -ENOENT;
		bdesc_retain(info->bitmap_cache);
		info->bitmap_cache->flags |= BDESC_FLAG_BITMAP;
		info->bitmap_cache_number = info->groups[block_group].bg_block_bitmap;
	}
	
	block_in_group = blockno % info->super->s_blocks_per_group;	
	/* does it already have the right value? */
	if(((uint32_t *) bdesc_data(info->bitmap_cache))[block_in_group / 32] & (1 << (block_in_group % 32)))
	{
		if(value)
			return 0;
	}
	else if(!value)
		return 0;
	
	/* bit patches take offset in increments of 32 bits */
	r = patch_create_bit(info->bitmap_cache, info->ubd, block_in_group / 32, 1 << (block_in_group % 32), head);
	if(r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, value ? "allocate block" : "free block");
	
	r = CALL(info->ubd, write_block, info->bitmap_cache, info->bitmap_cache_number);
	if(r < 0)
		return r;
	
	return ext2_super_report(object, block_group, (value ? -1 : 1), 0, 0);
}

static int ext2_write_inode_bitmap(LFS_t * object, inode_t inode_no, bool value, patch_t ** head)
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
		info->inode_cache = CALL(info->ubd, read_block, info->groups[block_group].bg_inode_bitmap, 1, NULL);
		if(!info->inode_cache)
			return -ENOENT;
		bdesc_retain(info->inode_cache);
		info->inode_cache->flags |= BDESC_FLAG_BITMAP;
		info->inode_cache_number = info->groups[block_group].bg_inode_bitmap;
	}
	
	inode_in_group = (inode_no - 1) % info->super->s_inodes_per_group;
	/* does it already have the right value? */
	if(((uint32_t *) bdesc_data(info->inode_cache))[inode_in_group / 32] & (1 << (inode_in_group % 32)))
	{
		if(value)
			return 0;
	}
	else if(!value)
		return 0;
	
	/* bit patches take offset in increments of 32 bits */
	r = patch_create_bit(info->inode_cache, info->ubd, inode_in_group / 32, 1 << (inode_in_group % 32), head);
	if(r < 0)
		return r;	
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, value ? "allocate inode" : "free inode");
	
	r = CALL(info->ubd, write_block, info->inode_cache, info->inode_cache_number);
	if(r < 0)
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

static uint32_t ext2_allocate_block(LFS_t * object, fdesc_t * file, int purpose, patch_t ** tail)
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
	if(!f->f_ip->i_size || purpose)
		goto inode_search;
	
	// Get the block number of the last block of the inode
	if(f->f_lastblock != 0)
		blockno = f->f_lastblock;
	else
		blockno = get_file_block(object, (ext2_fdesc_t *) f, f->f_ip->i_size - 1);	
	if(blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	lastblock = blockno;
	// FIXME this could affect performance
	// Look in the 32 block vicinity of the lastblock
	// There is no check to make sure that these blocks are all in the same block group
	while(blockno - lastblock < 32)
	{
		int r = ext2_read_block_bitmap(object, ++blockno);
		if(r == EXT2_FREE)
			goto claim_block;
		else if(r < 0)
			return INVALID_BLOCK;
	}
	
inode_search:	
	// Look for free blocks in same block group as the inode
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
	// FIXME this should be slightly smarter
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

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("EXT2DEBUG: ext2_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) object;
	return CALL(info->ubd, read_block, number, 1, page);
}

static bdesc_t * ext2_synthetic_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("EXT2DEBUG: ext2_synthetic_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) object;
	return CALL(info->ubd, synthetic_read_block, number, 1, page);
}

static void __ext2_free_fdesc(ext2_fdesc_t * f)
{
	assert(f && !f->f_nopen);
	if(f->f_inode_cache)
		bdesc_release(&f->f_inode_cache);
	if((*f->f_cache_pprev = f->f_cache_next))
		f->f_cache_next->f_cache_pprev = f->f_cache_pprev;
	ext2_fdesc_pool_free(f);
}

static inline void ext2_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) fdesc;
	if(f && !--f->f_nopen)
		__ext2_free_fdesc(f);
}

static fdesc_t * ext2_lookup_inode(LFS_t * object, inode_t ino)
{
	ext2_fdesc_t * fd = NULL, * oldest_fd = NULL;
	struct ext2_info * info = (struct ext2_info *) object;
	static uint32_t age;
	int r, nincache = 0;
	
	if(ino <= 0)
		return NULL;
	
	if(!++age)
		++age;
	
	for(fd = info->filecache; fd; fd = fd->f_cache_next)
		if(fd->f_ino == ino)
		{
			fd->f_nopen += (fd->f_age ? 1 : 2);
			fd->f_age = age;
			return (fdesc_t *) fd;
		}
		else if(fd->f_age)
		{
			++nincache;
			if(!oldest_fd || (int32_t) (oldest_fd->f_age - fd->f_age) > 0)
				oldest_fd = fd;
		}
	
	fd = ext2_fdesc_pool_alloc();
	if(!fd)
		goto ext2_lookup_inode_exit;
	
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_inode_cache = NULL;
	fd->f_ino = ino;
	fd->f_nopen = 2;
#if !ROUND_ROBIN_ALLOC
	fd->f_lastblock = 0;
#endif
	fd->f_age = age;
	
	r = ext2_get_inode(info, fd, 1);
	if(r < 0)
		goto ext2_lookup_inode_exit;
	fd->f_type = ext2_to_fstitch_type(fd->f_ip->i_mode);
	
	// stick in cache
	if(oldest_fd && nincache >= 4)
	{
		oldest_fd->f_age = 0;
		ext2_free_fdesc(object, (fdesc_t *) oldest_fd);
	}
	fd->f_cache_pprev = &info->filecache;
	fd->f_cache_next = info->filecache;
	info->filecache = fd;
	if(fd->f_cache_next)
		fd->f_cache_next->f_cache_pprev = &fd->f_cache_next;
	
	return (fdesc_t *) fd;
	
ext2_lookup_inode_exit:
	ext2_fdesc_pool_free(fd);
	return NULL;
}

static int ext2_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("EXT2DEBUG: ext2_lookup_name %s\n", name);
	ext2_fdesc_t * fd;
	ext2_fdesc_t * parent_file;
	ext2_mdir_t * mdir;
	ext2_mdirent_t * mdirent;
	int r = 0;
	
	// TODO do some sanity checks on name
	
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
	
	return (f->f_ip->i_size + object->blocksize - 1) / object->blocksize;
}

static uint32_t get_file_block(LFS_t * object, ext2_fdesc_t * file, uint32_t offset)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno, n_per_block;
	bdesc_t * block_desc;
	uint32_t * inode_nums, blocknum;
	
	if(offset >= file->f_ip->i_size || file->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;
	
	n_per_block = object->blocksize / (sizeof(uint32_t));
	
	// non block aligned offsets suck (aka aren't supported)
	blocknum = offset / object->blocksize;
	
	// TODO: compress this code, but right now its much easier to understand...
	if(blocknum >= n_per_block * n_per_block + n_per_block + EXT2_NDIRECT)
	{
		// Lets not worry about tripley indirect for the momment
		return INVALID_BLOCK;
	}
	else if(blocknum >= n_per_block + EXT2_NDIRECT)
	{
		blocknum -= (EXT2_NDIRECT + n_per_block);
		block_desc = CALL(info->ubd, read_block, file->f_ip->i_block[EXT2_DINDIRECT], 1, NULL);
		if(!block_desc)
		{
			Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *) bdesc_data(block_desc);
		blockno = inode_nums[blocknum / n_per_block];
		block_desc = CALL(info->ubd, read_block, blockno, 1, NULL);
		if(!block_desc)
		{
			Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *) bdesc_data(block_desc);
		blocknum %= n_per_block;
		return inode_nums[blocknum];
	}	
	else if(blocknum >= EXT2_NDIRECT)
	{
		blocknum -= EXT2_NDIRECT;
		block_desc = CALL(info->ubd, read_block, file->f_ip->i_block[EXT2_INDIRECT], 1, NULL);
		if(!block_desc)
		{
			Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
			return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *) bdesc_data(block_desc);
		return inode_nums[blocknum];
	}
	else
		return file->f_ip->i_block[blocknum];
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
	
	if(size < reclen || !basep)
		return -EINVAL;
	
	if(!dirfile->rec_len)
		return -1;
	
	// If the name length is 0 (or less?) then we assume it's an empty slot
	if(namelen < 1)
		return -1;
	
	entry->d_type = ext2_to_fstitch_type(dirfile->file_type);
	
	//EXT2_inode_t inode;
	//if(ext2_get_inode(info, ino, &inode) < 0)
	//	return -1;
	
	entry->d_fileno = ino;
	//entry->d_filesize = inode.i_size;
	entry->d_reclen = reclen;
	entry->d_namelen = namelen;
	memcpy(entry->d_name, dirfile->name, namelen);
	entry->d_name[namelen] = 0;
	
	Dprintf("EXT2DEBUG: %s, created %s\n", __FUNCTION__, entry->d_name);
	return 0;
}

// Note: *dirent may be a pointer into a bdesc and so can become invalid
// TODO really, this shouldn't return inode == 0, since it's annoying, but then to iterate to find free space it's more work =(
static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent)
{
	Dprintf("EXT2DEBUG: %s %u\n", __FUNCTION__, *basep);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, file_blockno, num_file_blocks, block_offset;
	
	num_file_blocks = f->f_ip->i_blocks / (object->blocksize / 512);
	block_offset = *basep % object->blocksize;
	
	if(*basep >= f->f_ip->i_size)
		return -1; // should be: -ENOENT;
	
	blockno = *basep / object->blocksize;
	file_blockno = get_file_block(object, f, *basep);
	
	if(file_blockno == INVALID_BLOCK)
		return -1;
	
	dirblock = CALL(info->ubd, read_block, file_blockno, 1, NULL);
	if(!dirblock)
		return -1;
	
	// Callers must deal with *dirent pointing into a bdesc
	*dirent = (EXT2_Dir_entry_t *) (bdesc_data(dirblock) + block_offset);
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
	
	if(!basep || !file || !entry)
		return -1;
	
	if(f->f_type != TYPE_DIR)
		return -ENOTDIR;
	
	do {
		r = ext2_get_disk_dirent(object, f, basep, &dirent);
		if(r < 0)
			return r;
	} while(!dirent->inode); /* rec_len is zero if a dirent is used to fill a large gap */
	
	return fill_dirent(info, dirent, dirent->inode, entry, size, basep);
}

/* FIXME: this function does not deallocate blocks on failures */
static int ext2_append_file_block_set(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** tail, patch_pass_set_t * befores, int ioff1, int ioff2)
{
	Dprintf("EXT2DEBUG: %s %d\n", __FUNCTION__, block);
	struct ext2_info * info = (struct ext2_info *) object;
	const uint32_t n_per_block = object->blocksize / sizeof(uint32_t);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t nblocks;
	
	DEFINE_PATCH_PASS_SET(set, 2, NULL);
	patch_pass_set_t * inode_dep = PASS_PATCH_SET(set);
	set.array[0] = info->write_head ? *info->write_head : NULL;
	set.array[1] = NULL;
	/* we only need size 2 in some cases */
	set.size = 1;
	
	assert(tail && f && block != INVALID_BLOCK && f->f_type != TYPE_SYMLINK);
	
	/* calculate current number of blocks */
	nblocks = f->f_ip->i_blocks / (object->blocksize / 512);
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
		INODE_SET(f, i_block[nblocks], block);
		inode_dep = befores;
	}
	else if(nblocks < EXT2_NDIRECT + n_per_block)
	{
		int r;
		bdesc_t * indirect;
		uint32_t indirect_number;
		
		nblocks -= EXT2_NDIRECT;
		
		if(!nblocks)
		{
			/* allocate the indirect block */
			indirect_number = ext2_allocate_block(object, file, PURPOSE_INDIRECT, &set.array[0]);
			if(indirect_number == INVALID_BLOCK)
				return -ENOSPC;
			indirect = ext2_synthetic_lookup_block(object, indirect_number, NULL);
			if(!indirect)
				return -ENOSPC;
			r = patch_create_init(indirect, info->ubd, &set.array[0]);
			if(r < 0)
				return r;
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, set.array[0], "init indirect block");
			
			/* there are no references to the indirect block yet, so we can update it without depending on befores */
			r = patch_create_byte(indirect, info->ubd, 0, sizeof(uint32_t), &block, &set.array[0]);
			if(r < 0)
				return r;
			/* however, updating the inode itself must then depend on befores */
			set.next = befores;
			
			/* these changes will be written later, depending on inode_dep (set) */
			INODE_ADD(f, i_blocks, object->blocksize / 512);
			INODE_SET(f, i_block[EXT2_INDIRECT], indirect_number);
		}
		else
		{
			int offset = nblocks * sizeof(uint32_t);
			indirect_number = f->f_ip->i_block[EXT2_INDIRECT];
			indirect = ext2_lookup_block(object, indirect_number, NULL);
			if(!indirect)
				return -ENOSPC;
			/* the indirect block is already referenced, so updating it has to depend on befores */
			r = patch_create_byte_set(indirect, info->ubd, offset, sizeof(uint32_t), &block, &set.array[0], befores);
			if(r < 0)
				return r;
		}
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, set.array[0], "add block");
		indirect->flags |= BDESC_FLAG_INDIR;
		
		r = CALL(info->ubd, write_block, indirect, indirect_number);
		if(r < 0)
			return r;
	}
	else
	{
		int r, offset;
		bdesc_t * indirect = NULL;
		patch_t * indir_init = set.array[0]; /* write_head */
		uint32_t indirect_number;
		bdesc_t * dindirect = NULL;
		patch_t * dindir_init = set.array[0]; /* write_head */
		uint32_t dindirect_number;
		
		nblocks -= EXT2_NDIRECT + n_per_block;
		
		if(!nblocks)
		{
			/* allocate and init doubly indirect block */
			dindirect_number = ext2_allocate_block(object, file, PURPOSE_DINDIRECT, &dindir_init);
			if(dindirect_number == INVALID_BLOCK)
				return -ENOSPC;
			dindirect = ext2_synthetic_lookup_block(object, dindirect_number, NULL);
			if(!dindirect)
				return -ENOSPC;
			r = patch_create_init(dindirect, info->ubd, &dindir_init);
			if(r < 0)
				return r;
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, dindir_init, "init double indirect block");
			
			/* these changes will be written later, depending on inode_dep (set) */
			INODE_ADD(f, i_blocks, object->blocksize / 512);
			INODE_SET(f, i_block[EXT2_DINDIRECT], dindirect_number);
		}
		else
		{
			dindirect_number = f->f_ip->i_block[EXT2_DINDIRECT];
			dindirect = ext2_lookup_block(object, dindirect_number, NULL);
			if(!dindirect)
				return -ENOSPC;
		}
		dindirect->flags |= BDESC_FLAG_INDIR;
		
		if(!(nblocks % n_per_block))
		{
			/* allocate and init indirect block */
			indirect_number = ext2_allocate_block(object, file, PURPOSE_INDIRECT, &indir_init);
			if(indirect_number == INVALID_BLOCK)
				return -ENOSPC;
			indirect = ext2_synthetic_lookup_block(object, indirect_number, NULL);
			if(!indirect)
				return -ENOSPC;
			r = patch_create_init(indirect, info->ubd, &indir_init);
			if(r < 0)
				return r;
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, indir_init, "init indirect block");
			
			set.next = befores;
			if(!nblocks)
			{
				/* in the case where we are also allocating the doubly indirect
				 * block, the inode can depend directly on everything and no
				 * dependencies are necessary between the other changes involved */
				set.array[1] = dindir_init;
				r = patch_create_byte(dindirect, info->ubd, 0, sizeof(uint32_t), &indirect_number, &set.array[1]);
			}
			else
			{
				offset = (nblocks / n_per_block) * sizeof(uint32_t);
				set.array[0] = indir_init;
				r = patch_create_byte_set(dindirect, info->ubd, offset, sizeof(uint32_t), &indirect_number, &set.array[1], PASS_PATCH_SET(set));
				set.next = NULL;
			}
			if(r < 0)
				return r;
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, set.array[1], "add indirect block");
			
			/* the cases involving allocating an indirect block require a larger set */
			set.size = 2;
			
			/* this change will be written later, depending on inode_dep (set) */
			INODE_ADD(f, i_blocks, object->blocksize / 512);
			
			set.array[0] = indir_init;
			r = patch_create_byte(indirect, info->ubd, 0, sizeof(uint32_t), &block, &set.array[0]);
			if(r < 0)
				return r;
		}
		else
		{
			offset = nblocks / n_per_block;
			indirect_number = ((uint32_t *) bdesc_data(dindirect))[offset];
			indirect = ext2_lookup_block(object, indirect_number, NULL);
			if(!indirect)
				return -ENOSPC;
			offset = (nblocks % n_per_block) * sizeof(uint32_t);
			r = patch_create_byte_set(indirect, info->ubd, offset, sizeof(uint32_t), &block, &set.array[0], befores);
			if(r < 0)
				return r;
		}
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, set.array[0], "add block");
		indirect->flags |= BDESC_FLAG_INDIR;
		
		r = CALL(info->ubd, write_block, indirect, indirect_number);
		if(r < 0)
			return r;
		
		if(!(nblocks % n_per_block))
		{
			/* we write this one second since it probably
			 * should be written second (to the disk) */
			r = CALL(info->ubd, write_block, dindirect, dindirect_number);
			if(r < 0)
				return r;
		}
	}
	
	/* increment i_blocks for the block itself */
	INODE_ADD(f, i_blocks, object->blocksize / 512);
	return ext2_write_inode_set(info, f, tail, inode_dep, ioff1, ioff2);
}

static int ext2_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	DECL_INODE_MOD(f);
	return ext2_append_file_block_set(object, file, block, head, PASS_PATCH_SET(set), ioff1, ioff2);
}

static int ext2_write_dirent_extend_set(LFS_t * object, ext2_fdesc_t * parent,
                                        EXT2_Dir_entry_t * dirent_exists,
                                        EXT2_Dir_entry_t * dirent_new, uint32_t basep,
                                        patch_t ** tail, patch_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno;
	bdesc_t * dirblock;
	int r;
	// check: off end of file?
	if(!parent || !dirent_exists || !dirent_new || !tail)
		return -EINVAL;
	
	if(basep + dirent_exists->rec_len + dirent_new->rec_len > parent->f_ip->i_size)
		return -EINVAL;
	
	uint32_t exists_rec_len_actual = dirent_rec_len(dirent_exists->name_len);
	uint32_t new_rec_len_actual = dirent_rec_len(dirent_new->name_len);
	
	// dirents are in a single block:
	if(basep % object->blocksize + exists_rec_len_actual + new_rec_len_actual <= object->blocksize)
	{
		EXT2_Dir_entry_t entries[2];
		
		// it would be brilliant if we could cache this, and not call get_file_block, read_block =)
		blockno = get_file_block(object, parent, basep);
		if(blockno == INVALID_BLOCK)
			return -1;
		
		basep %= object->blocksize;
		
		dirblock = CALL(info->ubd, read_block, blockno, 1, NULL);
		if(!dirblock)
			return -1;
		
		memcpy(entries, dirent_exists, exists_rec_len_actual);
		memcpy((void *) entries + exists_rec_len_actual, dirent_new, new_rec_len_actual);
		
		if((r = patch_create_byte_set(dirblock, info->ubd, basep, exists_rec_len_actual + new_rec_len_actual, (void *) entries, tail, befores )) < 0)
			return r;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *tail, "write dirent '%s'", dirent_new->name);
		dirblock->flags |= BDESC_FLAG_DIRENT;
		
		r = CALL(info->ubd, write_block, dirblock, blockno);
		if(r < 0)
			return r;
	}
	else
		kpanic("overlapping dirent");
	return 0;
}

static int ext2_write_dirent_set(LFS_t * object, ext2_fdesc_t * parent, EXT2_Dir_entry_t * dirent,
                                 uint32_t basep, patch_t ** tail, patch_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blockno;
	bdesc_t * dirblock;
	int r;
	// check: off end of file?
	if(!parent || !dirent || !tail)
		return -EINVAL;
	
	if(basep + dirent->rec_len > parent->f_ip->i_size)
		return -EINVAL;
	
	// dirent is in a single block:
	uint32_t actual_rec_len = dirent_rec_len(dirent->name_len);
	if(basep % object->blocksize + actual_rec_len <= object->blocksize)
	{
		// it would be brilliant if we could cache this, and not call get_file_block, read_block =)
		blockno = get_file_block(object, parent, basep);
		if(blockno == INVALID_BLOCK)
			return -1;
		
		basep %= object->blocksize;
		
		dirblock = CALL(info->ubd, read_block, blockno, 1, NULL);
		if(!dirblock)
			return -1;
		
		if((r = patch_create_byte_set(dirblock, info->ubd, basep, actual_rec_len, (void *) dirent, tail, befores)) < 0)
			return r;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *tail, "write dirent '%s'", dirent->name);
		dirblock->flags |= BDESC_FLAG_DIRENT;
		
		r = CALL(info->ubd, write_block, dirblock, blockno);
		if(r < 0)
			return r;
	}
	else
		kpanic("overlapping dirent");
	return 0;
}

static int ext2_write_dirent(LFS_t * object, ext2_fdesc_t * parent, EXT2_Dir_entry_t * dirent,
				 uint32_t basep, patch_t ** head)
{
	DEFINE_PATCH_PASS_SET(set, 1, NULL);
	set.array[0] = *head;
	return ext2_write_dirent_set(object, parent, dirent, basep, head, PASS_PATCH_SET(set));
}

static int ext2_insert_dirent_set(LFS_t * object, ext2_fdesc_t * parent, ext2_mdir_t * mdir, EXT2_Dir_entry_t * new_dirent, ext2_mdirent_t ** pmdirent, patch_t ** tail, patch_pass_set_t * befores)
{
	Dprintf("EXT2DEBUG: ext2_insert_dirent %s\n", new_dirent->name);
	const EXT2_Dir_entry_t * entry;
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t prev_eof = parent->f_ip->i_size, new_block;
	int r;
	bdesc_t * block;
	patch_t * append_patch;
	ext2_mdirent_t * mdirent;
	DEFINE_PATCH_PASS_SET(set, 1, befores);
	set.array[0] = NULL;
	
	r = ext2_mdir_get(object, parent, &mdir);
	if(r < 0)
		return r;
	
	if(parent->f_ip->i_size)
	{
		for(mdirent = mdir->free_first; mdirent; mdirent = mdirent->freel.next)
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
				{
					ext2_mdirent_clear(mdir, mdirent, object->blocksize);
					return r;
				}
				patch_weak_retain(*tail, &mdirent->create, NULL, NULL);
				*pmdirent = mdirent;
				return 0;
			}
			if(mdirent->dirent.inode && mdirent->dirent.rec_len - (8 + mdirent->dirent.name_len) > new_dirent->rec_len)
			{
				EXT2_Dir_entry_t entry_updated;
				uint16_t entry_updated_len;
				uint32_t existing_offset = mdirent->offset;
				uint32_t new_offset;
				uint16_t backup_rec_len = new_dirent->rec_len;
				ext2_mdirent_t * nmdirent;
				r = ext2_get_disk_dirent(object, parent, &existing_offset, &entry);
				if(r < 0)
					return r;
				existing_offset = mdirent->offset;
				memcpy(&entry_updated, entry, MIN(entry->rec_len, sizeof(entry_updated)));
				entry_updated_len = dirent_rec_len(entry_updated.name_len);
				new_dirent->rec_len = entry_updated.rec_len - entry_updated_len;
				entry_updated.rec_len = entry_updated_len;
				
				new_offset = existing_offset + entry_updated.rec_len;
				r = ext2_mdirent_split(mdir, mdirent, &entry_updated, new_dirent, &nmdirent);
				if(r < 0)
				{
					new_dirent->rec_len = backup_rec_len;
					return r;
				}
				r = ext2_write_dirent_extend_set(object, parent, &entry_updated, new_dirent, existing_offset, tail, befores);
				if(r < 0)
					assert(0); // TODO: join the existing and new mdirents
				else
					patch_weak_retain(*tail, &nmdirent->create, NULL, NULL);
				*pmdirent = nmdirent;
				return 0;
			}
		}
	}
	
	// test the aligned case! test by having a 16 whatever file
	new_block = ext2_allocate_block(object, (fdesc_t *) parent, PURPOSE_DIRDATA, &set.array[0]);
	if(new_block == INVALID_BLOCK)
		return -ENOSPC;
	/* FIXME: these errors should all free the block we allocated! */
	block = CALL(info->ubd, synthetic_read_block, new_block, 1, NULL);
	if(block == NULL)
		return -ENOSPC;
	r = patch_create_init(block, info->ubd, &set.array[0]);
	if(r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, set.array[0], "init new dirent block");
	block->flags |= BDESC_FLAG_DIRENT;
	r = CALL(info->ubd, write_block, block, new_block);
	if(r < 0)
		return r;
	DECL_INODE_MOD(parent);
	INODE_ADD(parent, i_size, object->blocksize);
	r = ext2_append_file_block_set(object, (fdesc_t *) parent, new_block, &append_patch, PASS_PATCH_SET(set), ioff1, ioff2);
	if(r < 0)
		return r;
	lfs_add_fork_head(append_patch);
	
	new_dirent->rec_len = object->blocksize;
	r = ext2_mdirent_add(mdir, new_dirent, prev_eof, &mdirent);
	if(r < 0)
		return r;
	r = ext2_write_dirent_set(object, parent, new_dirent, prev_eof, tail, PASS_PATCH_SET(set));
	assert(r >= 0); // need to undo ext2_dir_add()
	patch_weak_retain(*tail, &mdirent->create, NULL, NULL);
	*pmdirent = mdirent;
	return r;
}

static int find_free_inode_block_group(LFS_t * object, inode_t * ino)
{
	Dprintf("EXT2DEBUG: %s inode number is %u\n", __FUNCTION__, *ino);
	struct ext2_info * info = (struct ext2_info *) object;
	bdesc_t * bitmap;
	inode_t curr = 0;
	
	if(*ino > info->super->s_inodes_count)
	{
		printf("%s requested status of inode %u too large!\n",__FUNCTION__, *ino);
		return -ENOSPC;
	}
	
	curr = *ino;
	
	uint32_t block_group = curr / info->super->s_inodes_per_group;
	
	/* TODO: clean this up like ext2_find_free_block() */
	short firstrun = 1;
	while(block_group != (*ino / info->super->s_inodes_per_group) || firstrun)
	{
		if(info->inode_gdesc != block_group || info->inode_cache == NULL)
		{
			if(info->inode_cache != NULL)
				bdesc_release(&info->inode_cache);	
			info->inode_gdesc = block_group;
			bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_inode_bitmap, 1, NULL);
			if(!bitmap)
				return -ENOSPC;
			bdesc_retain(bitmap);
			bitmap->flags |= BDESC_FLAG_BITMAP;
			info->inode_cache = bitmap;
			info->inode_cache_number = info->groups[block_group].bg_inode_bitmap;
		}
		
		const unsigned long * array = (unsigned long *) bdesc_data(info->inode_cache);
		//assert(!(curr % info->super->s_inodes_per_group));
		int index = find_first_zero_bit(array, info->super->s_inodes_per_group/*, (curr % info->super->s_inodes_per_group)*/ );
		if(index < (info->super->s_inodes_per_group))
		{
			curr += index + 1;
			*ino = curr;
			//printf("returning inode number %d\n", *ino);
			return EXT2_FREE;
		}
		
		firstrun = 0;
		block_group = (block_group + 1) % info->ngroups;
		curr = block_group * info->super->s_inodes_per_group;	
	}
	
	return -ENOSPC;
}

static inode_t ext2_find_free_inode(LFS_t * object, inode_t parent)
{
	Dprintf("EXT2DEBUG: %s parent is %u\n", __FUNCTION__, parent);
	struct ext2_info * info = (struct ext2_info *) object;
	inode_t ino = 0;
	int r;
	
	ino = (parent / info->super->s_inodes_per_group) * info->super->s_inodes_per_group;
	r = find_free_inode_block_group(object, &ino);
	if(r != -ENOSPC)
	{
		return ino;
	}
	
	return EXT2_BAD_INO;
}

static int ext2_set_symlink(LFS_t * object, ext2_fdesc_t * f, const void * data, uint32_t size, patch_t ** head, int * ioff1p, int * ioff2p)
{
	struct ext2_info * info = (struct ext2_info *) object;
	int ioff1 = *ioff1p, ioff2 = *ioff2p;
	
	if(size > object->blocksize)
		return -ENAMETOOLONG;
	
	if(size <= EXT2_N_BLOCKS * sizeof(uint32_t))
	{
		if(f->f_ip->i_size > EXT2_N_BLOCKS * sizeof(uint32_t))
		{
			_ext2_free_block(object, f->f_ip->i_block[0], head);
			INODE_SET(f, i_block[0], 0);
		}
		
		if(ioff1 > offsetof(EXT2_inode_t, i_block))
			ioff1 = offsetof(EXT2_inode_t, i_block);
		if(ioff2 < offsetof(EXT2_inode_t, i_block) + size)
			ioff2 = offsetof(EXT2_inode_t, i_block) + size;
		memcpy((char *) f->f_xinode.i_block, data, size);
		
	}
	else
	{
		// allocate a block, link it into the inode, write the file, write the inodeo
		DEFINE_PATCH_PASS_SET(set, 2, NULL);
		set.array[0] = *head;
		set.array[1] = NULL;
		set.size = 1;
	
		if(f->f_ip->i_size <= EXT2_N_BLOCKS * sizeof(uint32_t))
		{
			uint32_t bno = ext2_allocate_block(object, (fdesc_t *) f, PURPOSE_FILEDATA, &set.array[1]);
			if(bno == INVALID_BLOCK)
				return -EINVAL;
			
			INODE_SET(f, i_block[0], bno);
			set.size = 2;
		}
		
		bdesc_t * b = CALL(info->ubd, synthetic_read_block, f->f_ip->i_block[0], 1, NULL);
		if(!b)
			return -EINVAL;
		
		int r = patch_create_byte_set(b, info->ubd, 0, size, (void *) data, head, PASS_PATCH_SET(set));
		if(r < 0)
			return r;
		
		r = CALL(info->ubd, write_block, b, f->f_ip->i_block[0]);
		if(r < 0)
			return r;
	}
	
	INODE_SET(f, i_size, size); // size must include zerobyte!
	*ioff1p = ioff1;
	*ioff2p = ioff2;
	return 0;
}

static fdesc_t * ext2_allocate_name(LFS_t * object, inode_t parent_ino, const char * name,
                                    uint8_t type, fdesc_t * link, const metadata_set_t * initialmd,
                                    inode_t * new_ino, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_allocate_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * parent_file = NULL, * new_file = NULL;
	uint16_t mode;
	int r, name_len;
	ext2_fdesc_t * ln = (ext2_fdesc_t *) link;
	EXT2_Dir_entry_t new_dirent;
	char * link_buf = NULL;
	ext2_mdir_t * mdir;
	ext2_mdirent_t * mdirent;
	ext2_minode_t * minode = NULL;
	DEFINE_PATCH_PASS_SET(head_set, 2, NULL);
	
	// what is link? link is a symlink fdesc. dont deal with it, yet.
	assert(head);
	
	// Don't link files of different types
	assert(!ln || type == ln->f_type);
	
	// TODO: we need some way to prevent regular users from creating . and ..
	
	name_len = strlen(name);
	if(name_len >= EXT2_NAME_LEN)
		return NULL;
	
	switch(type)
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
	
	// this might be redundant:
	parent_file = (ext2_fdesc_t *) ext2_lookup_inode(object, parent_ino);
	if(!parent_file)
		return NULL;
	
	if(!ln)
	{
		inode_t ino = ext2_find_free_inode(object, parent_ino);
		uint32_t x32;
		uint16_t x16;
		
		if(ino == EXT2_BAD_INO)
			goto allocate_name_exit;
		
		new_file = (ext2_fdesc_t *) ext2_lookup_inode(object, ino);
		if(!new_file)
			goto allocate_name_exit;
		
		minode = ext2_minode_create(&info->minode_cache, ino);
		if(!minode)
			goto allocate_name_exit;
		
		new_file->f_type = type;
		
		INODE_CLEAR(new_file);
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UID, sizeof(x32), &x32);
		if(r > 0)
			new_file->f_xinode.i_uid = x32;
		else if(r == -ENOENT)
			new_file->f_xinode.i_uid = 0;
		else
			assert(0);
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_GID, sizeof(x32), &x32);
		if(r > 0)
			new_file->f_xinode.i_gid = x32;
		else if(r == -ENOENT)
			new_file->f_xinode.i_gid = 0;
		else
			assert(0);
		
		new_file->f_xinode.i_mode = mode | EXT2_S_IRUSR | EXT2_S_IWUSR;
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UNIX_PERM, sizeof(x16), &x16);
		if(r > 0)
			new_file->f_xinode.i_mode |= x16;
		else if(r != -ENOENT)
			assert(0);
		
		new_file->f_xinode.i_links_count = 1;
		
		head_set.array[1] = info->write_head ? *info->write_head : NULL;
		r = ext2_write_inode_bitmap(object, ino, 1, &head_set.array[1]);
		if(r != 0)
			goto allocate_name_exit2;
		
		if(type == TYPE_SYMLINK)
		{
			int ioff1 = 0, ioff2 = 0;
			link_buf = malloc(object->blocksize);
			if(!link_buf)
			{
				r = -ENOMEM;
				goto allocate_name_exit2;
			}
			r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_SYMLINK, object->blocksize, link_buf);
			if(r < 0)
				goto allocate_name_exit2;
			r = ext2_set_symlink(object, new_file, link_buf, r, &head_set.array[1], &ioff1, &ioff2);
			if(r < 0)
				goto allocate_name_exit2;
		}
		else if(type == TYPE_DIR)
		{
			// Create . and ..
			uint32_t dirblock_no;
			bdesc_t * dirblock_bdesc;
			patch_t * init_head;
			EXT2_Dir_entry_t dir_dirent;
			uint32_t prev_basep, group;
			DEFINE_PATCH_PASS_SET(inode_set, 5, NULL);
			inode_set.array[0] = *head;
			inode_set.array[1] = head_set.array[1];
			
			// allocate and append first directory entry block
			dirblock_no = ext2_allocate_block(object, (fdesc_t *) new_file, 1, &init_head);
			dirblock_bdesc = CALL(info->ubd, synthetic_read_block, dirblock_no, 1, NULL);
			r = patch_create_init(dirblock_bdesc, info->ubd, &init_head);
			assert(r >= 0);
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, init_head, "init new dirent block");
			new_file->f_xinode.i_block[0] = dirblock_no;
			new_file->f_xinode.i_size = object->blocksize;
			new_file->f_xinode.i_blocks = object->blocksize / 512;
			
			// should "." and ".." be inserted into the mdirent cache with their
			// creation patch(es)?
			
			// insert "."
			dir_dirent.inode = ino;
			strcpy(dir_dirent.name, ".");
			dir_dirent.name_len = strlen(dir_dirent.name);
			dir_dirent.rec_len = dirent_rec_len(dir_dirent.name_len);
			dir_dirent.file_type = EXT2_TYPE_DIR;
			inode_set.array[2] = init_head;
			r = patch_create_byte(dirblock_bdesc, info->ubd, 0, dir_dirent.rec_len, &dir_dirent, &inode_set.array[2]);
			assert(r >= 0);
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, inode_set.array[2], "write dirent '.'");
			new_file->f_xinode.i_links_count++;
			prev_basep = dir_dirent.rec_len;
			
			DECL_INODE_MOD(parent_file);
			INODE_ADD(parent_file, i_links_count, 1);
			inode_set.array[3] = info->write_head ? *info->write_head : NULL;
			r = ext2_write_inode(info, parent_file, &inode_set.array[3], ioff1, ioff2);
			assert(r >= 0);
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, inode_set.array[3], "linkcount++");
			
			// insert ".."
			dir_dirent.inode = parent_ino;
			strcpy(dir_dirent.name, "..");
			dir_dirent.name_len = strlen(dir_dirent.name);
			dir_dirent.rec_len = object->blocksize - prev_basep;
			dir_dirent.file_type = EXT2_TYPE_DIR;
			inode_set.array[4] = init_head;
			r = patch_create_byte(dirblock_bdesc, info->ubd, prev_basep, dirent_rec_len(dir_dirent.name_len), &dir_dirent, &inode_set.array[4]);
			assert(r >= 0);
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, inode_set.array[4], "write dirent '..'");
			prev_basep = dir_dirent.rec_len;
			
			dirblock_bdesc->flags |= BDESC_FLAG_DIRENT;
			r = CALL(info->ubd, write_block, dirblock_bdesc, dirblock_no);
			assert(r >= 0);
			
			group = (new_file->f_ino - 1) / info->super->s_inodes_per_group;
			r = ext2_super_report(object, group, 0, 0, 1);
			if(r < 0)
				goto allocate_name_exit2;
			
			r = ext2_write_inode_set(info, new_file, &head_set.array[1], PASS_PATCH_SET(inode_set), 0, sizeof(EXT2_inode_t));
		}
		
		if(type != TYPE_DIR)
			r = ext2_write_inode(info, new_file, &head_set.array[1], 0, sizeof(EXT2_inode_t));
		if(r < 0)
			goto allocate_name_exit2;
		patch_weak_retain(head_set.array[1], &minode->create, NULL, NULL);
		*new_ino = ino;
	}
	else
	{
		new_file = (ext2_fdesc_t *) ext2_lookup_inode(object, ln->f_ino);
		
		assert(ln == new_file);
		if(!new_file)
			goto allocate_name_exit;
		*new_ino = ln->f_ino;
		
		// Increase link count
		DECL_INODE_MOD(ln);
		INODE_ADD(ln, i_links_count, 1);
		head_set.array[1] = info->write_head ? *info->write_head : NULL;
		r = ext2_write_inode(info, ln, &head_set.array[1], ioff1, ioff2);
		if(r < 0)
			goto allocate_name_exit2;
		
		minode = ext2_minode_get(&info->minode_cache, ln->f_ino);
	}
	
	// create the directory entry
	new_dirent.inode = *new_ino;
	new_dirent.name_len = name_len;
	memcpy(new_dirent.name, name, name_len);
	new_dirent.name[name_len] = 0;
	// round len up to multiple of 4 bytes:
	// (this value just computed for searching for a slot)
	new_dirent.rec_len = dirent_rec_len(name_len);
	switch(type)
	{
		case TYPE_DIR:
			new_dirent.file_type = EXT2_TYPE_DIR;
			break;
		case TYPE_FILE:
			new_dirent.file_type = EXT2_TYPE_FILE;
			break;
		case TYPE_SYMLINK:
			new_dirent.file_type = EXT2_TYPE_SYMLINK;
			break;
		default: // TODO: add more types
			new_dirent.file_type = EXT2_TYPE_FILE;
	}
	
	r = ext2_mdir_get(object, parent_file, &mdir);
	assert(r >= 0);
	
	head_set.array[0] = *head;
	r = ext2_insert_dirent_set(object, parent_file, mdir, &new_dirent, &mdirent, head, PASS_PATCH_SET(head_set));
	if(r < 0)
	{
		printf("Inserting a dirent in allocate_name failed for \"%s\"!\n", name);
		goto allocate_name_exit2;
	}
	
	if(minode)
	{
		ext2_minode_retain(minode);
		assert(!mdirent->minode);
		mdirent->minode = minode;
	}
	
	ext2_free_fdesc(object, (fdesc_t *) parent_file);
	return (fdesc_t *) new_file;
	
  allocate_name_exit2:
	free(link_buf);
	ext2_free_fdesc(object, (fdesc_t *)new_file);
	if(!ln && minode)
		ext2_minode_destroy(&info->minode_cache, minode);
allocate_name_exit:
	ext2_free_fdesc(object, (fdesc_t *)parent_file);
	return NULL;
}

static uint32_t ext2_erase_block_ptr(LFS_t * object, ext2_fdesc_t * f, patch_t ** head, int * ioff1p, int * ioff2p)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, f, f->f_ip->i_size);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t blocknum, n_per_block;
	bdesc_t * block_desc, * double_block_desc;
	uint32_t indir_ptr, double_indir_ptr;
	int r, ioff1 = *ioff1p, ioff2 = *ioff2p;
	uint32_t target = INVALID_BLOCK;
	
	n_per_block = object->blocksize / (sizeof(uint32_t));
	
	// non block aligned offsets suck (aka aren't supported)
	
	if(f->f_ip->i_size <= object->blocksize)
		blocknum = 0;
	else if(!(f->f_ip->i_size % object->blocksize))
		blocknum = (f->f_ip->i_size / object->blocksize) - 1;
	else
		blocknum = f->f_ip->i_size / object->blocksize;
	
	if(blocknum < EXT2_NDIRECT)
	{
		target = f->f_ip->i_block[blocknum];
		INODE_SET(f, i_block[blocknum], 0);
		if(f->f_ip->i_size > object->blocksize)
			INODE_ADD(f, i_size, -object->blocksize);
		else
			INODE_SET(f, i_size, 0);
		
	}
	else if(blocknum < EXT2_NDIRECT + n_per_block)
	{
		uint32_t * block_nums;
		blocknum -= EXT2_NDIRECT;
		block_desc = CALL(info->ubd, read_block, f->f_ip->i_block[EXT2_INDIRECT], 1, NULL);
		if(!block_desc)
			return INVALID_BLOCK;
		block_nums = (uint32_t *) bdesc_data(block_desc);
		target = block_nums[blocknum];
		
		if(!blocknum)
		{
			indir_ptr = f->f_ip->i_block[EXT2_INDIRECT];
			INODE_ADD(f, i_size, -object->blocksize);
			r = _ext2_free_block(object, indir_ptr, head);
			if(r < 0)
				return INVALID_BLOCK;
			INODE_ADD(f, i_blocks, -(object->blocksize / 512));
			INODE_SET(f, i_block[EXT2_INDIRECT], 0);
		}
		else
		{
			INODE_ADD(f, i_size, -object->blocksize);
			//r = patch_create_byte(block_desc, info->ubd, blocknum * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
			//if(r < 0)
			//	return INVALID_BLOCK;
			//r = CALL(info->ubd, write_block, block_desc, block_desc->xxx_number);
			//if(r < 0)
			//	return INVALID_BLOCK;
		}
	}
	else if(blocknum < EXT2_NDIRECT + n_per_block + n_per_block * n_per_block)
	{
		uint32_t * block_nums, * double_block_nums;
		blocknum -= (EXT2_NDIRECT + n_per_block);
		block_desc = CALL(info->ubd, read_block, f->f_ip->i_block[EXT2_DINDIRECT], 1, NULL);
		if(!block_desc)
			return INVALID_BLOCK;
		block_nums = (uint32_t *) bdesc_data(block_desc);
		indir_ptr = block_nums[blocknum / n_per_block];
		double_block_desc = CALL(info->ubd, read_block, indir_ptr, 1, NULL);
		if(!block_desc)
			return INVALID_BLOCK;
		double_block_nums = (uint32_t *) bdesc_data(double_block_desc);
		double_indir_ptr = (blocknum % n_per_block);
		target = double_block_nums[double_indir_ptr];
		
		INODE_ADD(f, i_size, -object->blocksize);
		
		if(!(blocknum % n_per_block))
		{
			if(!blocknum)
			{
				r = _ext2_free_block(object, f->f_ip->i_block[EXT2_DINDIRECT], head);
				if(r < 0)
					return INVALID_BLOCK;
				INODE_ADD(f, i_blocks, -(object->blocksize / 512));
				INODE_SET(f, i_block[EXT2_DINDIRECT], 0);
			}
			else
			{
				//r = patch_create_byte(block_desc, info->ubd, (blocknum / n_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
				//if(r < 0)
				//	return INVALID_BLOCK;
				//r = CALL(info->ubd, write_block, block_desc, block_desc->xxx_number);
				//if(r < 0)
				//	return INVALID_BLOCK;
			}
			r = _ext2_free_block(object, indir_ptr, head);
			if(r < 0)
				return INVALID_BLOCK;
			INODE_ADD(f, i_blocks, -(object->blocksize / 512));
		}
		else
		{
			//r = patch_create_byte(double_block_desc, info->ubd, (blocknum % n_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
			//if(r < 0)
			//	return INVALID_BLOCK;
			//r = CALL(info->ubd, write_block, double_block_desc, double_block_desc->xxx_number);
			//if(r < 0)
			//	return INVALID_BLOCK;
		}
	}
	else
	{
		Dprintf("Triply indirect blocks are not implemented.\n");
		assert(0);
	}
	
	*ioff1p = ioff1;
	*ioff2p = ioff2;
	return target;
}

static uint32_t ext2_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_truncate_file_block\n");
	int r;
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t blockno;
	
	assert(f && f->f_ip->i_blocks > 0 && f->f_type != TYPE_SYMLINK);
	
	if(!f->f_ip->i_size)
		return INVALID_BLOCK;
	
	// Update ext2_mdir code if we want to directory truncation
	assert(f->f_type != TYPE_DIR);
	
	// FIXME: need to do [d]indirect block count decrement, and write it, here!
	DECL_INODE_MOD(f);
	INODE_ADD(f, i_blocks, -(object->blocksize / 512));
	
	// ext2_erase_block_ptr will either return INVALID_BLOCK, or the block that was truncated...
	blockno = ext2_erase_block_ptr(object, f, head, &ioff1, &ioff2);
	
	if(blockno != INVALID_BLOCK)
	{
		r = ext2_write_inode(info, f, head, ioff1, ioff2);
		if(r < 0)
			blockno = INVALID_BLOCK;
	}
	
	return blockno;
}

static int empty_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	return -ENOENT;
}

static int ext2_dir_rename(LFS_t * object, ext2_fdesc_t * foparent, ext2_mdir_t * omdir, ext2_mdirent_t * omdirent, ext2_fdesc_t * fold,
                           ext2_fdesc_t * fnparent, ext2_fdesc_t * fnew, const char * newname, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_dir_rename %u:%u -> %u:%s\n", foparent->f_ino, fold->f_ino, fnparent->f_ino, newname);
	struct ext2_info * info = (struct ext2_info *) object;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };
	DEFINE_PATCH_PASS_SET(set, 2, NULL);
	ext2_mdir_t * rdir;
	ext2_mdirent_t * dotdot;
	EXT2_Dir_entry_t copy;
	inode_t newino;
	int r;
	
	/* cannot overwrite anything with a directory */
	if(fnew)
	{
		r = -EPERM;
		goto exit_fnew;
	}
	
	/* Linux has already made sure that fnparent is not a subdirectory of
	 * fold, so we need not check that here. However, this is where we'd do
	 * it if we had to. */
	
	set.array[0] = *head;
	/* step 1: create a new hardlink to the directory (also increments link count) */
	fnew = (ext2_fdesc_t *) ext2_allocate_name(object, fnparent->f_ino, newname, fold->f_type, (fdesc_t *) fold, &emptymd, &newino, &set.array[0]);
	if(!fnew)
	{
		r = -1;
		goto exit_fnparent;
	}
	assert(fold->f_ino == newino);
	
	/* step 2: increment the new parent link count */
	{
		DECL_INODE_MOD(fnparent);
		INODE_ADD(fnparent, i_links_count, 1);
		set.array[1] = *head;
		r = ext2_write_inode(info, fnparent, &set.array[1], ioff1, ioff2);
		if(r < 0)
			goto exit_fnew;
	}
	
	/* step 3: reset .. in the directory, depending on steps 1 and 2 */
	r = ext2_mdir_get(object, fold, &rdir);
	if(r < 0)
		goto exit_fnew;
	dotdot = ext2_mdirent_get(rdir, "..");
	if(!dotdot)
	{
		r = -1;
		goto exit_fnew;
	}
	memcpy(&copy, &dotdot->dirent, MIN(dotdot->dirent.rec_len, sizeof(copy)));
	copy.inode = fnparent->f_ino;
	r = ext2_write_dirent_set(object, fold, &copy, dotdot->offset, head, PASS_PATCH_SET(set));
	if(r < 0)
		goto exit_fnew;
	dotdot->dirent.inode = copy.inode;
	
	/* step 4: decrement the old parent link count, depending on step 3 */
	{
		patch_t * fork_head = *head;
		DECL_INODE_MOD(foparent);
		INODE_ADD(foparent, i_links_count, -1);
		r = ext2_write_inode(info, foparent, &fork_head, ioff1, ioff2);
		if(r < 0)
			goto exit_fnew;
		lfs_add_fork_head(fork_head);
	}
	
	/* step 5: remove the original hardlink, depending on step 3 */
	r = ext2_delete_dirent(object, foparent, omdir, omdirent, head);
	if(r < 0)
		goto exit_fnew;
	
	/* step 6: decrement the link count, depending on step 5 */
	{
		DECL_INODE_MOD(fold);
		INODE_ADD(fold, i_links_count, -1);
		r = ext2_write_inode(info, fold, head, ioff1, ioff2);
		if(r < 0)
			goto exit_fnew;
	}
	
	r = 0;
	
  exit_fnew:
	ext2_free_fdesc(object, (fdesc_t *) fnew);
  exit_fnparent:
	ext2_free_fdesc(object, (fdesc_t *) fnparent);
	ext2_free_fdesc(object, (fdesc_t *) fold);
	ext2_free_fdesc(object, (fdesc_t *) foparent);
	return r;
}

static int ext2_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_rename %u:%s -> %u:%s\n", oldparent, oldname, newparent, newname);
	struct ext2_info * info = (struct ext2_info *) object;
	ext2_mdir_t * omdir, * nmdir;
	ext2_mdirent_t * omdirent, * nmdirent;
	ext2_fdesc_t * fold, * fnew, * foparent, * fnparent;
	bool existing = 0;
	inode_t newino;
	patch_t * prev_head = NULL;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };
	int r;
	
	if(!head)
		return -EINVAL;
	if(strlen(oldname) > EXT2_NAME_LEN || strlen(newname) > EXT2_NAME_LEN)
		return -EINVAL;
	
	if(oldparent == newparent && !strcmp(oldname, newname))
		return 0;
	
	foparent = (ext2_fdesc_t *) ext2_lookup_inode(object, oldparent);
	if(!foparent)
		return -ENOENT;
	r = ext2_mdir_get(object, foparent, &omdir);
	if(r < 0)
		goto exit_foparent;
	omdirent = ext2_mdirent_get(omdir, oldname);
	if(!omdirent)
	{
		r = -ENOENT;
		goto exit_foparent;
	}
	fold = (ext2_fdesc_t *) ext2_lookup_inode(object, omdirent->dirent.inode);
	if(!fold)
	{
		r = -ENOENT;
		goto exit_foparent;
	}
	
	fnparent = (ext2_fdesc_t *) ext2_lookup_inode(object, newparent);
	if(!fnparent)
	{
		r = -ENOENT;
		goto exit_fold;
	}
	r = ext2_mdir_get(object, fnparent, &nmdir);
	if(r < 0)
		goto exit_fold;
	nmdirent = ext2_mdirent_get(nmdir, newname);
	if(nmdirent)
		fnew = (ext2_fdesc_t *) ext2_lookup_inode(object, nmdirent->dirent.inode);
	else
		fnew = NULL;
	
	if(fold->f_type == TYPE_DIR)
		return ext2_dir_rename(object, foparent, omdir, omdirent, fold, fnparent, fnew, newname, head);
	
	if(fnew)
	{
		EXT2_Dir_entry_t copy;
		
		// Overwriting a directory makes little sense
		if(fnew->f_type == TYPE_DIR)
		{
			r = -ENOTEMPTY;
			goto exit_fnew;
		}
		
		memcpy(&copy, &nmdirent->dirent, MIN(nmdirent->dirent.rec_len, sizeof(copy)));
		copy.inode = fold->f_ino;
		
		// File already exists
		existing = 1;
		
		r = ext2_write_dirent(object, fnparent, &copy, nmdirent->offset, head);
		if(r < 0)
			goto exit_fnew;
		prev_head = *head;
		nmdirent->dirent.inode = copy.inode;
		// TODO: can we store *head as the dirent's creation patch?
		
		/* XXX: should this be before the write_dirent above?? */
		DECL_INODE_MOD(fold);
		INODE_ADD(fold, i_links_count, 1);
		r = ext2_write_inode(info, fold, head, ioff1, ioff2);
		assert(r >= 0); // recover mdir and mdirent changes; then exit_fnew
	}
	else
	{
		// Link files together
		fnew = (ext2_fdesc_t *) ext2_allocate_name(object, newparent, newname, fold->f_type, (fdesc_t *) fold, &emptymd, &newino, head);
		if(!fnew)
		{
			r = -1;
			goto exit_fnparent;
		}
		//assert(new_dirent->inode == newino);
	}
	
	r = ext2_delete_dirent(object, foparent, omdir, omdirent, head);
	if(r < 0)
		goto exit_fnew;
	
	{
		DECL_INODE_MOD(fold);
		INODE_ADD(fold, i_links_count, -1);
		r = ext2_write_inode(info, fold, head, ioff1, ioff2);
		if(r < 0)
			goto exit_fnew;
	}
	
	if(existing)
	{
		DECL_INODE_MOD(fnew);
		INODE_ADD(fnew, i_links_count, -1);
		// TODO: the inode update needn't depend on the dirent overwrite
		// if the dirent overwrite merged with the overwritten dirent's create
		r = ext2_write_inode(info, fnew, &prev_head, ioff1, ioff2);
		if(r < 0)
			goto exit_fnew;
		
		if(!fnew->f_ip->i_links_count)
		{
			// TODO: make these dependencies less strict (like in remove_name)
			
			uint32_t i, n = ext2_get_file_numblocks(object, (fdesc_t *) fnew);
			for(i = 0; i < n; i++)
			{
				uint32_t block = ext2_truncate_file_block(object, (fdesc_t *) fnew, &prev_head);
				if(block == INVALID_BLOCK)
				{
					r = -1;
					goto exit_fnew;
				}
				r = _ext2_free_block(object, block, &prev_head);
				if(r < 0)
					goto exit_fnew;
			}
			
			INODE_CLEAR(fnew);
			r = ext2_write_inode(info, fnew, &prev_head, 0, sizeof(EXT2_inode_t));
			if(r < 0)
				goto exit_fnew;
			
			r = ext2_write_inode_bitmap(object, fnew->f_ino, 0, &prev_head);
			if(r < 0)
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

static int _ext2_free_block(LFS_t * object, uint32_t block, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_free_block\n");
	int r;
	
	if(!head || block == INVALID_BLOCK)
		return -EINVAL;
	
	r = ext2_write_block_bitmap(object, block, 0, head);
	if(r < 0)
	{
		Dprintf("failed to free block %d in bitmap\n", block);
		return r;
	}
	
	return r;
}

static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	return _ext2_free_block(object, block, head);
}

static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, ext2_mdir_t * mdir, ext2_mdirent_t * mdirent, patch_t ** phead)
{
	Dprintf("EXT2DEBUG: ext2_delete_dirent %u\n", mdirent->offset);
	struct ext2_info * info = (struct ext2_info *) object;
	uint32_t base = mdirent->offset, prev_base;
	uint32_t base_blockno, prev_base_blockno;
	bdesc_t * dirblock;
	uint16_t len;
	patch_t * head = *phead;
	int r;
	
	if(!(base % object->blocksize))
	{
		// if the base is at the start of a block, zero it out
		EXT2_Dir_entry_t jump_dirent;
		const EXT2_Dir_entry_t * disk_dirent;
		base_blockno = get_file_block(object, dir_file, base);
		if(base_blockno == INVALID_BLOCK)
			return -1;
		dirblock = CALL(info->ubd, read_block, base_blockno, 1, NULL);
		if(!dirblock)
			return -EIO;
		disk_dirent = (const EXT2_Dir_entry_t *) bdesc_data(dirblock);
		jump_dirent.inode = 0;
		jump_dirent.rec_len = disk_dirent->rec_len;
		r = patch_create_byte(dirblock, info->ubd, 0, 6, &jump_dirent, &head);
		if(r < 0)
			return r;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "delete dirent '%s', add jump dirent", mdirent->dirent.name);
		r = CALL(info->ubd, write_block, dirblock, base_blockno);
	}
	else
	{
		// else in the middle of a block, so increase length of prev dirent
		prev_base = ext2_mdirent_offset_prev(mdir, mdirent)->offset;
		prev_base_blockno = get_file_block(object, dir_file, prev_base);
		if(prev_base_blockno == INVALID_BLOCK)
			return -1;
		dirblock = CALL(info->ubd, read_block, prev_base_blockno, 1, NULL);
		if(!dirblock)
			return -1;
		
		// update the length of the previous dirent:
		len = mdirent->dirent.rec_len + ext2_mdirent_offset_prev(mdir, mdirent)->dirent.rec_len;
		r = patch_create_byte(dirblock, info->ubd, (prev_base + 4) % object->blocksize, sizeof(len), (void *) &len, &head);
		if(r < 0)
			return r;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "delete dirent '%s'", mdirent->dirent.name);
	
		r = CALL(info->ubd, write_block, dirblock, prev_base_blockno);
	}
	
	if(r < 0)
	{
		assert(0); // must undo patch creation to recover
		return r;
	}
	
	// Will the dirent never exist on disk?:
	if(head == WEAK(mdirent->create))
	{
		// Create and delete merged so the dirent will never exist on disk.
		// Therefore the caller need not depend on the dirent's deletion
		// (which could otherwise require many disk writes to enforce SU).
		lfs_add_fork_head(head);
#if DELETE_MERGE_STATS
		info->delete_dirent_stats.merged++;
#endif
	}
	else
		*phead = head;
#if DELETE_MERGE_STATS
	if(WEAK(mdirent->create) && !(WEAK(mdirent->create)->flags & PATCH_INFLIGHT))
		info->delete_dirent_stats.uncommitted++;
	info->delete_dirent_stats.total++;
#endif
	ext2_mdirent_clear(mdir, mdirent, object->blocksize);
	
	return 0;
}

static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_remove_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) object;
	patch_t * prev_head;
	ext2_fdesc_t * pfile = NULL, * file = NULL;
	uint8_t minlinks = 1;
	ext2_mdir_t * mdir;
	ext2_mdirent_t * mdirent;
	patch_t * inode_create = NULL;
	int r;
	
	if(!head)
		return -EINVAL;
	
	pfile = (ext2_fdesc_t *) ext2_lookup_inode(object, parent);
	if(!pfile)
		return -EINVAL;
	
	if(pfile->f_type != TYPE_DIR)
	{
		r = -ENOTDIR;
		goto remove_name_exit;
	}	
	
	r = ext2_mdir_get(object, pfile, &mdir);
	if(r < 0)
		goto remove_name_exit;
	mdirent = ext2_mdirent_get(mdir, name);
	if(!mdirent)
		goto remove_name_exit;
	if(mdirent->minode)
		inode_create = WEAK(mdirent->minode->create);
	file = (ext2_fdesc_t *) ext2_lookup_inode(object, mdirent->dirent.inode);
	if(!file)
		goto remove_name_exit;
	
	if(file->f_type == TYPE_DIR)
	{
		if(file->f_ip->i_links_count > 2 && !strcmp(name, ".."))
		{
			r = -ENOTEMPTY;
			goto remove_name_exit;
		}
		else if(file->f_ip->i_links_count < 2)
		{
			Dprintf("%s warning, directory with %d links\n", __FUNCTION__, file->f_ip->i_links_count);
			minlinks = file->f_ip->i_links_count;
		}
		else
			minlinks = 2;
	}
	
	r = ext2_delete_dirent(object, pfile, mdir, mdirent, head);
	if(r < 0)
		goto remove_name_exit;
	assert(file->f_ip->i_links_count >= minlinks);
	
	/* remove link to parent directory */
	if(file->f_type == TYPE_DIR)
	{
		DECL_INODE_MOD(pfile);
		INODE_ADD(pfile, i_links_count, -1);
		prev_head = *head;
		r = ext2_write_inode(info, pfile, &prev_head, ioff1, ioff2);
		if(r < 0)
			goto remove_name_exit;
		lfs_add_fork_head(prev_head);
	}
	
	if(file->f_ip->i_links_count == minlinks)
	{
		/* need to free the inode */
		uint32_t number, nblocks, j, group;
		EXT2_inode_t inode = *file->f_ip;
		
		group = (file->f_ino - 1) / info->super->s_inodes_per_group;
		nblocks = ext2_get_file_numblocks(object, (fdesc_t *) file);
		
		if(file->f_type == TYPE_DIR)
			ext2_mdir_remove(object, file->f_ino);
		
		INODE_CLEAR(file);
		prev_head = *head;
		r = ext2_write_inode(info, file, &prev_head, 0, sizeof(EXT2_inode_t));
		if(r < 0)
			goto remove_name_exit;
		
		if(prev_head == inode_create)
		{
			// Create and delete merged so the inode will never exist on disk.
			// Therefore the bitmap and block pointer erases need not depend
			// on the inode's deletion (which could otherwise require many
			// disk writes to enforce SU).
			lfs_add_fork_head(prev_head);
#if DELETE_MERGE_STATS
			info->delete_inode_stats.merged++;
#endif
		}
		else
		{
			*head = prev_head;
		}
#if DELETE_MERGE_STATS
		if(inode_create && !(inode_create->flags & PATCH_INFLIGHT))
			info->delete_inode_stats.uncommitted++;
		info->delete_inode_stats.total++;
#endif
		
		prev_head = *head;
		r = ext2_write_inode_bitmap(object, file->f_ino, 0, &prev_head);
		if(r < 0)
			goto remove_name_exit;
		lfs_add_fork_head(prev_head);
		
		file->f_xinode = inode; // XXX slow
		int ioff1 = sizeof(EXT2_inode_t), ioff2 = 0; // XXX lame
		for(j = 0; j < nblocks; j++)
		{
			prev_head = *head;
			number = ext2_erase_block_ptr(object, file, &prev_head, &ioff1, &ioff2);
			if(number == INVALID_BLOCK)
			{
				r = -EINVAL;
				goto remove_name_exit;
			}
			lfs_add_fork_head(prev_head);
			
			prev_head = *head;
			r = _ext2_free_block(object, number, &prev_head);
			if(r < 0)
				goto remove_name_exit;
			lfs_add_fork_head(prev_head);
		}
		memset(&file->f_xinode, 0, sizeof(EXT2_inode_t)); // XXX slow
		if(file->f_type == TYPE_DIR)
		{
			r = ext2_super_report(object, group, 0, 0, -1);
			if(r < 0)
				goto remove_name_exit;				
		}
	}
	else
	{
		DECL_INODE_MOD(file);
		INODE_ADD(file, i_links_count, -1);
		r = ext2_write_inode(info, file, head, ioff1, ioff2);
		if(r < 0)
			goto remove_name_exit;
	}
	
	r = 0;

  remove_name_exit:
	ext2_free_fdesc(object, (fdesc_t *) pfile);
	ext2_free_fdesc(object, (fdesc_t *) file);
	return r;
}

static int ext2_write_block(LFS_t * object, bdesc_t * block, uint32_t number, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_write_block\n");
	struct ext2_info * info = (struct ext2_info *) object;
	assert(head);
	
	return CALL(info->ubd, write_block, block, number);
}

static patch_t ** ext2_get_write_head(LFS_t * object)
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

static const bool ext2_features[] = {[FSTITCH_FEATURE_SIZE] = 1, [FSTITCH_FEATURE_FILETYPE] = 1, [FSTITCH_FEATURE_FREESPACE] = 1, [FSTITCH_FEATURE_FILE_LFS] = 1, [FSTITCH_FEATURE_BLOCKSIZE] = 1, [FSTITCH_FEATURE_DEVSIZE] = 1, [FSTITCH_FEATURE_MTIME] = 1, [FSTITCH_FEATURE_ATIME] = 1, [FSTITCH_FEATURE_GID] = 1, [FSTITCH_FEATURE_UID] = 1, [FSTITCH_FEATURE_UNIX_PERM] = 1, [FSTITCH_FEATURE_NLINKS] = 1, [FSTITCH_FEATURE_SYMLINK] = 1, [FSTITCH_FEATURE_DELETE] = 1};

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
	if(id == FSTITCH_FEATURE_SIZE)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_ip->i_size;
	}
	else if(id == FSTITCH_FEATURE_FILETYPE)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_type;
	}
	else if(id == FSTITCH_FEATURE_FREESPACE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = count_free_space(object);
	}
	else if(id == FSTITCH_FEATURE_FILE_LFS)
	{
		if(size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);
		
		*((typeof(object) *) data) = object;
	}
	else if(id == FSTITCH_FEATURE_BLOCKSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = object->blocksize;
	}
	else if(id == FSTITCH_FEATURE_DEVSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = info->super->s_blocks_count;
	}
	else if(id == FSTITCH_FEATURE_NLINKS)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = (uint32_t) f->f_ip->i_links_count;
	}
	else if(id == FSTITCH_FEATURE_UID)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_ip->i_uid;
	}
	else if(id == FSTITCH_FEATURE_GID)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_ip->i_gid;
	}
	else if(id == FSTITCH_FEATURE_UNIX_PERM)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);
		
		*((uint16_t *) data) = f->f_ip->i_mode & ~EXT2_S_IFMT;
	}
	else if(id == FSTITCH_FEATURE_MTIME)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_ip->i_mtime;
	}
	else if(id == FSTITCH_FEATURE_ATIME)
	{
		if(!f)
			return -EINVAL;
		
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		
		*((uint32_t *) data) = f->f_ip->i_atime;
	}
	else if(id == FSTITCH_FEATURE_SYMLINK)
	{
		struct ext2_info * info = (struct ext2_info *) object;
		if(!f || f->f_type != TYPE_SYMLINK)
			return -EINVAL;
		
		// f->f_ip->i_size includes the zero byte!
		if(size < f->f_ip->i_size)
			return -ENOMEM;
		size = f->f_ip->i_size;
		
		// size of the block pointer array in bytes:
		if(size <= EXT2_N_BLOCKS * sizeof(uint32_t))
			memcpy(data, (char *) f->f_ip->i_block, size);
		else
		{
			bdesc_t * symlink_block;
			symlink_block = CALL(info->ubd, read_block, f->f_ip->i_block[0], 1, NULL);
			if(!symlink_block)
				return -1;
			memcpy(data, bdesc_data(symlink_block), f->f_ip->i_size);
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
	if(f)
		ext2_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ext2_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	const ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	return ext2_get_metadata(object, f, id, size, data);
}

static int ext2_set_metadata2(LFS_t * object, ext2_fdesc_t * f, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_set_metadata %u\n", f->f_ino);
	struct ext2_info * info = (struct ext2_info *) object;
	
	assert(head && f && (!nfsm || fsm));
	DECL_INODE_MOD(f);
	
 retry:
	if(!nfsm)
		return ext2_write_inode(info, f, head, ioff1, ioff2);
	
	//assert(fsm->fsm_feature < 100);
	//metadatas[fsm->fsm_feature]++;
	
	if(fsm->fsm_feature == FSTITCH_FEATURE_SIZE)
	{
		if(fsm->fsm_value.u >= EXT2_MAX_FILE_SIZE)
			return -EINVAL;
		INODE_SET(f, i_size, fsm->fsm_value.u);
	}
	else if(fsm->fsm_feature == FSTITCH_FEATURE_FILETYPE)
	{
		uint32_t fs_type;
		switch(fsm->fsm_value.u)
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
		
		INODE_SET(f, i_mode, (f->f_ip->i_mode & ~EXT2_S_IFMT) | (fs_type));
		f->f_type = fsm->fsm_value.u;
	}
	else if(fsm->fsm_feature == FSTITCH_FEATURE_UID)
		INODE_SET(f, i_uid, fsm->fsm_value.u);
	else if(fsm->fsm_feature == FSTITCH_FEATURE_GID)
		INODE_SET(f, i_gid, fsm->fsm_value.u);
	else if(fsm->fsm_feature == FSTITCH_FEATURE_UNIX_PERM)
		INODE_SET(f, i_mode, (f->f_ip->i_mode & EXT2_S_IFMT) | (fsm->fsm_value.u & ~EXT2_S_IFMT));
	else if(fsm->fsm_feature == FSTITCH_FEATURE_MTIME)
		INODE_SET(f, i_mtime, fsm->fsm_value.u);
	else if(fsm->fsm_feature == FSTITCH_FEATURE_ATIME)
		INODE_SET(f, i_atime, fsm->fsm_value.u);
	else if(fsm->fsm_feature == FSTITCH_FEATURE_SYMLINK)
	{
		if(f->f_type != TYPE_SYMLINK)
			return -EINVAL;
		
		int r = ext2_set_symlink(object, f, fsm->fsm_value.p.data, fsm->fsm_value.p.length, head, &ioff1, &ioff2);
		if(r < 0)
			return r;
	}
	else
		return -EINVAL;
	
	fsm++;
	nfsm--;
	goto retry;
}

static int ext2_set_metadata2_inode(LFS_t * object, inode_t ino, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	int r;
	ext2_fdesc_t * f = (ext2_fdesc_t *) ext2_lookup_inode(object, ino);
	if(!f)
		return -EINVAL;
	r = ext2_set_metadata2(object, f, fsm, nfsm, head);
	ext2_free_fdesc(object, (fdesc_t *) f);
	return r;
}

static int ext2_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	return ext2_set_metadata2(object, f, fsm, nfsm, head);
}

static int ext2_destroy(LFS_t * lfs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	ext2_fdesc_t * f;
	int i, r;
	
#if DELETE_MERGE_STATS
	printf("ext2 delete dirent stats: merged %u/%u possible, %u total\n", info->delete_dirent_stats.merged, info->delete_dirent_stats.uncommitted, info->delete_dirent_stats.total);
	printf("ext2 delete inode stats: merged %u/%u possible, %u total\n", info->delete_inode_stats.merged, info->delete_inode_stats.uncommitted, info->delete_inode_stats.total);
#endif
	
	r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);	
	if(info->bitmap_cache != NULL)
		bdesc_release(&info->bitmap_cache);
	if(info->inode_cache != NULL)
		bdesc_release(&info->inode_cache);
	if(info->super_cache != NULL)
		bdesc_release(&info->super_cache);
	for(i = 0; i < info->ngroupblocks; i++)
		bdesc_release(&(info->gdescs[i]));
	for(f = info->filecache; f; f = f->f_cache_next)
		assert(f->f_nopen == 1 && f->f_age != 0);
	while(info->filecache)
		ext2_free_fdesc(lfs, (fdesc_t *) info->filecache);
	
	ext2_mdir_cache_deinit(&info->mdir_cache);
	ext2_minode_cache_deinit(&info->minode_cache);
	n_ext2_instances--;
	if(!n_ext2_instances)
	{
		ext2_minode_free_all();
		ext2_mdirent_free_all();
		ext2_fdesc_pool_free_all();
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

static int ext2_get_inode(ext2_info_t * info, ext2_fdesc_t * f, int copy)
{
	uint32_t block_group, offset, block;
	assert(f);
	assert(f->f_ino == EXT2_ROOT_INO || (f->f_ino >= info->super->s_first_ino && f->f_ino <= info->super->s_inodes_count));
	assert(!f->f_inode_cache);
	
	// Get the group the inode belongs in
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	offset = ((f->f_ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (offset >> (10 + info->super->s_log_block_size));
	
	f->f_inode_cache = CALL(info->ubd, read_block, block, 1, NULL);
	if(!f->f_inode_cache)
		return -EINVAL;
	bdesc_retain(f->f_inode_cache);
	
	if(copy)
	{
		offset &= info->lfs.blocksize - 1;
		// NOTE: the pointer into this bdesc will not become invalid
		// because inode blocks do not change
		f->f_ip = (const EXT2_inode_t *) (bdesc_data(f->f_inode_cache) + offset);
	}
	
	return f->f_ino;
}

static int ext2_write_inode_set(struct ext2_info * info, ext2_fdesc_t * f, patch_t ** tail, patch_pass_set_t * befores, int ioff1, int ioff2)
{
	uint32_t block_group, offset, block;
	int r;
	
	assert(tail);
	assert(f);
	assert(f->f_ino == EXT2_ROOT_INO || (f->f_ino >= info->super->s_first_ino && f->f_ino <= info->super->s_inodes_count));
	
	if(!f->f_inode_cache)
		if(ext2_get_inode(info, f, 0) < 0)
			return -1;
	
	// Get the group the inode belongs in
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	
	offset = ((f->f_ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (offset >> (10 + info->super->s_log_block_size));
	
	offset &= info->lfs.blocksize - 1;
	const EXT2_inode_t * old_inode = (EXT2_inode_t *) &bdesc_data(f->f_inode_cache)[offset]; (void) old_inode;
	if(!ioff1 && ioff2 == sizeof(EXT2_inode_t))
	{
		r = patch_create_diff_set(f->f_inode_cache, info->ubd, offset, sizeof(EXT2_inode_t), old_inode, f->f_ip, tail, befores);
		// patch_create_diff() returns 0 for "no change"
		if(r <= 0)
			return r;
		r = 0;
	}
	else if(ioff1 >= ioff2)
	{
#if 1
		assert(!memcmp(old_inode, f->f_ip, sizeof(EXT2_inode_t)));
#endif
		return 0;
	}
	else
	{
#if 1
		assert(!memcmp(old_inode, f->f_ip, ioff1)
		       && !memcmp((uint8_t *)old_inode + ioff2, (uint8_t *)f->f_ip + ioff2, sizeof(EXT2_inode_t) - ioff2));
#endif
		r = patch_create_byte_set(f->f_inode_cache, info->ubd, offset + ioff1, ioff2 - ioff1, (uint8_t *) f->f_ip + ioff1, tail, befores);
		if(r < 0)
			return r;
	}
	
	if(*tail)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *tail, "write inode");
		lfs_add_fork_head(*tail); // TODO: why do this?
		r = CALL(info->ubd, write_block, f->f_inode_cache, block);
	}
	
	return r;
}

static int ext2_super_report(LFS_t * lfs, uint32_t group, int32_t blocks, int32_t inodes, int32_t dirs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	int r = 0;
	patch_t * head = info->write_head ? *info->write_head : NULL;
	
	// Deal with the super block
	if(blocks || inodes)
	{
		EXT2_Super_t * super = (EXT2_Super_t *) info->super;
		super->s_free_blocks_count += blocks;
		super->s_free_inodes_count += inodes;
		
		int off1 = (blocks ? offsetof(EXT2_Super_t, s_free_blocks_count) : offsetof(EXT2_Super_t, s_free_inodes_count));
		int off2 = (inodes ? offsetof(EXT2_Super_t, s_free_inodes_count) + sizeof(super->s_free_inodes_count) : offsetof(EXT2_Super_t, s_free_blocks_count) + sizeof(super->s_free_blocks_count));
		
		r = patch_create_byte(info->super_cache, info->ubd,
				       off1 + 1024, off2 - off1,
				       ((const uint8_t *) super) + off1, &head);
		if(r >= 0 && head)
		{
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "write superblock");
			lfs_add_fork_head(head);
			r = CALL(info->ubd, write_block, info->super_cache, SUPER_BLOCKNO);
		}
	}
	
	if(r >= 0 && (blocks || inodes || dirs))
	{
		// Deal with the group descriptors
		EXT2_group_desc_t * gd = (EXT2_group_desc_t *) &info->groups[group];
		gd->bg_free_blocks_count += blocks;
		gd->bg_free_inodes_count += inodes;
		gd->bg_used_dirs_count += dirs;
	
		head = info->write_head ? *info->write_head : NULL;
	
		int group_bdesc = group / info->block_descs;
		int group_offset = group % info->block_descs;
		group_offset *= sizeof(EXT2_group_desc_t);
		
		int off1 = offsetof(EXT2_group_desc_t, bg_free_blocks_count);
		int off2 = offsetof(EXT2_group_desc_t, bg_used_dirs_count) + sizeof(gd->bg_used_dirs_count);
		
		r = patch_create_byte(info->gdescs[group_bdesc], info->ubd,
				       group_offset + off1, off2 - off1,
				       ((const uint8_t *) gd) + off1, &head);
		if(r >= 0 && head)
		{
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, head, "write group desc");
			lfs_add_fork_head(head);
			r = CALL(info->ubd, write_block, info->gdescs[group_bdesc], GDESC_BLOCKNO(group_bdesc));
		}
	}
	
	return r;
}

static int ext2_load_super(LFS_t * lfs)
{
	struct ext2_info * info = (struct ext2_info *) lfs;
	
	// initialize the pointers
	info->bitmap_cache = NULL;
	info->inode_cache = NULL;
	info->groups = NULL;
	info->gnum = INVALID_BLOCK;
	info->inode_gdesc = INVALID_BLOCK;
#if DELETE_MERGE_STATS
	info->delete_dirent_stats.merged = 0;
	info->delete_dirent_stats.uncommitted = 0;
	info->delete_dirent_stats.total = 0;
	info->delete_inode_stats.merged = 0;
	info->delete_inode_stats.uncommitted = 0;
	info->delete_inode_stats.total = 0;
#endif
#if ROUND_ROBIN_ALLOC
	info->last_fblock = 0;
	info->last_iblock = 0;
	info->last_dblock = 0;
#endif
	
	info->super_cache = CALL(info->ubd, read_block, SUPER_BLOCKNO, 1, NULL);
	if(info->super_cache == NULL)
	{
		printf("Unable to read superblock!\n");
		return 0;
	}
	bdesc_retain(info->super_cache);
	info->super = malloc(sizeof(struct EXT2_Super));
	memcpy((EXT2_Super_t *) info->super, bdesc_data(info->super_cache) + 1024, sizeof(EXT2_Super_t));
	
#if ROUND_ROBIN_ALLOC
	/* start file data at the beginning, indirect blocks halfway through,
	 * and directory data one quarter from the end of the file system */
	info->last_fblock = 0;
	info->last_iblock = info->super->s_blocks_count / 2;
	info->last_dblock = 3 * (info->super->s_blocks_count / 4);
#endif
	
	// now load the gdescs
	uint32_t i;
	uint32_t ngroupblocks;
	lfs->blocksize = 1024 << info->super->s_log_block_size;
	info->block_descs = lfs->blocksize / sizeof(EXT2_group_desc_t);
	int ngroups = (info->super->s_blocks_count / info->super->s_blocks_per_group);
	if(info->super->s_blocks_count % info->super->s_blocks_per_group)
		ngroups++;
	info->ngroups = ngroups;
	info->groups = calloc(ngroups, sizeof(EXT2_group_desc_t));
	if(!info->groups)
		goto wb_fail1;
	
	ngroupblocks = ngroups / info->block_descs;
	if(ngroups % info->block_descs)
		ngroupblocks++;
	
	info->gdescs = malloc(ngroupblocks * sizeof(bdesc_t *));
	int nbytes = 0;
	for(i = 0; i < ngroupblocks; i++)
	{
		info->gdescs[i] = CALL(info->ubd, read_block, GDESC_BLOCKNO(i), 1, NULL);
		if(!info->gdescs[i])
			goto wb_fail2;
		
		if((sizeof(EXT2_group_desc_t) * ngroups) < (lfs->blocksize * (i + 1)))
			nbytes = (sizeof(EXT2_group_desc_t) * ngroups) % lfs->blocksize;
		else
			nbytes = lfs->blocksize;
		
		if(!memcpy((EXT2_group_desc_t *) info->groups + (i * info->block_descs),
		           bdesc_data(info->gdescs[i]), nbytes))
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

LFS_t * ext2_lfs(BD_t * block_device)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info;
	LFS_t * lfs;
	int r;
	
	if(!block_device)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	
	lfs = &info->lfs;
	LFS_INIT(lfs, ext2);
	OBJMAGIC(lfs) = EXT2_FS_MAGIC;
	
	info->ubd = lfs->blockdev = block_device;
	info->write_head = CALL(block_device, get_write_head);
	
	info->filecache = NULL;
	
	r = ext2_minode_cache_init(&info->minode_cache);
	if(r < 0)
		goto error_info;
	
	r = ext2_mdir_cache_init(&info->mdir_cache, &info->minode_cache);
	if(r < 0)
		goto error_minode;
	
	if(!ext2_load_super(lfs))
		goto error_mdir;
	
	if(check_super(lfs))
		goto error_mdir;
	
	n_ext2_instances++;
	
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
	
  error_mdir:
	ext2_mdir_cache_deinit(&info->mdir_cache);
  error_minode:
	ext2_minode_cache_deinit(&info->minode_cache);
  error_info:
	free(info);
	return NULL;
}
