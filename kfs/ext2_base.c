#include <lib/platform.h>
#include <lib/hash_set.h>
#include <lib/jiffies.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/ext2_super_wb.h>
#include <kfs/ext2_base.h>
#include <kfs/feature.h>

#define EXT2_BASE_DEBUG 0

#if EXT2_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number);
static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head);
static int ext2_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t get_file_block(LFS_t * object, EXT2_File_t * file, uint32_t offset);
static uint32_t ext2_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head);
static int ext2_set_metadata(LFS_t * object, ext2_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head);

static int ext2_get_inode(ext2_info_t * info, inode_t ino, EXT2_inode_t * inode);
static uint8_t ext2_to_kfs_type(uint16_t type);
static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, uint32_t basep, uint32_t prev_basep, chdesc_t ** head);

static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent);
static int read_block_bitmap(LFS_t * object, uint32_t blockno);
int ext2_write_inode(struct ext2_info * info, inode_t ino, EXT2_inode_t inode, chdesc_t ** head);
static inode_t ext2_find_free_inode(LFS_t * object, inode_t parent);
static int empty_get_metadata(void * arg, uint32_t id, size_t size, void * data);

static uint32_t EXT2_BLOCK_SIZE;
static uint32_t EXT2_DESC_PER_BLOCK;

// Equivalent to JOS's read_super
static int check_super(LFS_t * object)
{
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
		
	/* the superblock is in block 1 */
	printf("\tMagic Number 0x%x \n", info->super->s_magic);
	printf("\tBlocksize might be %i\n", CALL(info->ubd, get_blocksize));
	printf("\tNumber of inodes %i\n", info->super->s_inodes_count);
	printf("\tSize of inode sturcture %i\n", info->super->s_inode_size);
	printf("\tNumber of free inodes %i\n", info->super->s_free_inodes_count);
	printf("\tNumber of blocks %i\n", info->super->s_blocks_count);
	printf("\tEXT2 Block size %i\n", 1024 << info->super->s_log_block_size);
	printf("\tNumber of free blocks %i\n", info->super->s_free_blocks_count);
	printf("\tSize of block group is %i\n", sizeof(EXT2_group_desc_t));
	printf("\tNumber of blocks per group %i\n", info->super->s_blocks_per_group);
	printf("\tNumber of inodes per group %i\n", info->super->s_inodes_per_group);

	if (info->super->s_magic != EXT2_FS_MAGIC) {
		printf("ext2_base: bad file system magic number\n");
		return -1;
	}
	
	EXT2_BLOCK_SIZE = (1024 << info->super->s_log_block_size);
	EXT2_DESC_PER_BLOCK = EXT2_BLOCK_SIZE / sizeof(EXT2_group_desc_t);
	
	return 0;
}

#ifdef __i386__
//Stolen from Linux kernel include/asm-i386/bitops.h 2.6.20
static inline int find_zero_bit( const unsigned long *addr, unsigned size )
{
	int d0, d1, d2;
	int res;

	if (!size)
		return 0;
	// This looks at memory. Mark it volatile to tell gcc not to move it around
	__asm__ __volatile__(
			"movl $-1,%%eax\n\t"
			"xorl %%edx,%%edx\n\t"
			"repe; scasl\n\t"
			"je 1f\n\t"
			"xorl -4(%%edi),%%eax\n\t"
			"subl $4,%%edi\n\t"
			"bsfl %%eax,%%edx\n"
			"1:\tsubl %%ebx,%%edi\n\t"
			"shll $3,%%edi\n\t"
			"addl %%edi,%%edx"
			:"=d" (res), "=&c" (d0), "=&D" (d1), "=&a" (d2)
			:"1" ((size + 31) >> 5), "2" (addr), "b" (addr) : "memory");
	return res;
}
#else
static inline int find_zero_bit( const unsigned long *addr, unsigned size )
{
#error implement find_zero_bit in C
}
#endif

static int ext2_find_free_block(LFS_t * object, uint32_t * blockno)
{
	Dprintf("EXT2DEBUG: %s blockno is %u\n", __FUNCTION__, *blockno);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	bdesc_t * bitmap;
	uint32_t block_group;
	
	if (*blockno < info->super->s_first_data_block) 
	{
		printf("%s requested status of block %u too small block no!\n",__FUNCTION__, *blockno);
		return -EINVAL;
	}
	if (*blockno >= info->super->s_blocks_count) 
	{
		printf("%s requested status of block %u too large block no!\n",__FUNCTION__, *blockno);
		return -EINVAL;
	}
	uint32_t curr = *blockno;
	short firstrun = 1;
	block_group = curr / info->super->s_blocks_per_group;
	while(block_group != ( (*blockno) / info->super->s_blocks_per_group) || firstrun) {
		//Read in the block bitmap for this group
		if(info->gnum != block_group || info->bitmap_cache == NULL) {
			if (info->bitmap_cache != NULL)
				bdesc_release(&info->bitmap_cache);	
			info->gnum = block_group;
			bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
			if (!bitmap)
				return -ENOENT;
			bdesc_retain(bitmap);
			info->bitmap_cache = bitmap;
		}
		
		const unsigned long * foo = (const unsigned long *)info->bitmap_cache->ddesc->data;
		int bar = 0;
		bar = find_zero_bit(foo, info->super->s_blocks_per_group );
		if (bar < (info->super->s_blocks_per_group)) {
			curr += bar; 
			*blockno = curr;
			return EXT2_FREE;
	       	}
		
		firstrun = 0;
		block_group = (block_group + 1) % info->ngroups;
		curr = block_group * info->super->s_blocks_per_group;	
	}
		
	return -ENOSPC;
}

static int read_block_bitmap(LFS_t * object, uint32_t blockno)
{
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	bdesc_t * bitmap;
	uint32_t * ptr;
	uint32_t block_group, block_in_group;
		
	if (blockno < info->super->s_first_data_block) 
	{
		printf("ext2: %s requested status of block %u too small block no!\n",__FUNCTION__, blockno);
		return -EINVAL;
	}
	if (blockno >= info->super->s_blocks_count) 
	{
		printf("ext2: %s requested status of block %u too large block no!\n",__FUNCTION__, blockno);
		return -EINVAL;
	}

	block_group = blockno / info->super->s_blocks_per_group;
	if(info->gnum != block_group || info->bitmap_cache == NULL) {
		if (info->bitmap_cache != NULL)
			bdesc_release(&info->bitmap_cache);	
		info->gnum = block_group;
		bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
		if (!bitmap)
			return -ENOENT;
		bdesc_retain(bitmap);
		info->bitmap_cache = bitmap;
	}

	block_in_group  = blockno % info->super->s_blocks_per_group;
	ptr = ((uint32_t *) info->bitmap_cache->ddesc->data) + (block_in_group/32);
	if (*ptr & (1 << (block_in_group % 32)))
		return EXT2_USED;
	return EXT2_FREE;
}

static int write_block_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: write_bitmap %u\n", blockno);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);

	bdesc_t * bitmap;
	int r;

	if (!head)
		return -1;

	if (blockno == 0 || blockno == INVALID_BLOCK) 
	{
		printf("ext2_base: attempted to write status of zero block!\n");
		return -EINVAL;
	}
	else if (blockno >= info->super->s_blocks_count) 
	{
		printf("ext2_base: requested status of block %u too large block no!\n", blockno);
		return -EINVAL;
	}
	
	uint32_t block_group = blockno / info->super->s_blocks_per_group;
	if(info->gnum != block_group || info->bitmap_cache == NULL) {
		if (info->bitmap_cache != NULL)
			bdesc_release(&info->bitmap_cache);	
		info->gnum = block_group;
		bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_block_bitmap, 1);
		if (!bitmap)
		{
			Dprintf("unable to read block bitmap in %s\n", __FUNCTION__);
			return -ENOENT;
		}
		bdesc_retain(bitmap);
		info->bitmap_cache = bitmap;
	}

	uint32_t block_in_group  = blockno % info->super->s_blocks_per_group;	

	// does it already have the right value? 
	if (((uint32_t *) info->bitmap_cache->ddesc->data)[(block_in_group) / 32] & (1 << (block_in_group % 32)))
	{
	       if (value)
		      return 0;
	}
	else if (!value)
	       return 0;

	// bit chdescs take offset in increments of 32 bits 
	r = chdesc_create_bit(info->bitmap_cache, info->ubd, (block_in_group) / 32, 1 << (block_in_group % 32), head);
	if (r < 0)
		return r;	

	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, value ? "allocate block" : "free block");
	r = CALL(info->ubd, write_block, info->bitmap_cache);
	if (r < 0)
		return r;

	r = CALL(info->super_wb, blocks, value ? -1 : 1);
	if (r < 0)
		return r;

	r = CALL(info->super_wb, write_gdesc, block_group, value ? -1 : 1, 0, 0);
	return r;
}

