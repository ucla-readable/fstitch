#include <inc/string.h>
#include <inc/partition.h>

#include "fs.h"

static struct Super* super = NULL;
int diskno = 0;
static uint32_t fs_offset = 0;
uint32_t part_length = 0;

uint32_t* bitmap;			// bitmap blocks mapped in memory

void file_flush(struct File*);
bool block_is_free(uint32_t);

// Return the virtual address of this disk block.
char*
diskaddr(uint32_t blockno)
{
	if (super && blockno >= super->s_nblocks)
		panic("bad block number %08x in diskaddr", blockno);
	return (char*) (DISKMAP + blockno*BLKSIZE);
}

// Is this virtual address mapped?
bool
va_is_mapped(char* va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

// Is this disk block mapped?
bool
block_is_mapped(uint32_t blockno)
{
	char* va = diskaddr(blockno);
	return va_is_mapped(va) && va != 0;
}

// Is this virtual address dirty?
bool
va_is_dirty(char* va)
{
	return (vpt[VPN(va)] & PTE_D) != 0;
}

// Is this block dirty?
bool
block_is_dirty(uint32_t blockno)
{
	char* va = diskaddr(blockno);
	return va_is_mapped(va) && va_is_dirty(va);
}

// Allocate a page to hold the disk block
int
map_block(uint32_t blockno)
{
	if (block_is_mapped(blockno))
		return 0;
	return sys_page_alloc(0, diskaddr(blockno), PTE_U|PTE_W|PTE_P);
}

// Make sure a particular disk block is loaded into memory.
// Returns 0 on success, or a negative error code on error.
// 
// If blk != 0, set *blk to the address of the block in memory.
//
// Hint: Use diskaddr, block_is_mapped, sys_page_alloc, and ide_read.
// Hint: If you loaded the block from disk, use sys_page_map to clear the
// corresponding page's PTE_D bit.  (This is an optimization.)
static int
read_block(uint32_t blockno, char** blk)
{
	char * addr = diskaddr(blockno);
	uint32_t sector;
	int r;

	if (super && blockno >= super->s_nblocks)
		panic("reading non-existent block %08x", blockno);

	if (bitmap && block_is_free(blockno))
		panic("reading free block %08x", blockno);

	if(block_is_mapped(blockno))
	{
		if(blk)
			*blk = addr;
		return 0;
	}
	
	r = sys_page_alloc(0, addr, PTE_U | PTE_W | PTE_P);
	if(r)
		return r;
	
	sector = blockno * BLKSECTS;
	if(sector >= part_length && part_length)
		panic("reading sector %08x past end of partition", sector);
	ide_read(diskno, sector + fs_offset, addr, BLKSECTS);
	
	sys_page_map(0, addr, 0, addr, PTE_U | PTE_W | PTE_P);
	
	if(blk)
		*blk = addr;
	
	return 0;
}

// Copy the current contents of the block out to disk.
// Then clear the PTE_D bit using sys_page_map.
// Hint: Use ide_write.
// Hint: Use the PTE_USER constant when calling sys_page_map.
void
write_block(uint32_t blockno)
{
	char * addr = diskaddr(blockno);
	uint32_t sector;

	if (!block_is_mapped(blockno))
		panic("write unmapped block %08x", blockno);
	
	// Write the disk block and clear PTE_D.
	if(vpt[VPN(addr)] & PTE_D)
	{
		sector = blockno * BLKSECTS;
		if(sector >= part_length && part_length)
			panic("writing sector %08x past end of partition", sector);
		ide_write(diskno, sector + fs_offset, addr, BLKSECTS);
		/* this can't fail */
		sys_page_map(0, addr, 0, addr, PTE_U | PTE_W | PTE_P);
	}
}

// Make sure this block is unmapped.
void
unmap_block(uint32_t blockno)
{
	int r;

	if (!block_is_mapped(blockno))
		return;

	assert(block_is_free(blockno) || !block_is_dirty(blockno));

	if ((r = sys_page_unmap(0, diskaddr(blockno))) < 0)
		panic("unmap_block: sys_mem_unmap: %e", r);
	assert(!block_is_mapped(blockno));
}

// Check to see if the block bitmap indicates that block 'blockno' is free.
// Return 1 if the block is free, 0 if not.
bool
block_is_free(uint32_t blockno)
{
	if (super == 0 || blockno >= super->s_nblocks)
		return 0;
	if (bitmap[blockno / 32] & (1 << (blockno % 32)))
		return 1;
	return 0;
}

// Mark a block free in the bitmap
void
free_block(uint32_t blockno)
{
	// Blockno zero is the null pointer of block numbers.
	if (blockno == 0)
		panic("attempt to free zero block");
	bitmap[blockno/32] |= 1<<(blockno%32);
}

// Search the bitmap for a free block and allocate it.
// 
// Return block number allocated on success,
// -E_NO_DISK if we are out of blocks.
int
alloc_block_num(void)
{
	uint32_t i;
	
	/* optimization/safety feature: never allocate the superblock or bitmap blocks */
	for(i = 2 + ((super->s_nblocks + BLKBITSIZE - 1) / BLKBITSIZE); i < super->s_nblocks; i++)
		if(block_is_free(i))
		{
			bitmap[i / 32] &= ~(1 << (i % 32));
			/* flush the bitmap immediately since we allocated a block */
			write_block(2 + i / BLKBITSIZE);
			return i;
		}
	
	return -E_NO_DISK;
}

// Allocate a block -- first find a free block in the bitmap,
// then map it into memory.
int
alloc_block(void)
{
	int r, bno;

	if ((r = alloc_block_num()) < 0)
		return r;
	bno = r;

	if ((r = map_block(bno)) < 0) {
		free_block(bno);
		return r;
	}
	/* clear it out */
	write_block(bno);
	return bno;
}

// Read and validate the file system super-block.
static int
read_super(void)
{
	int r;
	char* blk;

	if ((r = read_block(1, &blk)) < 0)
	{
		printf("Disk %d: cannot read superblock: %e\n", diskno, r);
		return r;
	}

	super = (struct Super*) blk;
	if (super->s_magic != FS_MAGIC)
	{
		printf("Disk %d: bad file system magic number\n", diskno);
		super = NULL;
		unmap_block(1);
		return -1;
	}

	printf("Filesystem size: %d blocks (%dMB)\n", super->s_nblocks, super->s_nblocks / (1024 * 1024 / BLKSIZE));
	if (super->s_nblocks > DISKSIZE/BLKSIZE)
	{
		printf("Disk %d: file system is too large\n", diskno);
		super = NULL;
		unmap_block(1);
		return -1;
	}

	//printf("superblock is good\n");
	return 0;
}

// Read and validate the file system bitmap.
//
// Read all the bitmap blocks into memory.
// Set the "bitmap" pointer to point at the beginning of the first
// bitmap block.
// 
// Check that all reserved blocks -- 0, 1, and the bitmap blocks themselves --
// are all marked as in-use
// (for each block i, assert(!block_is_free(i))).
//
// Hint: Assume that the superblock has already been loaded into
// memory (in variable 'super').  Check out super->s_nblocks.
void
read_bitmap(void)
{
	int r;
	uint32_t i;

	// Read the bitmap into memory.
	// The bitmap consists of one or more blocks.  A single bitmap block
	// contains the in-use bits for BLKBITSIZE blocks.  There are
	// super->s_nblocks blocks in the disk altogether.
	// Set 'bitmap' to point to the first address in the bitmap.
	// Hint: Use read_block.

	/* number of blocks of bitmaps */
	i = (super->s_nblocks + BLKBITSIZE - 1) / BLKBITSIZE;
	while(i--)
	{
		r = read_block(2 + i, NULL);
		if(r)
			panic("read_bitmap: %e", r);
	}
	bitmap = (uint32_t *) diskaddr(2);

	// Make sure the reserved and root blocks are marked in-use.
	assert(!block_is_free(0));
	assert(!block_is_free(1));
	assert(bitmap);

	// Make sure that the bitmap blocks are marked in-use.
	i = (super->s_nblocks + BLKBITSIZE - 1) / BLKBITSIZE;
	while(i--)
		assert(!block_is_free(i + 2));

	//printf("read_bitmap is good\n");
}

// Test that write_block works, by smashing the superblock and reading it back.
void
check_write_block(void)
{
	super = 0;

	// back up super block
	read_block(0, 0);
	memcpy(diskaddr(0), diskaddr(1), PGSIZE);

	// smash it 
	strcpy(diskaddr(1), "OOPS!\n");
	write_block(1);
	assert(block_is_mapped(1));
	assert(!va_is_dirty(diskaddr(1)));

	// clear it out
	sys_page_unmap(0, diskaddr(1));
	assert(!block_is_mapped(1));

	// read it back in
	read_block(1, 0);
	assert(strcmp(diskaddr(1), "OOPS!\n") == 0);

	// fix it
	memcpy(diskaddr(1), diskaddr(0), PGSIZE);
	write_block(1);
	super = (struct Super*)diskaddr(1);

	//printf("write_block is good\n");
}

/* find the first KudOS partition, or return 0 if none found */
uint32_t
find_kudos(uint8_t * buffer, uint32_t table_offset, uint32_t ext_offset)
{
	int i;
	struct pc_ptable * ptable = (struct pc_ptable *) (buffer + PTABLE_OFFSET);
	
	ide_read(diskno, table_offset, buffer, 1);
	/* first scan for a KudOS partition */
	for(i = 0; i != 4; i++)
		if(ptable[i].type == PTABLE_KUDOS_TYPE)
		{
			part_length = ptable[i].lba_length;
			return table_offset + ptable[i].lba_start;
		}
	/* then look inside the first extended partition */
	for(i = 0; i != 4; i++)
		if(ptable[i].type == PTABLE_DOS_EXT_TYPE ||
		   ptable[i].type == PTABLE_W95_EXT_TYPE ||
		   ptable[i].type == PTABLE_LINUX_EXT_TYPE)
			return find_kudos(buffer, ext_offset + ptable[i].lba_start, ext_offset ? ext_offset : ptable[i].lba_start);
	/* nothing here */
	return 0;
}

// Initialize the file system
void
fs_init(void)
{
	uint8_t buffer[512];
	
	static_assert(sizeof(struct File) == 256);

	for(diskno = 0; diskno < 2; diskno++)
	{
		printf("FS: Trying disk %d...\n", diskno);
		
		/* "no partition, allow whole disk" */
		part_length = 0;
		
		/* find the partition */
		fs_offset = find_kudos(buffer, 0, 0);
		
		printf("FS: Disk offset: %d\n", fs_offset);
		
		if(!read_super())
			break;
	}
	if(diskno == 2)
		panic("no valid filesystems found");
	printf("FS: Using filesystem on disk %d\n", diskno);

	check_write_block();
	read_bitmap();
}

// Find the disk block number slot for the 'filebno'th block in file 'f'.
// Set '*ppdiskbno' to point to that slot.
// The slot will be one of the f->f_direct[] entries,
// or an entry in the indirect block.
// When 'alloc' is set, this function will allocate an indirect block
// if necessary.
//
// Returns:
//	0 on success (but note that *ppdiskbno might equal 0).
//	-E_NOT_FOUND if the function needed to allocate an indirect block, but
//		alloc was 0.
//	-E_NO_DISK if there's no space on the disk for an indirect block.
//	-E_NO_MEM if there's no space in memory for an indirect block.
//	-E_INVAL if filebno is out of range (it's >= NINDIRECT).
//
// Analogy: This is like pgdir_walk for files.  
int
file_block_walk(struct File* f, uint32_t filebno, uint32_t** ppdiskbno, bool alloc)
{
	int r;
	uint32_t* ptr;
	char* blk;

	if (filebno < NDIRECT)
		ptr = &f->f_direct[filebno];
	else if (filebno < NINDIRECT) {
		bool indirect_alloced = 0;
		if (f->f_indirect == 0) {
			if (alloc == 0)
				return -E_NOT_FOUND;
			if ((r = alloc_block()) < 0)
				return r;
			f->f_indirect = r;
			indirect_alloced = 1;
		}
		if ((r = read_block(f->f_indirect, &blk)) < 0)
			return r;
		assert(blk != 0);
		if (indirect_alloced)
			memset(blk, 0, BLKSIZE);
		ptr = (uint32_t*)blk + filebno;
	} else
		return -E_INVAL;

	*ppdiskbno = ptr;
	return 0;
}

// Set '*diskbno' to the disk block number for the 'filebno'th block
// in file 'f'.
// If 'alloc' is set and the block does not exist, allocate it.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_NOT_FOUND if alloc was 0 but the block did not exist.
//	-E_NO_DISK if a block needed to be allocated but the disk is full.
//	-E_NO_MEM if we're out of memory.
//	-E_INVAL if filebno is out of range.
int
file_map_block(struct File* f, uint32_t filebno, uint32_t* diskbno, bool alloc)
{
	int r;
	uint32_t* ptr;

	if ((r = file_block_walk(f, filebno, &ptr, alloc)) < 0)
		return r;
	if (*ptr == 0) {
		if (alloc == 0)
			return -E_NOT_FOUND;
		if ((r = alloc_block()) < 0)
			return r;
		*ptr = r;
	}
	*diskbno = *ptr;
	return 0;
}

// Remove a block from file f.  If it's not there, just silently succeed.
// Returns 0 on success, < 0 on error.
int
file_clear_block(struct File* f, uint32_t filebno)
{
	int r;
	uint32_t *ptr;

	if ((r = file_block_walk(f, filebno, &ptr, 0)) < 0)
		return r;
	if (*ptr) {
		free_block(*ptr);
		unmap_block(*ptr);
		*ptr = 0;
	}
	return 0;
}

// Set *blk to point at the filebno'th block in file 'f'.
// Allocate the block if it doesn't yet exist.
// Returns 0 on success, < 0 on error.
int
file_get_block(struct File* f, uint32_t filebno, char** blk)
{
	int r;
	uint32_t diskbno;

	// Read in the block, leaving the pointer in *blk.
	// Hint: Use file_map_block and read_block.
	r = file_map_block(f, filebno, &diskbno, 1);
	if(r)
		return r;
	
	r = read_block(diskbno, blk);
	if(r)
		/* don't need to undo file_map_block() */
		return r;
	
	return 0;
}

// Mark the offset/BLKSIZE'th block dirty in file f
// by writing its first word to itself.  
int
file_dirty(struct File* f, off_t offset)
{
	int r;
	char* blk;

	if ((r = file_get_block(f, offset/BLKSIZE, &blk)) < 0)
		return r;
	*(volatile char*)blk = *(volatile char*)blk;
	return 0;
}

// Try to find a file named "name" in dir.  If so, set *file to it.
int
dir_lookup(struct File* dir, const char* name, struct File** file)
{
	int r;
	uint32_t i, j, nblock;
	char* blk;
	struct File* f;

	// Search dir for name.
	// We maintain the invariant that the size of a directory-file
	// is always a multiple of the file system's block size.
	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (strcmp(f[j].f_name, name) == 0) {
				*file = &f[j];
				f[j].f_dir = dir;
				return 0;
			}
	}
	return -E_NOT_FOUND;
}

