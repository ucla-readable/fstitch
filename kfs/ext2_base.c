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
#include <kfs/ext2_base.h>
#include <kfs/feature.h>

#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in ext2_base.c
#endif

#define EXT2_BASE_DEBUG 0

#if EXT2_BASE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct lfs_info
{
	BD_t * ubd;
	bdesc_t * super_block;
	struct EXT2_Super * super;
};
typedef struct lfs_info lfs_info_t;

struct ext2_fdesc {
	/* extend struct fdesc */
	fdesc_common_t * common;
	fdesc_common_t base;

	uint32_t dirb; // Block number on the block device of a block in one of
	               // the containing directory's data blocks. It is the block
	               // which contains the on-disk File structure for this file.
	uint32_t index; // the byte index in that block of the EXT2_File_t for this file
	inode_t ino;
	EXT2_File_t * file;
};

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number);
static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head);
static int ext2_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep);
static uint32_t get_file_block(LFS_t * object, EXT2_File_t * file, uint32_t offset);
static uint32_t ext2_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset);
static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head);
static int ext2_set_metadata(LFS_t * object, struct ext2_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head);

static int ext2_get_group_desc(lfs_info_t * info, uint32_t block_group, EXT2_group_desc_t * gdesc);
static int ext2_get_inode(lfs_info_t * info, inode_t ino, EXT2_inode_t * inode);
static uint8_t ext2_to_kfs_type(uint16_t type);

static int read_bitmap(LFS_t * object, uint32_t blockno);
static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head);

// Equivalent to JOS's read_super
static int check_super(LFS_t * object)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	//const struct EXT2_Super * super = CALL(info->super_block, read);
	/* make sure we have the block size we expect
	if (CALL(info->ubd, get_blocksize) != EXT2_BLKSIZE) {
		printf("Block device size is not EXT2_BLKSIZE!\n");
		return -1;
	}*/
	
	
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
	//numblocks = CALL(info->ubd, get_numblocks);

	//printf("JOS Filesystem size: %u blocks (%uMB)\n", (int) super->s_nblocks, (int) (super->s_nblocks / (1024 * 1024 / EXT2_BLKSIZE)));
	//if (super->s_nblocks > numblocks) {
	//	printf("ext2_base: file system is too large\n");
	//	return -1;
	//}

	//bdesc_retain(info->super_block);
	return 0;
}




// Return 1 if block is free 
static int read_bitmap(LFS_t * object, uint32_t blockno)
{
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	uint32_t * ptr;

	if (blockno >= super->s_nblocks) {
		printf("ext2_base: requested status of block %u past end of file system!\n", blockno);
		return -1;
	}

	target = 2 + (blockno / (EXT2_BLKBITSIZE));

	if (info->bitmap_cache && info->bitmap_cache->number != target)
		bdesc_release(&info->bitmap_cache);

	if (! info->bitmap_cache) {
		bdesc = CALL(info->ubd, read_block, target, 1);
		if (!bdesc || bdesc->ddesc->length != EXT2_BLKSIZE) {
			printf("ext2_base: trouble reading bitmap! (blockno = %u)\n", blockno);
			return -1;
		}
		bdesc_retain(bdesc);
		info->bitmap_cache = bdesc;
	}

	ptr = ((uint32_t *) info->bitmap_cache->ddesc->data) + ((blockno % EXT2_BLKBITSIZE) / 32);
	if (*ptr & (1 << (blockno % 32)))
		return 1;
		*/
	return 0;
}

static int write_bitmap(LFS_t * object, uint32_t blockno, bool value, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: write_bitmap %u\n", blockno);
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	uint32_t target;
	chdesc_t * ch;
	int r;

	if (!head)
		return -1;

	if (blockno == 0) {
		printf("ext2_base: attempted to write status of zero block!\n");
		return -1;
	}
	else if (blockno >= super->s_nblocks) {
		printf("ext2_base: attempted to write status of block %u past end of file system!\n", blockno);
		return -1;
	}

	target = 2 + (blockno / EXT2_BLKBITSIZE);

	if (info->bitmap_cache && info->bitmap_cache->number == target)
		bdesc = info->bitmap_cache;
	else {
		if(info->bitmap_cache)
			bdesc_release(&info->bitmap_cache);
		bdesc = CALL(info->ubd, read_block, target, 1);

		if (!bdesc || bdesc->ddesc->length != EXT2_BLKSIZE) {
			printf("ext2_base: trouble reading bitmap! (blockno = %u)\n", blockno);
			return -1;
		}

		bdesc_retain(bdesc);
		info->bitmap_cache = bdesc;
	}

	// does it already have the right value? 
	if (((uint32_t *) bdesc->ddesc->data)[(blockno % EXT2_BLKBITSIZE) / 32] >> (blockno % 32) == value)
		return 0;
	// bit chdescs take offset in increments of 32 bits 
	ch = chdesc_create_bit(bdesc, info->ubd, (blockno % EXT2_BLKBITSIZE) / 32, 1 << (blockno % 32));
	if (!ch)
		return -1;

	if (*head)
		if ((r = chdesc_add_depend(ch, *head)) < 0)
			return r;

	*head = ch;

	r = CALL(info->ubd, write_block, bdesc);

	return r;
*/
	return 0;
}