static int write_inode_bitmap(LFS_t * object, inode_t inode_no, bool value, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: write_inode_bitmap %u\n", inode_no);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);

	bdesc_t * bitmap;
	int r;

	if (!head)
		return -1;

	//check to make sure we're not writing too soon...

	if (inode_no >= info->super->s_inodes_count) 
	{
		printf("ext2_base:read_inode: inode %u past end of file system!\n", inode_no);
		return -1;
	}

	uint32_t block_group = (inode_no - 1)  / info->super->s_inodes_per_group;
	if(info->inode_gdesc != block_group || info->inode_cache == NULL) {
		if (info->inode_cache != NULL)
			bdesc_release(&info->inode_cache);	
		info->inode_gdesc = block_group;
		bitmap = CALL(info->ubd, read_block, info->groups[block_group].bg_inode_bitmap, 1);
		if (!bitmap)
			return -ENOENT;
		bdesc_retain(bitmap);
		info->inode_cache = bitmap;
	}

	uint32_t inode_in_group = (inode_no - 1)  % info->super->s_inodes_per_group;

	// does it already have the right value? 
	if (((uint32_t *) info->inode_cache->ddesc->data)[(inode_in_group) / 32] & (1 << (inode_in_group % 32)))
	{
		if (value)
		       return 0;
	} else { 
		if (!value)
		       return 0;
	}
	
	// bit chdescs take offset in increments of 32 bits 
	r = chdesc_create_bit(info->inode_cache, info->ubd, (inode_in_group) / 32, 1 << (inode_in_group % 32), head);
	if (r < 0)
		return r;	

	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, value ? "allocate inode" : "free inode");
	r = CALL(info->ubd, write_block, info->inode_cache);
	if (r < 0)
		return r;
		
	r = CALL(info->super_wb, inodes, value ? -1 : 1);
	if (r < 0)
		return r;

	r = CALL(info->super_wb, write_gdesc, block_group, 0, value ? -1 : 1, 0);
	return r;
}

static uint32_t count_free_space(LFS_t * object)
{
	//FIXME is this in bytes or blocks???
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	return info->super->s_free_blocks_count;
}

static int ext2_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != EXT2_FS_MAGIC)
		return -EINVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ext2_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != EXT2_FS_MAGIC)
		return -EINVAL;
	
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ext2_get_root(LFS_t * object, inode_t * ino)
{
	*ino = EXT2_ROOT_INO;
	return 0;
}

static uint32_t ext2_get_blocksize(LFS_t * object)
{
	return EXT2_BLOCK_SIZE;
}

static BD_t * ext2_get_blockdev(LFS_t * object)
{
	return ((struct ext2_info *) OBJLOCAL(object))->ubd;
}

// purpose parameter is ignored
// FIXME currently the superblock and group descriptor structures are not adjusted
static uint32_t ext2_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t lastblock = 0;; 
	uint32_t blockno, block_group;
	int r;

	if (!head || !f)
		return INVALID_BLOCK;
	
	if(f->f_inode.i_size == 0)
		goto inode_search;

	//Get the block number of the last block of the inode
	//FIXME this offset might be off
	if (f->f_lastblock != 0)
		blockno = f->f_lastblock;
	else
		blockno = get_file_block(object, (EXT2_File_t *) f, (f->f_inode.i_size) - 1);	
	if(blockno == INVALID_BLOCK)
		return INVALID_BLOCK;
	lastblock = blockno;
	//FIXME this could affect performance
	//Look in the 32 block(disk blocks not street blocks) vicinity of the lastblock
	//there is no check to make sure that these blocks are all in the same block group
	while(blockno - lastblock < 32) {
		blockno++;
		if(read_block_bitmap(object, blockno) == EXT2_FREE)
			goto claim_block;
	}

inode_search:	
	//Look for free blocks in same block group as the inode
	block_group = (f->f_ino - 1) / info->super->s_inodes_per_group;
	blockno = block_group * info->super->s_blocks_per_group;
	//FIXME this should be slightly smarter
	while(blockno < info->super->s_blocks_count) {
		r = ext2_find_free_block(object, &blockno);
		if (r < 0)
			break;
		if (r == EXT2_FREE)
			goto claim_block;
		blockno++;
	}

	return INVALID_BLOCK;

claim_block:
	if(write_block_bitmap(object, blockno, 1, head) < 0) {
		write_block_bitmap(object, blockno, 0, head);
		return INVALID_BLOCK;
	}
	f->f_lastblock = blockno;
	return blockno;
}

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	return CALL(info->ubd, read_block, number, 1);
}

static bdesc_t * ext2_synthetic_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_synthetic_lookup_block %u\n", number);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	return CALL(info->ubd, synthetic_read_block, number, 1);
}

static fdesc_t * ext2_lookup_inode(LFS_t * object, inode_t ino)
{
	ext2_fdesc_t * fd = NULL;
	uint32_t r;
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	
	if(ino <= 0)
		return NULL;

	fd = hash_map_find_val(info->filemap, (void *) ino);

	if (fd) {
		fd->f_nopen++;
		return (fdesc_t *)fd;
	}

	fd = (ext2_fdesc_t *)malloc(sizeof(ext2_fdesc_t));
	if (!fd)
		goto ext2_lookup_inode_exit;
	
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_ino = ino;
	fd->f_nopen = 1;
	fd->f_lastblock = 0;

	r = ext2_get_inode(info, ino, &(fd->f_inode));
	if(r < 0)
		goto ext2_lookup_inode_exit;
		
	fd->f_type = ext2_to_kfs_type(fd->f_inode.i_mode);

	r = hash_map_insert(info->filemap, (void *) ino, fd);
	if(r < 0)
		goto ext2_lookup_inode_exit;
	assert(r == 0);

	return (fdesc_t*)fd;

 ext2_lookup_inode_exit:
	free(fd);
	return NULL;
}

static void ext2_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("EXT2DEBUG: ext2_free_fdesc %p\n", fdesc);
	ext2_fdesc_t * f = (ext2_fdesc_t *) fdesc;
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	
	if (f) {
		if(f->f_nopen > 1) {
			f->f_nopen--;
			return;
		}
		hash_map_erase(info->filemap, (void *) f->f_ino);
		free(f);
	}
}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, ext2_fdesc_t * f, const char* name, uint32_t * basep, 
		uint32_t * pbasep, uint32_t * ppbasep, ext2_fdesc_t** file)
{
	Dprintf("EXT2DEBUG: dir_lookup %s\n", name);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t name_length = strlen(name);
	bdesc_t * dirblock1 = NULL, *dirblock2 = NULL;
	uint32_t blockno, file_blockno1, file_blockno2, num_file_blocks, block_offset;
	EXT2_Dir_entry_t entry;

	num_file_blocks = f->f_inode.i_blocks / (EXT2_BLOCK_SIZE / 512); 
	
	while (1) {
		blockno = *basep / EXT2_BLOCK_SIZE;
		file_blockno1 = get_file_block(object, f, *basep);

		if (file_blockno1 == INVALID_BLOCK)
			goto dir_lookup_error;
		
		dirblock1 = CALL(info->ubd, read_block, file_blockno1, 1);
		if (!dirblock1)
			goto dir_lookup_error;

		while ( (*basep / EXT2_BLOCK_SIZE) == blockno ) {
			*ppbasep = *pbasep;
			*pbasep = *basep;

			if (*basep >= f->f_inode.i_size)
				goto dir_lookup_error;

			block_offset = (*basep % EXT2_BLOCK_SIZE);

			/*check if the rec_len is available yet*/
			uint16_t rec_len;
			if (EXT2_BLOCK_SIZE - block_offset >= 6)
			{
				rec_len = *((uint16_t *) (dirblock1->ddesc->data + block_offset + 4));
				if (*basep + rec_len > f->f_inode.i_size)
					goto dir_lookup_error;
			}
			else
				rec_len = 0;

			/*if the dirent overlaps two blocks*/
			if(rec_len == 0 || block_offset + rec_len > EXT2_BLOCK_SIZE)
			{
				if (blockno + 1 >= f->f_inode.i_blocks)
					return -1;

				file_blockno2 = get_file_block(object, f, (*basep)+ sizeof(EXT2_Dir_entry_t));
				if (file_blockno2 == INVALID_BLOCK)
					return -1;

				dirblock2 = CALL(info->ubd, read_block, file_blockno2, 1);
				if(!dirblock2)
					return -1;  // should be: -ENOENT;
				//TODO: Clean this up for the weird case of large rec_lens due to lots of deletes 
				uint32_t block1_len;
				block1_len = EXT2_BLOCK_SIZE - block_offset;
				uint32_t block2_len;
				block2_len = (sizeof(EXT2_Dir_entry_t) - block1_len);
				if(block1_len > sizeof(EXT2_Dir_entry_t))
					block2_len = 0;
				if(block1_len > sizeof(EXT2_Dir_entry_t))
					block1_len = sizeof(EXT2_Dir_entry_t);

				/* copy each part from each block into the dir entry */
				memcpy(&entry, dirblock1->ddesc->data + block_offset, block1_len);
				memcpy((uint8_t*)(&entry) + block1_len, dirblock2->ddesc->data, block2_len);
				*basep += entry.rec_len;
				if (entry.name_len == name_length && strncmp(entry.name, name, entry.name_len) == 0) {
					*file = (ext2_fdesc_t *)ext2_lookup_inode(object, entry.inode);
					if(!(*file)) {
						goto dir_lookup_error;
					}
					return 0;
				}
			}
			else
			{
				EXT2_Dir_entry_t * pdirent =  (EXT2_Dir_entry_t *) (dirblock1->ddesc->data + block_offset);
				*basep += pdirent->rec_len;
				if (pdirent->name_len == name_length && strncmp(pdirent->name, name, pdirent->name_len) == 0) {
					*file = (ext2_fdesc_t *)ext2_lookup_inode(object, pdirent->inode);
					if(!(*file))
						goto dir_lookup_error;
					return 0;
				}
			}
		}

	}
	

dir_lookup_error:
	Dprintf("EXT2DEBUG: dir_lookup done: NOT FOUND\n");
	return -ENOENT;
}