// Set *file to point at a free File structure in dir.
int
dir_alloc_file(struct File* dir, struct File** file)
{
	int r;
	uint32_t nblock, i, j;
	char* blk;
	struct File* f;

	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (f[j].f_name[0] == '\0') {
				*file = &f[j];
				f[j].f_dir = dir;
				return 0;
			}
	}
	dir->f_size += BLKSIZE;
	if ((r = file_get_block(dir, i, &blk)) < 0)
		return r;
	f = (struct File*) blk;
	*file = &f[0];
	f[0].f_dir = dir;
	return 0;
}

// Skip over slashes.
static inline const char*
skip_slash(const char* p)
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
static int
walk_path(const char* path, struct File** pdir, struct File** pfile, char* lastelem)
{
	const char* p;
	char name[MAXNAMELEN];
	struct File *dir, *file;
	int r;

	// if (*path != '/')
	//	return -E_BAD_PATH;
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
		if (path - p >= MAXNAMELEN)
			return -E_BAD_PATH;
		memcpy(name, p, path - p);
		name[path - p] = '\0';
		path = skip_slash(path);

		if (dir->f_type != FTYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(dir, name, &file)) < 0) {
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

// Create "path".  On success set *file to point at the file and return 0.
// On error return < 0.
int
file_create(const char* path, struct File** file)
{
	char name[MAXNAMELEN];
	int r;
	struct File *dir, *f;

	if ((r = walk_path(path, &dir, &f, name)) == 0)
		return -E_FILE_EXISTS;
	if (r != -E_NOT_FOUND || dir == 0)
		return r;
	if (dir_alloc_file(dir, &f) < 0)
		return r;
	memset(f, 0, sizeof(*f));
	strcpy(f->f_name, name);
	*file = f;
	return 0;
}

// Open "path".  On success set *pfile to point at the file and return 0.
// On error return < 0.
int
file_open(const char* path, struct File** file, int mode)
{
	// Hint: Use walk_path.
	if(mode & O_CREAT)
	{
		int r = file_create(path, file);
		if(mode & O_MKDIR)
		{
			if(r < 0 && r != -E_FILE_EXISTS)
				return r;
			(*file)->f_type = FTYPE_DIR;
		}
		else
		{
			if(r != -E_FILE_EXISTS)
				return r;
			(*file)->f_type = FTYPE_REG;
		}
	}
	return walk_path(path, NULL, file, NULL);
}

// Remove any blocks currently used by file 'f',
// but not necessary for a file of size 'newsize'.
// For both the old and new sizes, figure out the number of blocks required,
// and then clear the blocks from new_nblocks to old_nblocks.
// If the new_nblocks is no more than NDIRECT, and the indirect block has
// been allocated (f->f_indirect != 0), then free the indirect block too.
// (Remember to clear the f->f_indirect pointer so you'll know
// whether it's valid!)
// Do not change f->f_size.
static void
file_truncate_blocks(struct File* f, off_t newsize)
{
	uint32_t old_nblocks, new_nblocks;

	// Hint: Use file_clear_block and/or free_block.
	old_nblocks = (f->f_size + BLKSIZE - 1) / BLKSIZE;
	new_nblocks = (newsize + BLKSIZE - 1) / BLKSIZE;
	while(old_nblocks > new_nblocks)
		if(file_clear_block(f, --old_nblocks))
			panic("file_clear_block failed; destupify it!");
	if(new_nblocks <= NDIRECT && f->f_indirect)
	{
		free_block(f->f_indirect);
		f->f_indirect = 0;
	}
}

int
file_set_size(struct File* f, off_t newsize)
{
	if (f->f_size > newsize)
		file_truncate_blocks(f, newsize);
	f->f_size = newsize;
	if (f->f_dir)
		file_flush(f->f_dir);
	return 0;
}

// Flush the contents of file f out to disk.
// Loop over all the blocks in file.
// Translate the file block number into a disk block number
// and then check whether that disk block is dirty.  If so, write it out.
//
// Hint: use file_map_block, block_is_dirty, and write_block.
void
file_flush(struct File* f)
{
	int blocks = (f->f_size + BLKSIZE - 1) / BLKSIZE;
	uint32_t i, db;
	for(i = 0; i < blocks && i < NINDIRECT; i++)
	{
		if(file_map_block(f, i, &db, 0))
			continue;
		if(block_is_dirty(db))
			write_block(db);
	}
}

// Sync the entire file system.  A big hammer.
void
fs_sync(void)
{
	int i;
	for (i = 1; i < super->s_nblocks; i++)
		if (block_is_dirty(i))
			write_block(i);
}

uint32_t
fs_get_navail_blocks(void)
{
	uint32_t n_avail_blocks = 0;
	uint32_t blockno;
	for(blockno = 0; blockno < super->s_nblocks; blockno++)
		if(block_is_free(blockno))
			n_avail_blocks++;

	return n_avail_blocks;
}

// Close a file.
void
file_close(struct File *f)
{
	file_flush(f);
	if (f->f_dir)
		file_flush(f->f_dir);
}

// Remove a file by truncating it and then zeroing the name.
int
file_remove(const char* path)
{
	int r;
	struct File *f;

	if ((r = walk_path(path, 0, &f, 0)) < 0)
		return r;

	file_truncate_blocks(f, 0);
	f->f_name[0] = '\0';
	file_flush(f);
	if (f->f_dir)
		file_flush(f->f_dir);

	return 0;
}