static uint32_t count_free_space(LFS_t * object)
{
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	const uint32_t s_nblocks = super->s_nblocks;
	uint32_t i, count = 0;

	for (i = 0; i < s_nblocks; i++)
		if (read_bitmap(object, i))
			count++;
	return count;
*/
	return 0;
}

static int ext2_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != EXT2_FS_MAGIC)
		return -E_INVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ext2_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != EXT2_FS_MAGIC)
		return -E_INVAL;
	
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
	return ((struct lfs_info *) OBJLOCAL(object))->ubd;
}

// file and purpose parameter are ignored
static uint32_t ext2_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t s_nblocks = super->s_nblocks;
	uint32_t bitmap_size = (s_nblocks + EXT2_BLKBITSIZE + 1) / EXT2_BLKBITSIZE;
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
			bdesc_t * bdesc = CALL(info->ubd, read_block, bitmap_block+2, 1);
			if (!bdesc || bdesc->ddesc->length != EXT2_BLKSIZE)
			{
				printf("ext2_base: trouble reading bitmap! (blockno = %u)\n", bitmap_block+2);
				return -1;
			}
			bdesc_retain(bdesc);
			info->bitmap_cache = bdesc;
		}

		curbitmap = (uint32_t *) info->bitmap_cache->ddesc->data;
		for (blockno = 0; blockno < EXT2_BLKBITSIZE; blockno += 32, curbitmap++)
		{
			uint32_t mask = 1;
			uint32_t full_blockno;

			if (!*curbitmap)
				continue;

			while (!(*curbitmap & mask))
				mask <<= 1, blockno++;
			full_blockno = blockno + bitmap_block * EXT2_BLKBITSIZE;

			r = write_bitmap(object, full_blockno, 0, head);
			if (r < 0)
				return INVALID_BLOCK;
			assert(!block_is_free(object, full_blockno));
			return full_blockno;
		}
	}

	return INVALID_BLOCK;
*/
	return 0;
}

static bdesc_t * ext2_lookup_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_lookup_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, read_block, number, 1);
}

static bdesc_t * ext2_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	Dprintf("EXT2DEBUG: ext2_synthetic_lookup_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, synthetic_read_block, number, 1, synthetic);
}

static int ext2_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	Dprintf("EXT2DEBUG: ext2_cancel_synthetic_block %u\n", number);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	return CALL(info->ubd, cancel_block, number);
}

/*
 *  So UFS keeps track of open fdesc and doesn't create a new one if its already open, we should
 *  add that functionality.
 */
static fdesc_t * ext2_lookup_inode(LFS_t * object, inode_t ino)
{
	ext2_fdesc_t * fd;
	uint32_t r;
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	
	if(ino <= 0)
		return NULL;

	fd = (ext2_fdesc_t *)malloc(sizeof(ext2_fdesc_t));
	if (!fd)
		goto ext2_lookup_inode_exit;
	
	//TODO more needs to happen here with the base fd
	//and file type
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_ino = ino;

	r = ext2_get_inode(info, ino, &(fd->f_inode));
	if(r < 0)
		goto ext2_lookup_inode_exit;
	
	fd->f_type = ext2_to_kfs_type(fd->f_inode.i_mode);

	return (fdesc_t*)fd;

 ext2_lookup_inode_exit:
	free(fd);
	return NULL;
}

static void ext2_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("EXT2DEBUG: ext2_free_fdesc %p\n", fdesc);
	ext2_fdesc_t * f = (ext2_fdesc_t *) fdesc;

	if (f) {
		free(f);
	}

}

// Try to find a file named "name" in dir.  If so, set *file to it.
static int dir_lookup(LFS_t * object, ext2_fdesc_t * dir, const char* name, ext2_fdesc_t** file)
{
	Dprintf("EXT2DEBUG: dir_lookup %s\n", name);
	uint32_t i, basep = 0;
	int r = 0;
	struct dirent entry;

	for (i = 0; r >= 0; i++)
	{
		r = ext2_get_dirent(object, (fdesc_t *)dir, (struct dirent *) &(entry), sizeof(EXT2_Dir_entry_t), &basep);
		if (r == 0 && !strcmp(entry.d_name, name)) {
			*file = (ext2_fdesc_t *)ext2_lookup_inode(object, entry.d_fileno);
			if(!(*file)) {
				free(*file);
				return -E_NOT_FOUND;
			}
			return 0;
		}
	}

	*file = NULL;
	Dprintf("EXT2DEBUG: dir_lookup done: NOT FOUND\n");
	return r;
}