static int ext2_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("EXT2DEBUG: ext2_lookup_name %s\n", name);
	ext2_fdesc_t * fd;
	ext2_fdesc_t * parent_file;
	uint32_t basep = 0;
	const EXT2_Dir_entry_t * entry;
	uint32_t name_length = strlen(name);
	int r= 0;
	
	//TODO do some sanity checks on name

	// "." and ".." are (at least right now) supported by code further up
	// (this seems hacky, but it would be hard to figure out parent's parent from here)

	fd = (ext2_fdesc_t *)ext2_lookup_inode(object, parent);
	if (!fd)
		return -ENOENT;
	if (fd->f_type != TYPE_DIR)
		return -ENOTDIR;
	
	parent_file = fd;
	while (r >= 0)
	{
		r = ext2_get_disk_dirent(object, parent_file, &basep, &entry);
		if (!r && entry->inode && entry->name_len == name_length && !strncmp(entry->name, name, entry->name_len)) {
			fd = (ext2_fdesc_t *) ext2_lookup_inode(object, entry->inode);
			break;
		}
	}
	if(fd && ino)
		*ino = fd->f_ino;
	if(fd != parent_file)
		ext2_free_fdesc(object, (fdesc_t *) fd);
	ext2_free_fdesc(object, (fdesc_t *) parent_file);
	fd = NULL;
	parent_file = NULL;
	if (r < 0)
		return -ENOENT;
	return 0;
}

static uint32_t ext2_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	
	if(f->f_type == TYPE_SYMLINK)
		return 0;

	//i_blocks holds number of 512 byte blocks not EXT2_BLOCK_SIZE blocks
	//return f->f_inode.i_blocks / (EXT2_BLOCK_SIZE / 512);
	if (f->f_inode.i_size == 0)
		return 0;
	return ((f->f_inode.i_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE);
}

static uint32_t get_file_block(LFS_t * object, EXT2_File_t * file, uint32_t offset)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t blockno, pointers_per_block;
	bdesc_t * block_desc;
	uint32_t * inode_nums, blocknum;
	
	if (offset >= file->f_inode.i_size || file->f_type == TYPE_SYMLINK)
		return INVALID_BLOCK;

	pointers_per_block = EXT2_BLOCK_SIZE / (sizeof(uint32_t));

	//non block aligned offsets suck (aka aren't supported)
	blocknum = offset / EXT2_BLOCK_SIZE;

	//TODO: compress this code, but right now its much easier to understand...
	if (blocknum >= pointers_per_block * pointers_per_block + pointers_per_block + EXT2_NDIRECT)
	{
		// Lets not worry about tripley indirect for the momment
		return INVALID_BLOCK;
	}
	else if (blocknum >= pointers_per_block + EXT2_NDIRECT)
	{
		blocknum -= (EXT2_NDIRECT + pointers_per_block);
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_DINDIRECT], 1));
		if (!block_desc)
		{
		       Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
		       return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		blockno = inode_nums[blocknum / pointers_per_block];
		block_desc = CALL(info->ubd, read_block, blockno, 1);
		if (!block_desc)
		{
		       Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
		       return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		blocknum %= pointers_per_block;
		return inode_nums[blocknum];
	}	
	else if (blocknum >= EXT2_NDIRECT)
	{
	        blocknum -= EXT2_NDIRECT;
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_NINDIRECT], 1));
		if (!block_desc)
		{
		       Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
		       return INVALID_BLOCK;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		return inode_nums[blocknum];
	}
	else
	{
              return file->f_inode.i_block[blocknum];
	}
}

// Offset is a byte offset
static uint32_t ext2_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
       Dprintf("EXT2DEBUG: ext2_get_file_block %p, %u\n", file, offset);
	return get_file_block(object, (EXT2_File_t *)file, offset);
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
	//      return -1;

	entry->d_fileno = ino;
	//entry->d_filesize = inode.i_size;
	entry->d_reclen = reclen;
	entry->d_namelen = namelen;
	strncpy(entry->d_name, dirfile->name, namelen);
	entry->d_name[namelen] = 0;
  	
		
	Dprintf("EXT2DEBUG: %s, created  %s\n", __FUNCTION__, entry->d_name);
	return 0;
}

