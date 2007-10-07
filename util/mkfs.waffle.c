/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#define _BSD_EXTENSION
#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#include <lib/partition.h>
#include <modules/waffle.h>

static int diskfd;
static off_t diskoff;
static uint32_t nblocks, ninodes;
static uint32_t next_free = 2;

struct block {
	uint32_t busy, used, number;
	uint8_t data[WAFFLE_BLOCK_SIZE];
};

#define CACHE_BLOCKS 64
static struct block cache[CACHE_BLOCKS];

static ssize_t readn(int f, void * av, size_t n)
{
	uint8_t * a;
	size_t t;

	a = av;
	t = 0;
	while(t < n)
	{
		size_t m = read(f, a + t, n - t);
		if(m <= 0)
		{
			if(!t)
				return m;
			break;
		}
		t += m;
	}
	return t;
}

static struct block * get_block(uint32_t number)
{
	int i, least;
	/* implement an LRU cache */
	static uint32_t t = 1;
	struct block * b;
	
	if(!number)
	{
		fprintf(stderr, "Request for reserved block 0\n");
		return NULL;
	}
	
	if(number >= nblocks)
	{
		fprintf(stderr, "Request for block %u past end of disk\n", number);
		return NULL;
	}
	
	least = -1;
	for(i = 0; i < CACHE_BLOCKS; i++)
	{
		if(cache[i].number == number)
		{
			b = &cache[i];
			goto out;
		}
		if(!cache[i].busy && (least == -1 || cache[i].used < cache[least].used))
			least = i;
	}
	
	if(least == -1)
	{
		fprintf(stderr, "panic: block cache full\n");
		return NULL;
	}
	
	b = &cache[least];
	
	if(b->used)
	{
		/* evict block b */
		if(lseek(diskfd, diskoff + b->number * WAFFLE_BLOCK_SIZE, SEEK_SET) < 0 || write(diskfd, b->data, WAFFLE_BLOCK_SIZE) != WAFFLE_BLOCK_SIZE)
		{
			fprintf(stderr, "write block %d: ", b->number);
			perror("");
			return NULL;
		}
	}
	
	if(lseek(diskfd, diskoff + number * WAFFLE_BLOCK_SIZE, SEEK_SET) < 0 || readn(diskfd, b->data, WAFFLE_BLOCK_SIZE) != WAFFLE_BLOCK_SIZE)
	{
		fprintf(stderr, "read block %d: ", number);
		perror("");
		return NULL;
	}
	b->number = number;
	
out:
	b->busy++;
	/* update last used time */
	b->used = ++t;
	if(!t)
	{
		fprintf(stderr, "panic: too many block reads\n");
		return NULL;
	}
	return b;
}

static void put_block(struct block * b)
{
	b->busy--;
}

#if BYTE_ORDER == LITTLE_ENDIAN
/* check for a partition table and use the first JOSFS/WAFFLE partition if there is one */
static void partition_adjust(uint64_t * size)
{
	unsigned char mbr[512];
	struct pc_ptable * ptable;
	int i = read(diskfd, mbr, 512);
	if(i != 512)
		return;
	if(mbr[PTABLE_MAGIC_OFFSET] != PTABLE_MAGIC[0] ||
	   mbr[PTABLE_MAGIC_OFFSET + 1] != PTABLE_MAGIC[1])
		return;
	ptable = (struct pc_ptable *) &mbr[PTABLE_OFFSET];
	for(i = 0; i < 4; i++)
		if(ptable[i].type == PTABLE_JOS_TYPE)
			break;
	if(i == 4)
		return;
	printf("Using JOSFS/WAFFLE partition %d, sector offset %d, size %d (%d blocks)\n", i + 1,
	       ptable[i].lba_start, ptable[i].lba_length, ptable[i].lba_length / (WAFFLE_BLOCK_SIZE / 512));
	diskoff = ptable[i].lba_start << 9;
	*size = ptable[i].lba_length << 9;
}
#endif

/* open the disk, check the superblock, and check the block bitmap for sanity */
static int open_disk(const char * name, int use_ptable)
{
	struct stat s;
	uint64_t size;
	
	if((diskfd = open(name, O_RDWR)) < 0)
	{
		perror(name);
		return -1;
	}
	
	if(fstat(diskfd, &s) < 0)
	{
		perror(name);
		close(diskfd);
		return -1;
	}
	
	if(s.st_mode & S_IFBLK)
	{
		/* it's a block device; stat size will be zero */
		if(ioctl(diskfd, BLKGETSIZE64, &size) < 0)
		{
			perror(name);
			close(diskfd);
			return -1;
		}
	}
	else
		size = s.st_size;
#if BYTE_ORDER == LITTLE_ENDIAN
	/* if requested, and if there is a partition table, use only the JOSFS/WAFFLE partition */
	if(use_ptable)
		partition_adjust(&size);
#endif
	nblocks = size / WAFFLE_BLOCK_SIZE;
	
	/* minimally, we have a reserved block, a superblock, a bitmap
	 * block, an inode block, and a root directory block */
	if(nblocks < 5)
	{
		fprintf(stderr, "Bad disk size (%u blocks)\n", nblocks);
		close(diskfd);
		return -1;
	}
	/* by default, half as many inodes as blocks */
	ninodes = nblocks / 2;
	
	printf("Initializing waffle file system: %u blocks, %u inodes\n", nblocks, ninodes);
	
	return 0;
}