static int ext2_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("EXT2DEBUG: ext2_lookup_name %s\n", name);
	ext2_fdesc_t * fd;
	ext2_fdesc_t * parent_file;
	int r;
	
	//TODO do some sanity checks on name

	// "." and ".." are (at least right now) supported by code further up
	// (this seems hacky, but it would be hard to figure out parent's parent from here)

	fd = (ext2_fdesc_t *)ext2_lookup_inode(object, parent);
	if (!fd)
		return -E_NOT_FOUND;
	if (fd->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	parent_file = fd;
	r = dir_lookup(object, parent_file, name, &fd);
	if(fd)
		*ino = fd->f_ino;
	ext2_free_fdesc(object, (fdesc_t *) parent_file);
	ext2_free_fdesc(object, (fdesc_t *) fd);
	fd = NULL;
	parent_file = NULL;
	if (r < 0)
		return r;
	return 0;
}

static uint32_t ext2_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	
	if(f->f_type == TYPE_SYMLINK)
		return 0;

	return f->f_inode.i_blocks;
}

static uint32_t get_file_block(LFS_t * object, EXT2_File_t * file, uint32_t offset)
{
	Dprintf("EXT2DEBUG: %s %p %d\n", __FUNCTION__, file, offset);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	uint32_t blockno, pointers_per_block;
	bdesc_t * block_desc;
	uint32_t * inode_nums, blocknum;

	pointers_per_block = EXT2_BLOCK_SIZE / (sizeof(uint32_t));

	//non block aligned offsets suck (aka aren't supported)
	       
	blocknum = offset / EXT2_BLOCK_SIZE;

	//TODO: compress this code, but right now its much easier to understand...
	if (blocknum >= pointers_per_block * pointers_per_block + pointers_per_block + EXT2_NDIRECT)
	{
		/* Lets not worry about tripley indirect for the momment
		offset -= (inodes_per_block * inodes_per_block + inodes_per_block + EXT2_NDIRECT);
		inode_nums = (uint32_t * )(CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_TINDIRECT], 1));
		if (!inode_nums)
		{
		       Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
		       return EXT2_BAD_INO;
		}
		blockno = inode_nums[offset / (inodes_per_block * inodes_per_block)];
		inode_nums = (uint32_t * )(CALL(info->ubd, read_block, blockno, 1));
		if (!inode_nums)
		{
		       Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
		       return EXT2_BAD_INO;
		}
		offset %= inodes_per_block * inodes_per_block;
		blockno = inode_nums[offset / inodes_per_block];		
		inode_nums = (uint32_t * )(CALL(info->ubd, read_block, blockno, 1));
		if (!inode_nums)
		{
		       Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
		       return EXT2_BAD_INO;
		}
		offset %= inodes_per_block;
		return inode_nums[offset];
		*/
		return EXT2_BAD_INO;
	}
	else if (blocknum >= pointers_per_block + EXT2_NDIRECT)
	{
		blocknum -= (EXT2_NDIRECT + pointers_per_block);
		block_desc = (CALL(info->ubd, read_block, file->f_inode.i_block[EXT2_DINDIRECT], 1));
		if (!block_desc)
		{
		       Dprintf("failed dindirect block lookup in %s\n", __FUNCTION__);
		       return EXT2_BAD_INO;
		}
		inode_nums = (uint32_t *)block_desc->ddesc->data;
		blockno = inode_nums[blocknum / pointers_per_block];
		block_desc = CALL(info->ubd, read_block, blockno, 1);
		if (!block_desc)
		{
		       Dprintf("failed indirect block lookup in %s\n", __FUNCTION__);
		       return EXT2_BAD_INO;
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
		       return EXT2_BAD_INO;
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

static int fill_dirent(lfs_info_t * info, EXT2_Dir_entry_t * dirfile, inode_t ino, struct dirent * entry, uint16_t size, uint32_t * basep)
{
  	Dprintf("EXT2DEBUG: %s %s, %u\n", __FUNCTION__, entry->d_name, *basep);
	uint16_t namelen = MIN(dirfile->name_len, sizeof(entry->d_name) - 1);
	uint16_t reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;
	uint32_t old_basep;
	
	if (size < reclen || !basep)
		return -E_INVAL;

	old_basep = *basep;

	// If the name length is 0 (or less?) then we assume it's an empty slot
	if (namelen < 1) {
		return -E_UNSPECIFIED;
	}

	entry->d_fileno = ino;

	switch(ext2_to_kfs_type(dirfile->file_type))
	{
		case TYPE_FILE:
			entry->d_type = TYPE_FILE;
			break;
		case TYPE_DIR:
			entry->d_type = TYPE_DIR;
			break;
		default:
			entry->d_type = TYPE_INVAL;
	}
	
	EXT2_inode_t inode;

	if (ext2_get_inode(info, ino, &inode) < 0)
	      return -E_UNSPECIFIED;
	

	entry->d_filesize = 0;//inode.i_size;
	entry->d_reclen = reclen;
	entry->d_namelen = namelen;
	strncpy(entry->d_name, dirfile->name, namelen);
	entry->d_name[namelen] = 0;

	*basep += dirfile->rec_len;
	if (old_basep >= *basep)
	  return -E_UNSPECIFIED;
	return 0;
}

//so you pass in a file desc thats a directory... makes sense...
static int ext2_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
  	Dprintf("EXT2DEBUG: ext2_get_dirent %p, %u\n", basep, *basep);
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	ext2_fdesc_t * f = (ext2_fdesc_t *) file;
	bdesc_t *dirblock1, *dirblock2;
	EXT2_Dir_entry_t dirfile;
	uint32_t blockno, file_blockno1, file_blockno2;
	uint32_t num_file_blocks, num_file_bytes;
	int r = 0;

	if (f->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	num_file_bytes = 0;
	num_file_blocks = ext2_get_file_numblocks(object, (fdesc_t *)f);

	do {
	       
	       blockno = *basep / (1024 << info->super->s_log_block_size);
		
		if (*basep >= f->f_inode.i_size)
		  return -E_UNSPECIFIED;//-E_NOT_FOUND;

		file_blockno1 = get_file_block(object, f, *basep);

		//handle overlap case:
		if (blockno != INVALID_BLOCK){
		      dirblock1 = CALL(info->ubd, read_block, file_blockno1, 1);
    		      if (blockno < (num_file_blocks - 1) && (*basep % (EXT2_BLOCK_SIZE) + sizeof(EXT2_Dir_entry_t)) > EXT2_BLOCK_SIZE){
			     file_blockno2 = get_file_block(object, f, (*basep)+ sizeof(EXT2_Dir_entry_t));
			     dirblock2 = CALL(info->ubd, read_block, file_blockno2, 1);
			     if(!dirblock1 && !dirblock2)
				return -E_UNSPECIFIED;//-E_NOT_FOUND;-E_NOT_FOUND;

			     uint32_t start = (*basep % EXT2_BLOCK_SIZE);
			     uint32_t block1_len = EXT2_BLOCK_SIZE - (*basep % EXT2_BLOCK_SIZE);
			     uint32_t block2_len = sizeof(EXT2_Dir_entry_t) - block1_len;

			     /* copy each part from each block into the dir entry */
			     memcpy(&dirfile, dirblock1->ddesc->data + start, block1_len);
			     memcpy((uint8_t*)(&dirfile) + block1_len, dirblock2->ddesc->data, block2_len);

		      }
		      else
		      {
			     if (!dirblock1)
			           return -E_NOT_FOUND;

			     uint32_t len_to_copy;
			     /* calc the amount to copy, in case the dir entry is near the end of the file*/
			     if ((*basep % EXT2_BLOCK_SIZE) + sizeof(EXT2_Dir_entry_t) > EXT2_BLOCK_SIZE)
				len_to_copy = sizeof(EXT2_Dir_entry_t) - ((*basep % EXT2_BLOCK_SIZE) + sizeof(EXT2_Dir_entry_t) - EXT2_BLOCK_SIZE);
			     else
				len_to_copy = sizeof(EXT2_Dir_entry_t);
			     memcpy(&dirfile,  (EXT2_Dir_entry_t *) (dirblock1->ddesc->data + (*basep % EXT2_BLOCK_SIZE)), len_to_copy);
		      }
		}
 
		r = fill_dirent(info, &dirfile, dirfile.inode, entry, size, basep);
	} while (r > 0);
	return r;
}

static int ext2_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_append_file_block\n");
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ext2_fdesc * f = (struct ext2_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(info, f->file);
	bdesc_t * indirect = NULL, * dirblock = NULL;
	int r, offset;

	if (!head || nblocks >= EXT2_NINDIRECT || nblocks < 0)
		return -E_INVAL;

	if (nblocks > EXT2_NDIRECT) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return -E_NO_DISK;

		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		return CALL(info->ubd, write_block, indirect);
	}
	else if (nblocks == EXT2_NDIRECT) {
		uint32_t inumber = ext2_allocate_block(object, NULL, 0, head);
		chdesc_t * temp_head = *head;
		bdesc_t * indirect;
		if (inumber == INVALID_BLOCK)
			return -E_NO_DISK;
		indirect = ext2_lookup_block(object, inumber);

		// Initialize the new indirect block
		if ((r = chdesc_create_init(indirect, info->ubd, &temp_head)) < 0)
			return r;

		// Initialize the structure, then point to it
		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_NO_DISK;

		// this head is from ext2_allocate_block() above
		offset = nblocks * sizeof(uint32_t);
		if ((r = chdesc_create_byte(indirect, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		offset = f->index;
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &inumber, head)) < 0)
			return r;

		// FIXME handle the return values better? 
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
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_direct[nblocks];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &block, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_direct[nblocks] = block;
		return 0;
	}
*/
	return 0;
}

static fdesc_t * ext2_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_allocate_name %s\n", name);
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	EXT2_File_t *dir = NULL, *f = NULL;
	struct ext2_fdesc * dir_fdesc = NULL;
	EXT2_File_t temp_file;
	struct ext2_fdesc * new_fdesc;
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
			type = EXT2_TYPE_FILE;
			break;
		case TYPE_DIR:
			type = EXT2_TYPE_DIR;
			break;
		default:
			return NULL;
	}

	new_fdesc = malloc(sizeof(struct ext2_fdesc));
	if (!new_fdesc)
		return NULL;
	new_fdesc->common = &new_fdesc->base;
	new_fdesc->base.parent = INODE_NONE;

	pdir_fdesc = ext2_lookup_inode(object, parent);
	if (!pdir_fdesc)
		goto allocate_name_exit;

	// Modified dir_alloc_file() from JOS
	nblock = get_file_numblocks(info, ((struct ext2_fdesc *) pdir_fdesc)->file);

	// Search existing blocks for empty spot
	for (i = 0; i < nblock; i++) {
		int j;
		number = get_file_block(object, ((struct ext2_fdesc *) pdir_fdesc)->file, i * EXT2_BLKSIZE);
		if (number != INVALID_BLOCK)
			blk = ext2_lookup_block(object, number);
		else
			blk = NULL;
		if (!blk)
			goto allocate_name_exit2;

		f = (EXT2_File_t *) blk->ddesc->data;
		// Search for an empty slot
		for (j = 0; j < EXT2_BLKFILES; j++) {
			if (!f[j].f_name[0]) {
				memset(&temp_file, 0, sizeof(EXT2_File_t));
				strcpy(temp_file.f_name, name);
				temp_file.f_type = type;

				offset = j * sizeof(EXT2_File_t);
				if ((r = chdesc_create_byte(blk, info->ubd, offset, sizeof(EXT2_File_t), &temp_file, head)) < 0) 
					goto allocate_name_exit2;

				r = CALL(info->ubd, write_block, blk);
				if (r < 0)
					goto allocate_name_exit2;

				new_fdesc->file = malloc(sizeof(EXT2_File_t));
				assert(new_fdesc->file); // TODO: handle error
				memcpy(new_fdesc->file, &temp_file, sizeof(EXT2_File_t));
				new_fdesc->dirb = blk->number;
				new_fdesc->index = j * sizeof(EXT2_File_t);
				new_fdesc->ino = blk->number * EXT2_BLKFILES + j;
				ext2_free_fdesc(object, pdir_fdesc);
				*newino = new_fdesc->ino;
				return (fdesc_t *) new_fdesc;
			}
		}
		blk = NULL;
	}

	// No empty slots, gotta allocate a new block
	number = ext2_allocate_block(object, NULL, 0, head);
	if (number != INVALID_BLOCK)
		blk = ext2_lookup_block(object, number);
	else
		blk = NULL;
	if (!blk)
		goto allocate_name_exit2;
	temp_head = *head;
	if (chdesc_create_init(blk, info->ubd, &temp_head) < 0)
		goto allocate_name_exit3;

	// TODO: change the order of the chdescs in this function! we "lose" the metadata update in the eventual head... 
	dir_fdesc = (struct ext2_fdesc *) ext2_lookup_inode(object, parent);
	assert(dir_fdesc && dir_fdesc->file);
	dir = dir_fdesc->file;
	updated_size = dir->f_size + EXT2_BLKSIZE;
	r = ext2_set_metadata(object, (struct ext2_fdesc *) pdir_fdesc, KFS_feature_size.id, sizeof(uint32_t), &updated_size, &temp_head);
	ext2_free_fdesc(object, (fdesc_t *) dir_fdesc);
	dir_fdesc = NULL;
	dir = NULL;
	if (r < 0)
		goto allocate_name_exit3;

	memset(&temp_file, 0, sizeof(EXT2_File_t));
	strcpy(temp_file.f_name, name);
	temp_file.f_type = type;

	if (chdesc_create_byte(blk, info->ubd, 0, sizeof(EXT2_File_t), &temp_file, head) < 0)
		goto allocate_name_exit3;

	if ((r = CALL(info->ubd, write_block, blk)) < 0)
		goto allocate_name_exit3;
		
	if (ext2_append_file_block(object, pdir_fdesc, number, head) >= 0) {
		new_fdesc->file = malloc(sizeof(EXT2_File_t));
		memcpy(new_fdesc->file, &temp_file, sizeof(EXT2_File_t));
		new_fdesc->dirb = blk->number;
		new_fdesc->index = 0;
		new_fdesc->ino = blk->number * EXT2_BLKFILES;
		*newino = new_fdesc->ino;
		ext2_free_fdesc(object, pdir_fdesc);
		return (fdesc_t *) new_fdesc;
	}

allocate_name_exit3:
	ext2_free_block(object, NULL, number, head);
allocate_name_exit2:
	ext2_free_fdesc(object, pdir_fdesc);
allocate_name_exit:
	free(new_fdesc);
	return NULL;
*/
	return 0;
}