//TODO really, this shouldnt return inode == 0, since its annoying, but then to iterate to find free space its more work =(
static int ext2_get_disk_dirent(LFS_t * object, ext2_fdesc_t * file, uint32_t * basep, const EXT2_Dir_entry_t ** dirent)
{
  	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, file_blockno, num_file_blocks, block_offset;

	num_file_blocks = f->f_inode.i_blocks / (EXT2_BLOCK_SIZE / 512); 
	block_offset = (*basep % EXT2_BLOCK_SIZE);

	if (*basep >= f->f_inode.i_size)
		return -1; // should be: -ENOENT;

	blockno = *basep / EXT2_BLOCK_SIZE;
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
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
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

static int add_indirect(LFS_t * object, ext2_fdesc_t * f, uint32_t block, chdesc_t ** head) {
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t blockno, nindirect, offset, nblocks;
	bdesc_t * dindirect = NULL, * indirect = NULL;
	uint32_t * dindir = NULL;
	int r;

	nblocks = ((f->f_inode.i_blocks) / (EXT2_BLOCK_SIZE / 512)) + 1; //plus 1 to account for newly allocated block
	nindirect = EXT2_BLOCK_SIZE / sizeof(uint32_t);
		
	dindirect = ext2_lookup_block(object, f->f_inode.i_block[EXT2_DINDIRECT]);
	if(dindirect == NULL)
		return -ENOSPC;
	//get rid of the direct blocks, indirect blocks, indirect pointer, 
	//doubly indirect pointer & new allocated block
	nblocks -= (EXT2_NDIRECT + nindirect + 3);
	//get rid of the doubly-indirect indirect block pointers
	if (nblocks < nindirect)
		nblocks -= 1;
	else if((nblocks % nindirect) > (nblocks/nindirect) )
		nblocks -= ((nblocks / nindirect) + 1);
	else
		nblocks -= ((nblocks / nindirect));
	if(nblocks != 0 && (nblocks % (nindirect)) == 0) {
		chdesc_t * prev_head = NULL;
		//allocate an indirect pointer
		blockno = ext2_allocate_block(object, (fdesc_t *)f, 0, head);
		if(blockno == INVALID_BLOCK)
			return -ENOSPC;
		indirect = ext2_synthetic_lookup_block(object, blockno);
		if(indirect == NULL)
			return -ENOSPC;
		if((r = chdesc_create_init(indirect, info->ubd, head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init indirect block");
		dindir = (uint32_t *)indirect->ddesc->data;
		f->f_inode.i_blocks += EXT2_BLOCK_SIZE / 512;
		offset = (nblocks / (nindirect)) * sizeof(uint32_t);
		prev_head = *head;
		if ((r = chdesc_create_byte(dindirect, info->ubd, offset, sizeof(uint32_t), &blockno, &prev_head)) < 0)
			return r;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, prev_head, "add indirect block");
		r = lfs_add_fork_head(prev_head);
		assert(r >= 0);
		//add the block to the indirect pointer
		if ((r = chdesc_create_byte(indirect, info->ubd, 0, sizeof(uint32_t), &block, head)) < 0)
			return r;

		if ((r = CALL(info->ubd, write_block, indirect)) < 0)
			return r;

		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add block");
		return CALL(info->ubd, write_block, dindirect);
	} else {
		dindir = (uint32_t *)dindirect->ddesc->data;
		offset = (nblocks / (nindirect));
		indirect = ext2_lookup_block(object, dindir[offset]);
		dindir = (uint32_t *)indirect->ddesc->data;
		if(indirect == NULL) 
			return -ENOSPC;
		offset = (nblocks % (nindirect)) * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add block");
		return CALL(info->ubd, write_block, indirect);
	}
	return -EINVAL;
}       

static int ext2_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s %d\n", __FUNCTION__, block);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	uint32_t nblocks = ((f->f_inode.i_blocks) / (EXT2_BLOCK_SIZE / 512)) + 1; //plus 1 to account for newly allocated block
	bdesc_t * indirect = NULL, * dindirect = NULL;
	uint32_t blockno, nindirect;
	int r, offset, allocated;
	
	if (f->f_type == TYPE_SYMLINK)
		return -EINVAL;

	if (!head || !f || block == INVALID_BLOCK)
		return -EINVAL;
	
	nindirect = EXT2_BLOCK_SIZE / sizeof(uint32_t);
	allocated = 0;

	//FIXME as long as we only support doubly indirect blocks this
	//is the maximum number of blocks
	if (nblocks >= (EXT2_NDIRECT + ((nindirect + 1) * (nindirect + 1)) + 1))
			return -EINVAL;

	if (nblocks <= EXT2_NDIRECT) {
		blockno = nblocks;
		if(blockno != 0)
			blockno--;
		f->f_inode.i_block[blockno] = block;
	} 
	else if (nblocks > (EXT2_NDIRECT + nindirect + 1) ) {
		if(nblocks == (EXT2_NDIRECT+nindirect+2)) {
			chdesc_t * prev_head = *head;
			//allocate the doubly indirect block pointer & the first indirect block
			blockno = ext2_allocate_block(object, file, 0, &prev_head);
			if(blockno == INVALID_BLOCK)
				return -ENOSPC;
			dindirect = ext2_synthetic_lookup_block(object, blockno);
			if(dindirect == NULL) {
				(void)ext2_free_block(object, file, blockno, &prev_head);
				return -ENOSPC;
			}
			if((r = chdesc_create_init(dindirect, info->ubd, &prev_head)))
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, prev_head, "init double indirect block");
			f->f_inode.i_blocks += EXT2_BLOCK_SIZE / 512;
			f->f_inode.i_block[EXT2_DINDIRECT] = blockno;
			//first indirect block
			blockno = ext2_allocate_block(object, file, 0, head);
			if(blockno == INVALID_BLOCK) {
				(void)ext2_free_block(object, file, dindirect->number, head);
				return -ENOSPC;
			}
			indirect = ext2_synthetic_lookup_block(object, blockno);
			assert(indirect);
			if((r = chdesc_create_init(indirect, info->ubd, head)))
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init indirect block");
			if ((r = chdesc_create_byte(dindirect, info->ubd, 0, sizeof(uint32_t), &blockno, head)) < 0)
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add indirect block");
			if ((r = CALL(info->ubd, write_block, dindirect)) < 0)
				return r;
			if ((r = CALL(info->ubd, write_block, indirect)) < 0)
				return r;
			f->f_inode.i_blocks += EXT2_BLOCK_SIZE / 512;
		}
		r = add_indirect(object, f, block, head);
		if (r < 0)
			return r;		
	}
	else if (nblocks > EXT2_NDIRECT) {
		if(nblocks == (EXT2_NDIRECT+1)) {
			//allocate the indirect block pointer
			blockno = ext2_allocate_block(object, file, 0, head);
			if(blockno == INVALID_BLOCK)
				return -ENOSPC;
			f->f_inode.i_blocks += EXT2_BLOCK_SIZE / 512;
			f->f_inode.i_block[EXT2_NDIRECT] = blockno;
			indirect = ext2_synthetic_lookup_block(object, blockno);
			if(indirect == NULL)
				return -ENOSPC;
			if ((r = chdesc_create_init(indirect, info->ubd, head)))
				return r;
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init indirect block");
		} else {
			blockno = f->f_inode.i_block[EXT2_NDIRECT];
			indirect = ext2_lookup_block(object, f->f_inode.i_block[EXT2_NDIRECT]);
			if(indirect == NULL)
				return -ENOSPC;
		}
		offset = (nblocks - EXT2_NDIRECT - 1) * sizeof(uint32_t);
		//This is to account for the fact that indirect block now affects the block count
		if(nblocks > (EXT2_NDIRECT + 2))
			offset -= sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add block");
		r = CALL(info->ubd, write_block, indirect);
		if (r < 0)
			return r;
	}
	f->f_inode.i_blocks += EXT2_BLOCK_SIZE / 512;
	return ext2_write_inode(info, f->f_ino, f->f_inode, head);
}

static int ext2_write_dirent(LFS_t * object, EXT2_File_t * parent, EXT2_Dir_entry_t * dirent, 
				 uint32_t basep, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t blockno;
	bdesc_t * dirblock;
	int r;
	//check: off end of file?
	if (!parent || !dirent || !head)
		return -EINVAL;

	if (basep + dirent->rec_len > parent->f_inode.i_size)
		return -EINVAL;

	//dirent is in a single block:
	uint32_t actual_rec_len = 8 + ((dirent->name_len - 1) / 4 + 1) * 4;
	if (basep % EXT2_BLOCK_SIZE + actual_rec_len <= EXT2_BLOCK_SIZE) {
		//it would be brilliant if we could cache this, and not call get_file_block, read_block =)
		blockno = get_file_block(object, parent, basep);
		if (blockno == INVALID_BLOCK)
			return -1;

		basep %= EXT2_BLOCK_SIZE;

		dirblock = CALL(info->ubd, read_block, blockno, 1);
		if (!dirblock)
			return -1;
	     
		if ((r = chdesc_create_byte(dirblock, info->ubd, basep, actual_rec_len, (void *) dirent, head )) < 0)
			return r;

		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "write dirent");
		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;
	} else
		kpanic("overlapping dirent");
	return 0;
}

static int ext2_insert_dirent(LFS_t * object, EXT2_File_t * parent, EXT2_Dir_entry_t * new_dirent, chdesc_t ** head)
{ 
	Dprintf("EXT2DEBUG: ext2_insert_dirent %s\n", new_dirent->name);
	const EXT2_Dir_entry_t * entry;
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t new_prev_len, basep = 0, prev_basep = 0, new_block;
	int r = 0;
	int newdir = 0;
	bdesc_t * block;
	chdesc_t * prev_head;
	
	if (parent->f_inode.i_size == 0)
		newdir = 1;	
	
	while (r >= 0 && !newdir) {
		r = ext2_get_disk_dirent(object, parent, &basep, &entry);
		if (r == -1)
			return r;
		else if (r < 0)
			return r;

		//check if we can overwrite a jump dirent:
		if (!entry->inode && entry->rec_len >= new_dirent->rec_len) {
			new_dirent->rec_len = entry->rec_len;
			return ext2_write_dirent(object, parent, new_dirent, prev_basep, head);
		}

		//check if we can insert the dirent:
		else if ((entry->rec_len - (8 + entry->name_len)) > new_dirent->rec_len) {
			EXT2_Dir_entry_t copy = *entry;
			new_prev_len =  8 + ((copy.name_len - 1) / 4 + 1) * 4;
			new_dirent->rec_len = copy.rec_len - new_prev_len;
			copy.rec_len = new_prev_len;

			r = ext2_write_dirent(object, parent, &copy, prev_basep, head);
			if (r < 0)
				return r;

			return ext2_write_dirent(object, parent, new_dirent, prev_basep + copy.rec_len, head);
		}
		//detect the end of file, and break
		if (prev_basep + entry->rec_len == parent->f_inode.i_size)
			break;
		prev_basep = basep;
	}

	//test the aligned case! test by having a 16 whatever file
	new_block = ext2_allocate_block(object, (fdesc_t *) parent, 1, head);
	if (new_block == INVALID_BLOCK)
		return -EINVAL;
	/* FIXME: these errors should all free the block we allocated! */
	block = CALL(info->ubd, synthetic_read_block, new_block, 1);
	if (block == NULL)
		return -ENOSPC;
	r = chdesc_create_init(block, info->ubd, head);
	if (r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "init new dirent block");
	r = CALL(info->ubd, write_block, block);
	if (r < 0)
		return r;
	parent->f_inode.i_size += EXT2_BLOCK_SIZE;
	prev_head = *head;
	r = ext2_append_file_block(object, (fdesc_t *) parent, new_block, &prev_head);
	if (r < 0)
		return r;
	lfs_add_fork_head(prev_head);
	
	if (newdir)
	{	//fix the size of the dirent:
		new_dirent->rec_len = parent->f_inode.i_size;	
		r = ext2_write_dirent(object, parent, new_dirent, 0, head);
		if (r < 0)
			return r;
	} else {
		new_dirent->rec_len = EXT2_BLOCK_SIZE;
		r = ext2_write_dirent(object, parent, new_dirent, prev_basep + entry->rec_len, head);
		if (r < 0)
			return r;
	}
	return 0;
}