static int init_super(void)
{
	struct waffle_super * super;
	struct block * block = get_block(1);
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	
	memset(block->data, 0, sizeof(block->data));
	super->s_magic = WAFFLE_FS_MAGIC;
	super->s_blocks = nblocks;
	super->s_inodes = ninodes;
	
	put_block(block);
	
	return 0;
}

static int alloc_block(uint32_t * pointer)
{
	struct block * block = get_block(next_free++);
	if(!block)
		return -1;
	memset(block->data, 0, sizeof(block->data));
	*pointer = block->number;
	put_block(block);
	return 0;
}

static int append_block(struct waffle_inode * inode)
{
	uint32_t i_blocks = inode->i_size / WAFFLE_BLOCK_SIZE;
	struct block * dindirect;
	struct block * indirect;
	inode->i_size += WAFFLE_BLOCK_SIZE;
	if(i_blocks < WAFFLE_DIRECT_BLOCKS)
		return alloc_block(&inode->i_direct[i_blocks]);
	if(i_blocks == WAFFLE_DIRECT_BLOCKS)
		if(alloc_block(&inode->i_indirect) < 0)
			return -1;
	if(i_blocks < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect = get_block(inode->i_indirect);
		if(!indirect)
			return -1;
		if(alloc_block(&((uint32_t *) indirect->data)[i_blocks - WAFFLE_DIRECT_BLOCKS]) < 0)
		{
			put_block(indirect);
			return -1;
		}
		put_block(indirect);
		return 0;
	}
	if(i_blocks == WAFFLE_INDIRECT_BLOCKS)
		if(alloc_block(&inode->i_dindirect) < 0)
			return -1;
	dindirect = get_block(inode->i_dindirect);
	if(!dindirect)
		return -1;
	if(!((i_blocks - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS))
	{
		if(alloc_block(&((uint32_t *) dindirect->data)[(i_blocks - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS]) < 0)
		{
			put_block(dindirect);
			return -1;
		}
	}
	indirect = get_block(((uint32_t *) dindirect->data)[(i_blocks - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS]);
	if(!indirect)
	{
		put_block(dindirect);
		return -1;
	}
	if(alloc_block(&((uint32_t *) indirect->data)[(i_blocks - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS]) < 0)
	{
		put_block(indirect);
		put_block(dindirect);
		return -1;
	}
	put_block(indirect);
	put_block(dindirect);
	return 0;
}

static struct block * get_inode_block(struct waffle_inode * inode, uint32_t index)
{
	struct block * block;
	struct block * dindirect;
	struct block * indirect;
	if(index < WAFFLE_DIRECT_BLOCKS)
		return get_block(inode->i_direct[index]);
	if(index < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect = get_block(inode->i_indirect);
		if(!indirect)
			return NULL;
		block = get_block(((uint32_t *) indirect->data)[index - WAFFLE_DIRECT_BLOCKS]);
		put_block(indirect);
		return block;
	}
	dindirect = get_block(inode->i_dindirect);
	if(!dindirect)
		return NULL;
	indirect = get_block(((uint32_t *) dindirect->data)[(index - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS]);
	put_block(dindirect);
	if(!indirect)
		return NULL;
	block = get_block(((uint32_t *) indirect->data)[(index - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS]);
	put_block(indirect);
	return block;
}

static int setup_inode(struct waffle_inode * inode, uint32_t size)
{
	inode->i_mode = WAFFLE_S_IRWXU | WAFFLE_S_IRWXG | WAFFLE_S_IRWXO;
	inode->i_links = 1;
	inode->i_size = 0;
	inode->i_atime = time(NULL);
	inode->i_ctime = inode->i_atime;
	inode->i_mtime = inode->i_atime;
	if(size > WAFFLE_INLINE_SIZE)
	{
		uint32_t i, blocks = (size + WAFFLE_BLOCK_SIZE - 1) / WAFFLE_BLOCK_SIZE;
		for(i = 0; i < blocks; i++)
			if(append_block(inode) < 0)
				return -1;
	}
	assert(inode->i_size >= size);
	inode->i_size = size;
	return 0;
}

static int init_blocks(void)
{
	uint32_t blocks;
	struct waffle_super * super;
	struct block * block = get_block(1);
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	super->s_active.sn_blocks = nblocks;
	blocks = (nblocks + WAFFLE_BLOCK_SIZE * 8 - 1) / (WAFFLE_BLOCK_SIZE * 8);
	if(setup_inode(&super->s_active.sn_block, blocks * WAFFLE_BLOCK_SIZE) < 0)
	{
		put_block(block);
		return -1;
	}
	put_block(block);
	return 0;
}

static int init_inodes(void)
{
	uint32_t blocks;
	struct waffle_super * super;
	struct block * block = get_block(1);
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	super->s_active.sn_inodes = ninodes;
	blocks = (ninodes + WAFFLE_BLOCK_INODES - 1) / WAFFLE_BLOCK_INODES;
	if(setup_inode(&super->s_active.sn_inode, blocks * WAFFLE_BLOCK_SIZE) < 0)
	{
		put_block(block);
		return -1;
	}
	put_block(block);
	return 0;
}

static int init_root(void)
{
	struct waffle_super * super;
	struct waffle_inode * inode;
	struct waffle_dentry * dirent;
	struct block * block = get_block(1);
	struct block * i_block;
	struct block * d_block;
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	i_block = get_inode_block(&super->s_active.sn_inode, WAFFLE_ROOT_INODE / WAFFLE_BLOCK_INODES);
	if(!i_block)
	{
		put_block(block);
		return -1;
	}
	inode = (struct waffle_inode *) i_block->data;
	inode = &inode[WAFFLE_ROOT_INODE % WAFFLE_BLOCK_INODES];
	inode->i_mode = WAFFLE_S_IFDIR | WAFFLE_S_IRWXU | WAFFLE_S_IRGRP | WAFFLE_S_IXGRP | WAFFLE_S_IROTH | WAFFLE_S_IXOTH;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_links = 2;
	if(append_block(inode) < 0)
	{
		put_block(i_block);
		put_block(block);
		return -1;
	}
	d_block = get_block(inode->i_direct[0]);
	if(!d_block)
	{
		put_block(i_block);
		put_block(block);
		return -1;
	}
	dirent = (struct waffle_dentry *) d_block->data;
	dirent[0].d_inode = WAFFLE_ROOT_INODE;
	strcpy(dirent[0].d_name, ".");
	dirent[1].d_inode = WAFFLE_ROOT_INODE;
	strcpy(dirent[1].d_name, "..");
	put_block(d_block);
	inode->i_atime = time(NULL);
	inode->i_ctime = inode->i_atime;
	inode->i_mtime = inode->i_atime;
	put_block(i_block);
	put_block(block);
	return 0;
}

static int update_blocks(void)
{
	struct waffle_super * super;
	struct block * block = get_block(1);
	struct block * b_block = NULL;
	uint32_t i, max;
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	/* total number of bits in free block bitmap */
	max = super->s_active.sn_block.i_size * 8;
	for(i = next_free; i < max; i++)
	{
		uint32_t need = i / (WAFFLE_BLOCK_SIZE * 8);
		if(!b_block || b_block->number != need)
		{
			if(b_block)
				put_block(b_block);
			b_block = get_inode_block(&super->s_active.sn_block, need);
			if(!b_block)
			{
				put_block(block);
				return -1;
			}
		}
		need = i % (WAFFLE_BLOCK_SIZE * 8);
		((uint32_t *) b_block->data)[need / 32] |= 1 << (need % 32);
	}
	if(b_block)
		put_block(b_block);
	put_block(block);
	return 0;
}

static int init_snapshots(void)
{
	struct waffle_super * super;
	struct block * block = get_block(1);
	if(!block)
		return -1;
	super = (struct waffle_super *) block->data;
	super->s_checkpoint = super->s_active;
	super->s_snapshot = super->s_active;
	put_block(block);
	return 0;
}

/* do enough reads to force any dirty blocks out of the cache */
static void flush_cache(void)
{
	int i, j, k;
	for(i = 1, j = 0; j < CACHE_BLOCKS; i++)
	{
		/* make sure it's not already in the cache */
		for(k = 0; k < CACHE_BLOCKS; k++)
			if(cache[k].number == i)
				break;
		if(k < CACHE_BLOCKS)
			continue;
		/* then read it */
		put_block(get_block(i));
		j++;
	}
}

int main(int argc, char * argv[])
{
	int use_ptable = 0;
	const char * ptable_help = "";
	
	assert(WAFFLE_BLOCK_SIZE % sizeof(struct waffle_inode) == 0);
	assert(WAFFLE_BLOCK_SIZE % sizeof(struct waffle_dentry) == 0);
	
#if BYTE_ORDER == LITTLE_ENDIAN
	ptable_help = "[--ptable] ";
	if(argc > 1 && !strcmp(argv[1], "--ptable"))
	{
		argc--;
		argv[1] = argv[0];
		argv++;
		use_ptable = 1;
	}
#endif
	
	if(argc != 2)
	{
		fprintf(stderr, "Usage: %s %s<device>\n", argv[0], ptable_help);
		return 1;
	}
	
	if(open_disk(argv[1], use_ptable) < 0)
		return 1;
	
	init_super();
	init_blocks();
	init_inodes();
	init_root();
	update_blocks();
	init_snapshots();
	
	flush_cache();
	return 0;
}