static int empty_get_metadata(void * arg, uint32_t id, size_t size, void * data)
{
	return -E_NOT_FOUND;
}

static int ext2_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_rename\n");
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	fdesc_t * oldfdesc;
	fdesc_t * newfdesc;
	struct ext2_fdesc * old;
	struct ext2_fdesc * new;
	EXT2_File_t * oldfile;
	EXT2_File_t temp_file;
	bdesc_t * dirblock = NULL;
	int i, r, offset;
	uint8_t filetype;
	inode_t inode;
	inode_t not_used;
	metadata_set_t emptymd = { .get = empty_get_metadata, .arg = NULL };

	if (!head)
		return -E_INVAL;

	r = ext2_lookup_name(object, oldparent, oldname, &inode);
	if (r)
		return r;

	oldfdesc = ext2_lookup_inode(object, inode);
	if (!oldfdesc)
		return -E_NOT_FOUND;

	old = (struct ext2_fdesc *) oldfdesc;
	dirblock = CALL(info->ubd, read_block, old->dirb, 1);
	if (!dirblock) {
		ext2_free_fdesc(object, oldfdesc);
		return -E_INVAL;
	}

	oldfile = (EXT2_File_t *) (((uint8_t *) dirblock->ddesc->data) + old->index);
	memcpy(&temp_file, oldfile, sizeof(EXT2_File_t));
	ext2_free_fdesc(object, oldfdesc);

	switch (temp_file.f_type)
	{
		case EXT2_TYPE_FILE:
			filetype = TYPE_FILE;
			break;
		case EXT2_TYPE_DIR:
			filetype = TYPE_DIR;
			break;
		default:
			filetype = TYPE_INVAL;
	}

	newfdesc = ext2_allocate_name(object, newparent, newname, filetype, NULL, &emptymd, &not_used, head);
	if (!newfdesc)
		return -E_FILE_EXISTS;

	new = (struct ext2_fdesc *) newfdesc;
	strcpy(temp_file.f_name, new->file->f_name);
	new->file->f_size = temp_file.f_size;
	new->file->f_indirect = temp_file.f_indirect;
	for (i = 0; i < EXT2_NDIRECT; i++)
		new->file->f_direct[i] = temp_file.f_direct[i];

	dirblock = CALL(info->ubd, read_block, new->dirb, 1);
	if (!dirblock) {
		ext2_free_fdesc(object, newfdesc);
		return -E_INVAL;
	}

	offset = new->index;
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(EXT2_File_t), &temp_file, head)) < 0) {
		ext2_free_fdesc(object, newfdesc);
		return r;
	}

	ext2_free_fdesc(object, newfdesc);
	r = CALL(info->ubd, write_block, dirblock);

	if (r < 0)
		return r;

	if (ext2_remove_name(object, oldparent, oldname, head) < 0)
		return ext2_remove_name(object, newparent, newname, head);

	return 0;