static int find_free_inode_block_group(LFS_t * object, inode_t * ino) {
	Dprintf("EXT2DEBUG: %s inode number is %u\n", __FUNCTION__, *ino);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	bdesc_t * bitmap;
	inode_t curr = 0;

	if (*ino >= info->super->s_inodes_count) 
	{
		printf("%s requested status of inode %u too large!\n",__FUNCTION__, *ino);
		return -ENOSPC;
	}
	
	curr = *ino;
	
	uint32_t block_group = curr  / info->super->s_inodes_per_group;
	
	
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
			info->inode_cache = bitmap;
		}
		
		const unsigned long * foo = (const unsigned long *)info->inode_cache->ddesc->data;
		//assert((curr % info->super->s_inodes_per_group) == 0);
		int bar = 0;
		bar = find_zero_bit(foo, info->super->s_inodes_per_group/*, (curr % info->super->s_inodes_per_group)*/ );
		if (bar < (info->super->s_inodes_per_group)) {
			curr += bar + 1; 
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
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	inode_t ino = 0;
	int r;
	
	ino = (parent / info->super->s_inodes_per_group) * info->super->s_inodes_per_group;
	r = find_free_inode_block_group(object, &ino);
	if (r != -ENOSPC) {
		return ino;
	}
	
	return EXT2_BAD_INO;
}

static fdesc_t * ext2_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link,
					 const metadata_set_t * initialmd, inode_t * newino, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_allocate_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	EXT2_File_t *dir = NULL, *newf = NULL;
	uint16_t mode, x16;
	uint32_t x32;
	ext2_fdesc_t * ln = (ext2_fdesc_t *) link;
	chdesc_t * prev_head;
	uint32_t ino;
	EXT2_Dir_entry_t new_dirent;
	int r, createdot = 0;
	char * link_buf = NULL;
	uint8_t file_type;

	//what is link?  link is a symlink fdesc.  dont deal with it, yet.
	if (!head || strlen(name) > EXT2_NAME_LEN)
		return NULL;

	//TODO: we need some way to prevent regular users from creating . and ..
	// Don't create directory hard links, except for . and ..
	if (!strcmp(name, "."))
		createdot = 1;
	else if (!strcmp(name, ".."))
		createdot = 1;

	switch (type)
	{
		case TYPE_FILE:
			mode = EXT2_S_IFREG;
			file_type = TYPE_FILE;
			break;
		case TYPE_DIR:
			mode = EXT2_S_IFDIR;
			file_type = TYPE_DIR;
			break;
		case TYPE_SYMLINK:      
			mode = EXT2_S_IFLNK;
			file_type = TYPE_SYMLINK;
		        break;
		default:
			return NULL;
	}

	if (ln && !createdot && type == TYPE_DIR)
		createdot = 1;

	// Don't link files of different types
	if (ln && file_type != ln->f_type)
		return NULL;

	dir = (ext2_fdesc_t *) ext2_lookup_inode(object, parent);
	if (!dir)
		return NULL;
	
	//FIXME this is redundent
	/*	r = ext2_lookup_name(object, parent, name, NULL);
	if (r >= 0) // File exists already
	goto allocate_name_exit; */

	if (!ln) {
		ino = ext2_find_free_inode(object, parent); 
		if (ino == EXT2_BAD_INO)
			goto allocate_name_exit;

		newf = (ext2_fdesc_t *) malloc(sizeof(ext2_fdesc_t));

		if (!newf)
			goto allocate_name_exit;

		newf->common = &newf->base;
		newf->base.parent = INODE_NONE;
		newf->f_nopen = 1;
		newf->f_lastblock = 0;
		newf->f_ino = ino;
		newf->f_type = file_type;

		memset(&newf->f_inode, 0, sizeof(struct EXT2_inode));
		
		r = hash_map_insert(info->filemap, (void *) ino, newf);
		if(r < 0)
			goto allocate_name_exit2;
		assert(r == 0);

		r = initialmd->get(initialmd->arg, KFS_feature_uid.id, sizeof(x32), &x32);
		if (r > 0)
			newf->f_inode.i_uid = x32;
		else if (r == -ENOENT)
			newf->f_inode.i_uid = 0;
		else
			assert(0);

		r = initialmd->get(initialmd->arg, KFS_feature_gid.id, sizeof(x32), &x32);
		if (r > 0)
			newf->f_inode.i_gid = x32;
		else if (r == -ENOENT)
			newf->f_inode.i_gid = 0;
		else
			assert(0);

		newf->f_inode.i_mode = mode | EXT2_S_IRUSR | EXT2_S_IWUSR;

		r = initialmd->get(initialmd->arg, KFS_feature_unix_permissions.id, sizeof(x16), &x16);
		if (r > 0)
			newf->f_inode.i_mode |= x16;
		else if (r != -ENOENT)
			assert(0);

		newf->f_inode.i_links_count = 1; 

		r = write_inode_bitmap(object, ino, 1, head);
		if (r != 0)
			goto allocate_name_exit2;

		if (type == TYPE_SYMLINK) {
			link_buf = malloc(EXT2_BLOCK_SIZE);
			if (!link_buf) {
				r = -ENOMEM;
				goto allocate_name_exit2;
			}
			r = initialmd->get(initialmd->arg, KFS_feature_symlink.id, EXT2_BLOCK_SIZE, link_buf);
			if (r < 0)
				goto allocate_name_exit2;
			else {
				r = ext2_set_metadata(object, newf, KFS_feature_symlink.id, r, link_buf, head);
				if (r < 0)
					goto allocate_name_exit2;
			}
			
		}

		r = ext2_write_inode(info, newf->f_ino, newf->f_inode, head);
		if (r < 0)
			goto allocate_name_exit2;

		*newino = ino;

	} else {
		newf = (ext2_fdesc_t *) ext2_lookup_inode(object, ln->f_ino);
		
		assert(ln == newf);
		if (!newf)
			goto allocate_name_exit;
		*newino = ln->f_ino;

		// Increase link count
		ln->f_inode.i_links_count++;
		r = ext2_write_inode(info, ln->f_ino, ln->f_inode, head);
		if (r < 0)
			goto allocate_name_exit2;
	}

	// create the directory entry
	new_dirent.inode = *newino;
	new_dirent.name_len = strlen(name);
	//round len up to multiple of 4 bytes:
	//(this value just computed for searching for a slot)
	new_dirent.rec_len = 8 + ((new_dirent.name_len - 1) / 4 + 1) * 4;
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
	strncpy(new_dirent.name, name, EXT2_NAME_LEN);
	prev_head = *head;
	r = ext2_insert_dirent(object, dir, &new_dirent, head);
	if (r < 0) {
		printf("Inserting a dirent in allocate_name failed for \"%s\"!\n", name);
		goto allocate_name_exit2;
	}

	// Create . and ..
	// FIXME: this should probably be before the dirent is inserted, in the !ln case above
	if (type == TYPE_DIR && !createdot) {
		inode_t ino;
		fdesc_t * cfdesc;
		metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };

		//TODO could save time by not reopening the parent!
		//in fact, just insert into the parent dirently!1
		cfdesc = ext2_allocate_name(object, newf->f_ino, ".", TYPE_DIR, (fdesc_t *) newf, &emptymd, &ino, &prev_head);
		if (!cfdesc)
			goto allocate_name_exit2;
		ext2_free_fdesc(object, (fdesc_t *)cfdesc);
		cfdesc = ext2_allocate_name(object, newf->f_ino, "..", TYPE_DIR, (fdesc_t *) dir, &emptymd, &ino, &prev_head);
		if (!cfdesc)
			goto allocate_name_exit2;
		ext2_free_fdesc(object, (fdesc_t *)cfdesc);
		lfs_add_fork_head(prev_head);

		uint32_t group = (newf->f_ino - 1) / info->super->s_inodes_per_group;
		r = CALL(info->super_wb, write_gdesc, group, 0, 0, 1);
		if (r < 0)
			goto allocate_name_exit2;
	}

	ext2_free_fdesc(object, (fdesc_t *)dir);
	return (fdesc_t *)newf;

allocate_name_exit2:
	free(link_buf);
	ext2_free_fdesc(object, (fdesc_t *)newf);

allocate_name_exit:
	ext2_free_fdesc(object, (fdesc_t *)dir);
	return NULL;
}

static int empty_get_metadata(void * arg, uint32_t id, size_t size, void * data)
{
	return -ENOENT;
}

static uint32_t ext2_erase_block_ptr(LFS_t * object, EXT2_File_t * file, uint32_t offset, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t blocknum, pointers_per_block;
	bdesc_t * block_desc, * double_block_desc;
	uint32_t * block_nums,* double_block_nums, indir_ptr, double_indir_ptr;
	int r;
	uint32_t target = INVALID_BLOCK;

	pointers_per_block = EXT2_BLOCK_SIZE / (sizeof(uint32_t));

	//non block aligned offsets suck (aka aren't supported)


	if (offset <= EXT2_BLOCK_SIZE)
		blocknum = 0;
	else if ( (offset % EXT2_BLOCK_SIZE) == 0)
		blocknum = (offset / EXT2_BLOCK_SIZE) - 1;
	else
		blocknum = offset / EXT2_BLOCK_SIZE;

	if (blocknum < EXT2_NDIRECT)
	{
		target = file->f_inode.i_block[blocknum];
		file->f_inode.i_block[blocknum] = 0;
		if (file->f_inode.i_size > EXT2_BLOCK_SIZE)
			file->f_inode.i_size = file->f_inode.i_size - EXT2_BLOCK_SIZE;
		else
			file->f_inode.i_size = 0;

	}
	else if (blocknum < EXT2_NDIRECT + pointers_per_block)
	{
		blocknum -= EXT2_NDIRECT;
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_NINDIRECT], 1));
		if (!block_desc)
			return INVALID_BLOCK;      
		block_nums = (uint32_t *)block_desc->ddesc->data;
		target = block_nums[blocknum];

		if (blocknum == 0)
		{
			indir_ptr = file->f_inode.i_block[EXT2_NDIRECT];
			if (file->f_inode.i_size > EXT2_BLOCK_SIZE)
				file->f_inode.i_size = file->f_inode.i_size - EXT2_BLOCK_SIZE;
			else
				file->f_inode.i_size = 0;
			r = ext2_free_block(object, (fdesc_t *) file, indir_ptr, head);
			if (r < 0)
				return INVALID_BLOCK;
			file->f_inode.i_blocks -= EXT2_BLOCK_SIZE / 512;
			file->f_inode.i_block[EXT2_NDIRECT] = 0;
		} else {
			if (file->f_inode.i_size > EXT2_BLOCK_SIZE)
				file->f_inode.i_size -= EXT2_BLOCK_SIZE;
			else
				file->f_inode.i_size = 0;
			//r = chdesc_create_byte(block_desc, info->ubd, blocknum * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
			//if (r < 0)
			//    return INVALID_BLOCK;
			//   r = CALL(info->ubd, write_block, block_desc);
			// if (r < 0)
			//    return INVALID_BLOCK;
		}
	}
	else if (blocknum < EXT2_NDIRECT + pointers_per_block + pointers_per_block * pointers_per_block)
	{
		blocknum -= (EXT2_NDIRECT + pointers_per_block);
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_DINDIRECT], 1));
		if (!block_desc)
			return INVALID_BLOCK;
		block_nums = (uint32_t *)block_desc->ddesc->data;
		indir_ptr = block_nums[blocknum / pointers_per_block];
		double_block_desc = CALL(info->ubd, read_block, indir_ptr, 1);
		if (!block_desc)
			return INVALID_BLOCK;
		double_block_nums = (uint32_t *)double_block_desc->ddesc->data;
		double_indir_ptr = (blocknum % pointers_per_block);
		target = double_block_nums[double_indir_ptr];

		if (file->f_inode.i_size > EXT2_BLOCK_SIZE)
			file->f_inode.i_size -= EXT2_BLOCK_SIZE;
		else
			file->f_inode.i_size = 0;

		if (blocknum % pointers_per_block == 0)
		{
			if (blocknum == 0)
			{
				r = ext2_free_block(object, (fdesc_t *) file, file->f_inode.i_block[EXT2_DINDIRECT], head);
				if (r < 0)
					return INVALID_BLOCK;
				file->f_inode.i_blocks -= EXT2_BLOCK_SIZE / 512;
				file->f_inode.i_block[EXT2_DINDIRECT] = 0;
			}
			else
			{
				//r = chdesc_create_byte(block_desc, info->ubd, (blocknum / pointers_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
				//if (r < 0)
				//	return INVALID_BLOCK;
				//r = CALL(info->ubd, write_block, block_desc);
				//if (r < 0)
				//	return INVALID_BLOCK;
			}
			r = ext2_free_block(object, (fdesc_t *) file, indir_ptr, head);
			if (r < 0)
				return INVALID_BLOCK;
			file->f_inode.i_blocks -= EXT2_BLOCK_SIZE / 512;
		}
		else
		{
			//r = chdesc_create_byte(double_block_desc, info->ubd, (blocknum % pointers_per_block) * sizeof(uint32_t), sizeof(uint32_t), &zero, head);
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
	//file->f_inode.i_blocks -= EXT2_BLOCK_SIZE / 512;
	//r = ext2_write_inode(info, file->f_ino, file->f_inode, head);
	//if (r < 0)
	//	return INVALID_BLOCK;
	return target;
}

static uint32_t ext2_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_truncate_file_block\n");
	int r;
	uint32_t target;
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);

	EXT2_File_t * f = (EXT2_File_t *)file;

	if (!f || f->f_inode.i_blocks == 0 || f->f_type == TYPE_SYMLINK)
	       return INVALID_BLOCK;

	if (f->f_inode.i_size == 0)
	       return INVALID_BLOCK;

	//ext2_erase_block_ptr will either return INLID_BLOCK, or the block that was truncated...
	target = ext2_erase_block_ptr(object, f, f->f_inode.i_size, head);
	f->f_inode.i_blocks -= EXT2_BLOCK_SIZE / 512;
	r = ext2_write_inode(info, f->f_ino, f->f_inode, head);
	if (r < 0)
		return INVALID_BLOCK;
	return target;
}