*/
	return 0;
}

static uint32_t ext2_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_truncate_file_block\n");
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct ext2_fdesc * f = (struct ext2_fdesc *) file;
	uint32_t nblocks = get_file_numblocks(info, f->file);
	bdesc_t * indirect = NULL, *dirblock = NULL;
	uint32_t blockno, data = 0;
	uint16_t offset;
	int r;

	if (!head || nblocks > EXT2_NINDIRECT || nblocks < 1)
		return INVALID_BLOCK;

	if (nblocks > EXT2_NDIRECT + 1) {
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
	else if (nblocks == EXT2_NDIRECT + 1) {
		indirect = CALL(info->ubd, read_block, f->file->f_indirect, 1);
		if (!indirect)
			return INVALID_BLOCK;

		blockno = *((uint32_t *) (indirect->ddesc->data) + nblocks - 1);

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_indirect;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_indirect = 0;
		r = ext2_free_block(object, NULL, indirect->number, head);

		return blockno;
	}
	else {
		blockno = f->file->f_direct[nblocks - 1];
		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return INVALID_BLOCK;

		offset = f->index;
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_direct[nblocks - 1];
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &data, head)) < 0)
			return INVALID_BLOCK;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return INVALID_BLOCK;

		f->file->f_direct[nblocks - 1] = 0;

		return blockno;
	}