static int ext2_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_rename\n");
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	ext2_fdesc_t * old, * new, * oldpar, * newpar;
	int r, existing = 0;
	uint32_t basep = 0, prev_basep = 0, prev_prev_basep = 0;
	const EXT2_Dir_entry_t * old_dirent;
	const EXT2_Dir_entry_t * new_dirent;
	inode_t newino;
	chdesc_t * prev_head = NULL;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };
	
	if (!head || strlen(oldname) > EXT2_NAME_LEN || strlen(newname) > EXT2_NAME_LEN)
		return -EINVAL;

	if (!strcmp(oldname, newname) && (oldparent == newparent))
		return 0;

	oldpar = (ext2_fdesc_t *)ext2_lookup_inode(object, oldparent);
	if (!oldpar)
		return -ENOENT;

	for (r = 0; r >= 0; )
	{
		r = ext2_get_disk_dirent(object, oldpar, &basep, &old_dirent);
		if (!r && strcmp(old_dirent->name, oldname) == 0)
			break;
		if (r < 0) 
			goto ext2_rename_exit;
	}

	old = (ext2_fdesc_t *)ext2_lookup_inode(object, old_dirent->inode);
	if (!old) {
		r = -ENOENT;
		goto ext2_rename_exit;
	}

	newpar = (ext2_fdesc_t *)ext2_lookup_inode(object, newparent);
	if (!newpar) {
		r = -ENOENT;
		goto ext2_rename_exit2;
	}
	
	basep = 0;
	for (r = 0; r >= 0;)
	{
		r = ext2_get_disk_dirent(object, newpar, &basep, &new_dirent);
		if (!r && strcmp(new_dirent->name, newname) == 0) {
			new = (ext2_fdesc_t *)ext2_lookup_inode(object, new_dirent->inode);
			break;
		}
		
		if (r == -ENOENT || r == -1) {
			new = NULL;
			break;
		}

		if (r < 0)
			goto ext2_rename_exit3;
	}

	if (new) {
		EXT2_Dir_entry_t copy = *new_dirent;

		// Overwriting a directory makes little sense
		if (new->f_type == TYPE_DIR) {
			r = -ENOTEMPTY;
			goto ext2_rename_exit4;
		}

		// File already exists
		existing = 1;

		copy.inode = old->f_ino;
		r = ext2_write_dirent(object, newpar, &copy, basep, head);
		if (r < 0)
			goto ext2_rename_exit4;
		prev_head = *head;

		old->f_inode.i_links_count++;
		r = ext2_write_inode(info, old->f_ino, old->f_inode, head);
		if (r < 0)
			goto ext2_rename_exit4;
	}
	else {
		// Link files together
		new = (ext2_fdesc_t *) ext2_allocate_name(object, newparent, newname, old->f_type, (fdesc_t *) old, &emptymd, &newino, head);
		if (!new) {
			r = -1;
			goto ext2_rename_exit3;
		}
		//assert(new_dirent->inode == newino);
	}

	basep = 0;
	for (r = 0; r >= 0; )
	{
		prev_prev_basep = prev_basep;
		prev_basep = basep;
		r = ext2_get_disk_dirent(object, oldpar, &basep, &old_dirent);
		if (!r && strcmp(old_dirent->name, oldname) == 0)
			break;
		if (r < 0) 
			goto ext2_rename_exit;
	}
	r = ext2_delete_dirent(object, oldpar, prev_basep, prev_prev_basep, head);
	if (r < 0)
		goto ext2_rename_exit4;

	old->f_inode.i_links_count--;
	r = ext2_write_inode(info, old->f_ino, old->f_inode, head);
	if (r < 0)
		goto ext2_rename_exit4;

	if (existing) {
		new->f_inode.i_links_count--;
		r = ext2_write_inode(info, new->f_ino, new->f_inode, &prev_head);
		if (r < 0)
			goto ext2_rename_exit4;

		if (new->f_inode.i_links_count == 0) {
			uint32_t block, i, n = ext2_get_file_numblocks(object, (fdesc_t *)new);
			for (i = 0; i < n; i++) {
				block = ext2_truncate_file_block(object, (fdesc_t *) new, &prev_head);
				if (block == INVALID_BLOCK) {
					r = -1;
					goto ext2_rename_exit4;
				}
				r = ext2_free_block(object, (fdesc_t *) new, block, &prev_head);
				if (r < 0)
					goto ext2_rename_exit4;
			}

			memset(&new->f_inode, 0, sizeof(EXT2_inode_t));
			r = ext2_write_inode(info, new->f_ino, new->f_inode, &prev_head);
			if (r < 0)
				goto ext2_rename_exit4;

			r = write_inode_bitmap(object, new->f_ino, 0, &prev_head);
			if (r < 0)
				goto ext2_rename_exit4;
			lfs_add_fork_head(prev_head);
		}
	}

	r = 0;

ext2_rename_exit4:
	ext2_free_fdesc(object, (fdesc_t *) new);
ext2_rename_exit3:
	ext2_free_fdesc(object, (fdesc_t *) newpar);
ext2_rename_exit2:
	ext2_free_fdesc(object, (fdesc_t *) old);
ext2_rename_exit:
	ext2_free_fdesc(object, (fdesc_t *) oldpar);
	return r;
}

/* so both ufs_free_block and fs/ext2/ext2_free_blocks dont erase the block pointer in the inode...
so to delete a file, first multiple calls to truncate file block must me made, which returns the the
block truncated.  I'm not a huge fan of this design, I'll be honest.  In fact it rather upsets me that
an above module can call to free a block, completely ignoring the file system. BOOOOOO! */

static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_free_block\n");
	int r;

	if(!head || block == INVALID_BLOCK)
	      return -EINVAL;
	
	r = write_block_bitmap(object, block, 0, head);
	if (r < 0)
	{
	      Dprintf("failed to free block %d in bitmap\n", block);
	      return r;
	}
	
	return r;
}

static int ext2_delete_dirent(LFS_t * object, ext2_fdesc_t * dir_file, uint32_t basep, uint32_t prev_basep, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_delete_dirent %\n", basep);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	uint32_t basep_blockno, prev_basep_blockno;
	uint16_t len;
	bdesc_t * dirblock;
	int r = 0;


	//if the basep is at the start of a block, zero it out.
	if(basep % EXT2_BLOCK_SIZE == 0) {
		EXT2_Dir_entry_t jump_dirent;

		basep_blockno = get_file_block(object, dir_file, basep);
		if (basep_blockno == INVALID_BLOCK)

			return -1;
		dirblock = CALL(info->ubd, read_block, basep_blockno, 1);
		if(!dirblock)
			return -1;
		len = *((uint16_t *)(dirblock->ddesc->data + (sizeof(inode_t))));
		jump_dirent.rec_len = len;
		jump_dirent.inode = 0;

		if ((r = chdesc_create_byte(dirblock, info->ubd, 0, 6, &jump_dirent, head) < 0))
			return r;

		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "delete dirent, add jump dirent");
		return CALL(info->ubd, write_block, dirblock);
	} 

	//if deleting in the middle of a block, increase length of previous dirent
	else {
		prev_basep_blockno = get_file_block(object, dir_file, prev_basep);
		if (prev_basep_blockno == INVALID_BLOCK)
			return -1;
		dirblock = CALL(info->ubd, read_block, prev_basep_blockno, 1);
		if(!dirblock)
			return -1;

		//get the length of the deleted dirent

		len = *((uint16_t *)(dirblock->ddesc->data + basep % EXT2_BLOCK_SIZE + (sizeof(inode_t))));

		//get the length of the previous dirent:
		len += basep - prev_basep;
		
		//update the length of the previous dirent:
		if ((r = chdesc_create_byte(dirblock, info->ubd,  ((prev_basep + 4) % EXT2_BLOCK_SIZE), sizeof(len), (void *) &len, head) < 0))
			return r;
	
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "delete dirent");
		return CALL(info->ubd, write_block, dirblock);
	}
}

static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_remove_name %s\n", name);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	chdesc_t * prev_head;
	ext2_fdesc_t * pfile = NULL, * file = NULL;

	int r;

	if (!head)
	      return -EINVAL;

	pfile = (ext2_fdesc_t *) ext2_lookup_inode(object, parent);
	if (!pfile)
		return -EINVAL;

	if (pfile->f_type != TYPE_DIR) {
	      r  = -ENOTDIR;
	      goto remove_name_exit;
	}	
	
	//put in sanity checks here!
	uint32_t basep = 0, prev_basep = 0, prev_prev_basep;
	uint8_t minlinks = 1;
	
	r = dir_lookup(object, pfile, name, &basep, &prev_basep, &prev_prev_basep, &file);
	if (r < 0)
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

	r = ext2_delete_dirent(object, pfile, prev_basep, prev_prev_basep, head);
	if (r < 0)
	      goto remove_name_exit;
	assert (file->f_inode.i_links_count >= minlinks);

	//Remove link to parent directory
	if (file->f_type == TYPE_DIR) {
	      pfile->f_inode.i_links_count--;
	      prev_head = *head;
	      r = ext2_write_inode(info, pfile->f_ino, pfile->f_inode, &prev_head);
	      if (r < 0)
		     goto remove_name_exit;
	      lfs_add_fork_head(prev_head);
	}
	
	if (file->f_inode.i_links_count == minlinks) {
		// Truncate the directory
		if (file->f_type == TYPE_DIR) {
			uint32_t number, nblocks, j, group;
			group = (file->f_ino - 1) / info->super->s_inodes_per_group;
			nblocks = ext2_get_file_numblocks(object, (fdesc_t *) file);
			
			for (j = 0; j < nblocks; j++) {
				prev_head = *head;
				number = ext2_erase_block_ptr(object, file, file->f_inode.i_size, &prev_head);
				if (number == INVALID_BLOCK) {
					r = -EINVAL;
					goto remove_name_exit;
				}

				r = ext2_free_block(object, (fdesc_t *) file, number, &prev_head);
				if (r < 0)
					goto remove_name_exit;
				lfs_add_fork_head(prev_head);
			}
			r = CALL(info->super_wb, write_gdesc, group, 0, 0, -1);
			if(r < 0)
				goto remove_name_exit;				
		}
		
		memset(&file->f_inode, 0, sizeof(EXT2_inode_t));
		r = ext2_write_inode(info, file->f_ino, file->f_inode, head);
		if (r < 0)
			goto remove_name_exit;

		r = write_inode_bitmap(object, file->f_ino, 0, head);
		if (r < 0)
			goto remove_name_exit;
	} else {
	      file->f_inode.i_links_count--;
	      r = ext2_write_inode(info, file->f_ino, file->f_inode, head);
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
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);

	if (!head)
		return -EINVAL;

	return CALL(info->ubd, write_block, block);
}

static chdesc_t * ext2_get_write_head(LFS_t * object)
{
	Dprintf("EXT2DEBUG: ext2_get_write_head\n");
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	return CALL(info->ubd, get_write_head);
}