*/
	return 0;
}

static int ext2_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_free_block\n");
//	return write_bitmap(object, block, 1, head);
	return 0;
}

static int ext2_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_remove_name %s\n", name);
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	
	fdesc_t * file;
	bdesc_t * dirblock = NULL;
	struct ext2_fdesc * f;
	int r;
	uint16_t offset;
	uint8_t data = 0;
	inode_t inode;

	if (!head)
		return -E_INVAL;

	r = ext2_lookup_name(object, parent, name, &inode);
	if (r)
		return r;

	file = ext2_lookup_inode(object, inode);
	if (!file)
		return -E_INVAL;

	f = (struct ext2_fdesc *) file;

	dirblock = CALL(info->ubd, read_block, f->dirb, 1);
	if (!dirblock) {
		r = -E_NO_DISK;
		goto remove_name_exit;
	}

	offset = f->index;
	offset += (uint32_t) &((EXT2_File_t *) NULL)->f_name[0];
	if ((r = chdesc_create_byte(dirblock, info->ubd, offset, 1, &data, head)) < 0)
		goto remove_name_exit;

	r = CALL(info->ubd, write_block, dirblock);

	if (r >= 0)
		f->file->f_name[0] = '\0';

remove_name_exit:
	ext2_free_fdesc(object, file);
	return r;
*/
	return 0;
}

static int ext2_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_write_block\n");
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (!head)
		return -E_INVAL;

	// XXX: with blockman, I don't think this can happen anymore... 
	if (info->bitmap_cache && info->bitmap_cache->number == block->number)
		bdesc_release(&info->bitmap_cache);

	return CALL(info->ubd, write_block, block);
*/
	return 0;
}

static const feature_t * ext2_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_freespace, &KFS_feature_file_lfs, &KFS_feature_blocksize, &KFS_feature_devicesize, &KFS_feature_mtime, &KFS_feature_atime};

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
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);

	if (id == KFS_feature_size.id) {
		if (!f)
			return -E_INVAL;

		if (size < sizeof(int32_t))
			return -E_NO_MEM;
		size = sizeof(int32_t);

		*((int32_t *) data) = f->f_inode.i_size;
	}
	else if (id == KFS_feature_filetype.id) {
		if (!f)
			return -E_INVAL;

		if (size < sizeof(uint32_t))
			return -E_NO_MEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = f->f_type;
	}
	else if (id == KFS_feature_freespace.id) {
		if (size < sizeof(uint32_t))
			return -E_NO_MEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = count_free_space(object);
	}
	else if (id == KFS_feature_file_lfs.id) {
		if (size < sizeof(object))
			return -E_NO_MEM;
		size = sizeof(object);

		*((typeof(object) *) data) = object;
	}
	else if (id == KFS_feature_blocksize.id) {
		if (size < sizeof(uint32_t))
			return -E_NO_MEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = ext2_get_blocksize(object);
	}
	else if (id == KFS_feature_devicesize.id) {
		if (size < sizeof(uint32_t))
			return -E_NO_MEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = info->super->s_blocks_count;
	}
	else if (id == KFS_feature_mtime.id || id == KFS_feature_atime.id) {
		if (!f)
			return -E_INVAL;

		if (size < sizeof(uint32_t))
			return -E_NO_MEM;
		size = sizeof(uint32_t);

		if (id == KFS_feature_mtime.id)
			*((uint32_t *) data) = f->f_inode.i_mtime;
		else
			*((uint32_t *) data) = f->f_inode.i_atime;
	}
	else
		return -E_INVAL;

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

static int ext2_set_metadata(LFS_t * object, struct ext2_fdesc * f, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	Dprintf("EXT2DEBUG: ext2_set_metadata %u, %u\n", id, size);
/*	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * dirblock = NULL;
	int r;
	uint16_t offset;

	if (!head)
		return -E_INVAL;

	if (id == KFS_feature_size.id) {
		if (sizeof(int32_t) != size || *((int32_t *) data) < 0 || *((int32_t *) data) >= EXT2_MAXFILESIZE)
			return -E_INVAL;

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_INVAL;

		offset = f->index;
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_size;
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
				fs_type = EXT2_TYPE_FILE;
				break;
			case TYPE_DIR:
				fs_type = EXT2_TYPE_DIR;
				break;
			default:
				return -E_INVAL;
		}

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_INVAL;

		offset = f->index;
		offset += (uint32_t) &((EXT2_File_t *) NULL)->f_type;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), &fs_type, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);

		if (r < 0)
			return r;

		f->file->f_type = fs_type;
		return 0;
	}
	else if (id == KFS_feature_mtime.id || id == KFS_feature_atime.id) {
		if (sizeof(uint32_t) != size)
			return -E_INVAL;

		dirblock = CALL(info->ubd, read_block, f->dirb, 1);
		if (!dirblock)
			return -E_INVAL;

		offset = f->index;
		if (id == KFS_feature_mtime.id)
			offset += (uint32_t) &((EXT2_File_t *) NULL)->f_mtime;
		else
			offset += (uint32_t) &((EXT2_File_t *) NULL)->f_atime;
		if ((r = chdesc_create_byte(dirblock, info->ubd, offset, sizeof(uint32_t), data, head)) < 0)
			return r;

		r = CALL(info->ubd, write_block, dirblock);
		if (r < 0)
			return r;

		f->file->f_mtime = *((uint32_t *) data);
		return 0;
	}

	return -E_INVAL;
*/
	return 0;
}