static int32_t ext2_get_block_space(LFS_t * object)
{
	Dprintf("EXT2DEBUG: ext2_get_block_space\n");
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	return CALL(info->ubd, get_block_space);
}

static const feature_t * ext2_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_freespace, &KFS_feature_file_lfs, &KFS_feature_blocksize, &KFS_feature_devicesize, &KFS_feature_mtime, &KFS_feature_atime, &KFS_feature_gid, &KFS_feature_uid, &KFS_feature_unix_permissions, &KFS_feature_nlinks, &KFS_feature_symlink};

static size_t ext2_get_num_features(LFS_t * object, inode_t ino)
{
	return sizeof(ext2_features) / sizeof(ext2_features[0]);
}

static const feature_t * ext2_get_feature(LFS_t * object, inode_t ino, size_t num)
{
	if(num < 0 || num >= sizeof(ext2_features) / sizeof(ext2_features[0]))
		return NULL;
	return ext2_features[num];
}

static int ext2_get_metadata(LFS_t * object, const ext2_fdesc_t * f, uint32_t id, size_t size, void * data)
{
	Dprintf("EXT2DEBUG: ext2_get_metadata\n");
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	if (id == KFS_feature_size.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_size;
	}
	else if (id == KFS_feature_filetype.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_type;
	}
	else if (id == KFS_feature_freespace.id) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = count_free_space(object);
	}
	else if (id == KFS_feature_file_lfs.id) {
		if (size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);

		*((typeof(object) *) data) = object;
	}
	else if (id == KFS_feature_blocksize.id) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = ext2_get_blocksize(object);
	}
	else if (id == KFS_feature_devicesize.id) {
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = info->super->s_blocks_count;
	}
	else if (id == KFS_feature_nlinks.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = (uint32_t) f->f_inode.i_links_count;
	}
	else if (id == KFS_feature_uid.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_uid;
	}
	else if (id == KFS_feature_gid.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_gid;
	}
	else if (id == KFS_feature_unix_permissions.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);

		*((uint16_t *) data) = f->f_inode.i_mode & ~EXT2_S_IFMT;
	}
	else if (id == KFS_feature_mtime.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_mtime;
	}
	else if (id == KFS_feature_atime.id) {
		if (!f)
			return -EINVAL;

		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_inode.i_atime;
	}
	else if (id == KFS_feature_symlink.id) {
		struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
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
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	int r = 0;
	uint32_t new_block_no;
	bdesc_t * new_block;
	
	if (name_len > EXT2_BLOCK_SIZE)
		return -ENAMETOOLONG;
	new_block_no = ext2_allocate_block(object, (fdesc_t *) f, 1, head);
	if (new_block_no == INVALID_BLOCK)
		 return -EINVAL;

	//TODO dont assume this is written after this function returns! (BAD!!)
	f->f_inode.i_block[0] = new_block_no;
	new_block = CALL(info->ubd, synthetic_read_block, new_block_no, 1);
	if (!new_block)
		return -1;

	r = chdesc_create_byte(new_block, info->ubd, 0, name_len, (void *) name, head);
	if (r < 0)
		return r;

	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "add slow symlink");	
	return CALL(info->ubd, write_block, new_block);
}

static int ext2_set_metadata(LFS_t * object, ext2_fdesc_t * f, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_set_metadata %u, %u\n", id, size);
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(object);
	
	if (!head || !f || !data)
		return -EINVAL;

	if (id == KFS_feature_size.id) {
		if (sizeof(uint32_t) != size || *((uint32_t *) data) < 0 || *((uint32_t *) data) >= EXT2_MAX_FILE_SIZE)
			return -EINVAL;
		f->f_inode.i_size = *((uint32_t *) data);
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_filetype.id) {
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
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_uid.id) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_uid = *(uint32_t *) data;
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_gid.id) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_gid = *(uint32_t *) data;
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_unix_permissions.id) {
		if (sizeof(uint16_t) != size)
			return -EINVAL;
		f->f_inode.i_mode = (f->f_inode.i_mode & EXT2_S_IFMT)
			| (*((uint16_t *) data) & ~EXT2_S_IFMT);
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_mtime.id ) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_mtime = *((uint32_t *) data);
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_atime.id) {
		if (sizeof(uint32_t) != size)
			return -EINVAL;
		f->f_inode.i_atime = *((uint32_t *) data);
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
	}
	else if (id == KFS_feature_symlink.id) {
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
		f->f_inode.i_size = size;  //size must include zerobyte!
		return ext2_write_inode(info, f->f_ino, f->f_inode, head);
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
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(lfs);
	chdesc_t * head = NULL; /* CALL(lfs, get_write_head); */
	CALL(info->super_wb, sync, &head);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	DESTROY(info->super_wb);
	modman_dec_bd(info->ubd, lfs);
	
	hash_map_destroy(info->filemap);
	if(info->bitmap_cache != NULL)
		bdesc_release(&info->bitmap_cache);
	if(info->inode_cache != NULL)
		bdesc_release(&info->inode_cache);

	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

/*
 *  Reads group descriptor of inode number ino and sets inode to that inode
 */

static int ext2_get_inode(ext2_info_t * info, inode_t ino, EXT2_inode_t * inode)
{
	uint32_t block_group, bitoffset, block;
	bdesc_t * bdesc;

	if((ino != EXT2_ROOT_INO && ino < info->super->s_first_ino)
		       	|| ino > info->super->s_inodes_count)
		return -EINVAL;
	
	//Get the group the inode belongs in
	block_group = (ino - 1) / info->super->s_inodes_per_group;
	bitoffset = ((ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (bitoffset >> (10 + info->super->s_log_block_size));
	bdesc = CALL(info->ubd, read_block, block, 1);
	if(!bdesc)
		return -EINVAL;
	bitoffset &= (EXT2_BLOCK_SIZE - 1);
	memcpy(inode, (bdesc->ddesc->data + bitoffset ), sizeof(EXT2_inode_t));
	if(!inode)
		return -ENOENT;
	else
		return ino;
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

int ext2_write_inode(struct ext2_info * info, inode_t ino, EXT2_inode_t inode, chdesc_t ** head)
{
	uint32_t block_group, bitoffset, block;
	int r;
	bdesc_t * bdesc;
	if (!head)
		return -EINVAL;
	
	if((ino != EXT2_ROOT_INO && ino < info->super->s_first_ino)
		       	|| ino > info->super->s_inodes_count)
		return -EINVAL;

	//Get the group the inode belongs in
	block_group = (ino - 1) / info->super->s_inodes_per_group;
	
	bitoffset = ((ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = info->groups[block_group].bg_inode_table + (bitoffset >> (10 + info->super->s_log_block_size));
	bdesc = CALL(info->ubd, read_block, block, 1);
	if(!bdesc)
		return -ENOENT;
	bitoffset &= (EXT2_BLOCK_SIZE - 1);
	r = chdesc_create_diff(bdesc, info->ubd, bitoffset, sizeof(EXT2_inode_t), &bdesc->ddesc->data[bitoffset], &inode, head);
	if (r < 0)
		return r;

	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "write inode");
	return CALL(info->ubd, write_block, bdesc);
}

static void ext2_destroy_super(LFS_t * lfs)
{
	struct ext2_info * info = (struct ext2_info *) OBJLOCAL(lfs);
	if (info->super_wb)
		DESTROY(info->super_wb);
}

LFS_t * ext2(BD_t * block_device)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	if (!block_device)
		return NULL;
	
	struct ext2_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));
	
	if (!lfs)
		return NULL;

	info = malloc(sizeof(*info));
	if (!info) {
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, ext2, info);
	OBJMAGIC(lfs) = EXT2_FS_MAGIC;

	info->ubd = block_device;

	info->filemap = hash_map_create();
	if (!info->filemap) {
		free(info);
		free(lfs);
		return NULL;
	}

	info->super_wb = ext2_super_wb(lfs, info);
	if (!info->super_wb) {
		ext2_destroy_super(lfs);
		return NULL;
	}
	info->super = CALL(info->super_wb, read);
	info->ngroups = info->super->s_blocks_count / info->super->s_blocks_per_group;
	info->bitmap_cache = NULL;
	info->inode_cache = NULL;
	info->groups = NULL;
	info->gnum = INVALID_BLOCK;	
	info->inode_gdesc = INVALID_BLOCK;	

	if (check_super(lfs)) {
		free(info);
		free(lfs);
		return NULL;
	}
	
	info->groups = CALL(info->super_wb, read_gdescs);
	/*	if(ext2_load_groups(info) < 0) {
		free(info);
		free(lfs);
		return NULL;
		}*/
	
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