static int ext2_set_metadata_inode(LFS_t * object, inode_t ino, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
/*	int r;
	struct ext2_fdesc * f = (struct ext2_fdesc *) ext2_lookup_inode(object, ino);
	if (!f)
		return -E_INVAL;
	r = ext2_set_metadata(object, f, id, size, data, head);
	ext2_free_fdesc(object, (fdesc_t *) f);
	return r;
*/
	return 0;
}

static int ext2_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head)
{
	struct ext2_fdesc * f = (struct ext2_fdesc *) file;
	return ext2_set_metadata(object, f, id, size, data, head);
}

static int ext2_destroy(LFS_t * lfs)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(lfs);
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);

	//bdesc_release(&info->super_block);
	//bdesc_release(&info->bitmap_cache);
	
	free(info->super);
	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

/*
 *  Reads group descriptor of inode number ino and sets inode to that inode
 */

static int ext2_get_inode(lfs_info_t * info, inode_t ino, EXT2_inode_t * inode)
{
	uint32_t block_group, bitoffset, block;
	int r;
	bdesc_t * bdesc;

	if((ino != EXT2_ROOT_INO && ino < info->super->s_first_ino)
		       	|| ino > info->super->s_inodes_count)
		return -E_INVAL;
	
	//Get the group the inode belongs in
	EXT2_group_desc_t gdesc;
	block_group = (ino - 1) / info->super->s_inodes_per_group;
	r = ext2_get_group_desc(info, block_group, &gdesc); 
	if(r < 0)
		return r;
	
	
	bitoffset = ((ino - 1) % info->super->s_inodes_per_group) * info->super->s_inode_size;
	block = gdesc.bg_inode_table + (bitoffset >> (10 + info->super->s_log_block_size));
	bdesc = CALL(info->ubd, read_block, block, 1);
	if(!bdesc)
		return -E_INVAL;
	bitoffset &= ((1024 << info->super->s_log_block_size) - 1);
	memcpy(inode, (bdesc->ddesc->data + bitoffset ), sizeof(EXT2_inode_t));
	if(!inode)
		return -E_NOT_FOUND;
	else
		return ino;
}


//TODO Make this pretty and better
static uint8_t ext2_to_kfs_type(uint16_t type)
{

	switch(type & S_IFMT) {
		case(S_IFDIR):
			return TYPE_DIR;
		case(S_IFREG):
			return TYPE_FILE;
		case(S_IFLNK):
			return TYPE_SYMLINK;	
		default:
			return TYPE_INVAL;
	}

}

//ext2_get_group_desc(object, block_group, gdesc) 
//	This function sets gdesc to the group descriptor at block block_group.
//	returns < 0 on error
static int ext2_get_group_desc(lfs_info_t * info, uint32_t block_group, EXT2_group_desc_t * gdesc) {
	
	uint32_t blockoff, byteoff;
	
	if(!info || !gdesc)
		return -E_INVAL;

	if(block_group > (info->super->s_blocks_count / info->super->s_blocks_per_group) ||
			block_group < 0)
		return -E_INVAL;

	//TODO do some sanity checks on block_group
	//TODO if blocksize is not 4K things go bad
	blockoff = (block_group / EXT2_DESC_PER_BLOCK) + 1;
	byteoff = (block_group * sizeof(EXT2_group_desc_t) % EXT2_DESC_PER_BLOCK) ;

	bdesc_t * group;
	group = CALL(info->ubd, read_block, blockoff, 1);
	if(!group)
		return -E_NOT_FOUND;
	if (memcpy(gdesc, group->ddesc->data + byteoff, sizeof(EXT2_group_desc_t)) == NULL)
		return -E_NOT_FOUND;
	
	return block_group;
}


void test_ext2_get_file_block(LFS_t * lfs)
{
	ext2_fdesc_t * fd;
	assert(fd = (ext2_fdesc_t *)ext2_lookup_inode(lfs, 50243));
	
	get_file_block(lfs, fd, 0);
	get_file_block(lfs, fd, 45056);
	get_file_block(lfs, fd, 49152);
	get_file_block(lfs, fd, 131072);
	get_file_block(lfs, fd, 176128);
	get_file_block(lfs, fd, 180224);
	get_file_block(lfs, fd, 626688);
	get_file_block(lfs, fd, 4370432);
	free(fd);
}


LFS_t * ext2(BD_t * block_device)
{
	Dprintf("EXT2DEBUG: %s\n", __FUNCTION__);
	
	if (!block_device)
		return NULL;
	
	struct lfs_info * info;
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

	//Load the Super Block into memory
	info->super = malloc(sizeof(struct EXT2_Super));
	if(!info->super) {
		free(info);
		free(lfs);
		return NULL;
	}

	info->super_block = CALL(info->ubd, read_block, 0, 1);
	if (!info->super_block)
	{
		printf("Unable to read superblock!\n");
		return NULL;
	}
	//TODO Check return value of memcpy
	memcpy(info->super, info->super_block->ddesc->data + 1024, sizeof(struct EXT2_Super));

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

