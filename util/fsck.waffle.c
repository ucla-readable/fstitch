/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#define _BSD_EXTENSION
#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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

static int fix = 0;
static int verbose = 0;

static int diskfd;
static off_t diskoff;
static int nblocks, ninodes;

static char current_snapshot[32];

struct block {
	uint32_t dirty:1, busy:31;
	uint32_t used, number;
	uint8_t data[WAFFLE_BLOCK_SIZE];
};

#define CACHE_BLOCKS 64
static struct block cache[CACHE_BLOCKS];
static uint32_t * referenced_bitmap = NULL;
static uint16_t * link_counts = NULL;

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
	static int t = 1;
	struct block * b;
	
	if(!number)
	{
		fprintf(stderr, "Request for reserved block 0\n");
		return NULL;
	}
	
	if(number >= nblocks)
	{
		fprintf(stderr, "Reference to block %u past end of disk\n", number);
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
	
	if(b->used && b->dirty)
	{
		/* evict block b */
		if(lseek(diskfd, diskoff + b->number * WAFFLE_BLOCK_SIZE, SEEK_SET) < 0 || write(diskfd, b->data, WAFFLE_BLOCK_SIZE) != WAFFLE_BLOCK_SIZE)
		{
			fprintf(stderr, "panic: error writing block %d\n", b->number);
			return NULL;
		}
		b->dirty = 0;
	}
	
	if(lseek(diskfd, diskoff + number * WAFFLE_BLOCK_SIZE, SEEK_SET) < 0 || readn(diskfd, b->data, WAFFLE_BLOCK_SIZE) != WAFFLE_BLOCK_SIZE)
	{
		fprintf(stderr, "panic: error reading block %d\n", number);
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

static void swizzle(uint32_t * x)
{
	uint32_t y;
	uint8_t * z;

	z = (uint8_t *) x;
	y = *x;
	z[0] = y & 0xFF;
	z[1] = (y >> 8) & 0xFF;
	z[2] = (y >> 16) & 0xFF;
	z[3] = (y >> 24) & 0xFF;
}

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
	swizzle(&ptable[i].lba_start);
	swizzle(&ptable[i].lba_length);
	printf("Using JOSFS/WAFFLE partition %d, sector offset %d, size %d (%d blocks)\n", i + 1,
	       ptable[i].lba_start, ptable[i].lba_length, ptable[i].lba_length / (WAFFLE_BLOCK_SIZE / 512));
	diskoff = ptable[i].lba_start << 9;
	*size = ptable[i].lba_length << 9;
}

static void reset_block_referenced(void)
{
	memset(referenced_bitmap, 0, (nblocks + 7) / 8);
}

static void reset_link_counts(void)
{
	memset(link_counts, 0, ninodes * sizeof(*link_counts));
}

/* open the disk and check the superblock for sanity */
static int open_disk(const char * name, int use_ptable)
{
	struct stat s;
	uint64_t size;
	struct block * b;
	struct waffle_super * super;
	
	if((diskfd = open(name, fix ? O_RDWR : O_RDONLY)) < 0)
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
#ifdef BLKGETSIZE64
		/* it's a block device; stat size will be zero */
		if(ioctl(diskfd, BLKGETSIZE64, &size) < 0)
		{
			perror(name);
			close(diskfd);
			return -1;
		}
#else
		fprintf(stderr, "%s is a block device\n", name);
		close(diskfd);
		return -1;
#endif
	}
	else
		size = s.st_size;
	/* if requested, and if there is a partition table, use only the JOSFS/WAFFLE partition */
	if(use_ptable)
		partition_adjust(&size);
	nblocks = size / WAFFLE_BLOCK_SIZE;
	
	/* superblock */
	b = get_block(WAFFLE_SUPER_BLOCK);
	if(!b)
	{
		close(diskfd);
		return -1;
	}
	super = (struct waffle_super *) b->data;
	
	if(super->s_magic != WAFFLE_FS_MAGIC)
	{
		fprintf(stderr, "Bad magic number 0x%08x\n", super->s_magic);
		put_block(b);
		close(diskfd);
		return -1;
	}
	ninodes = super->s_inodes;
	
	if(super->s_blocks > nblocks)
	{
		fprintf(stderr, "Bad superblock block count %u\n", super->s_blocks);
		put_block(b);
		close(diskfd);
		return -1;
	}
	else if(super->s_blocks != nblocks)
	{
		fprintf(stderr, "Warning: superblock block count (%u) is smaller than device (%u)\n", super->s_blocks, nblocks);
		/* restrict the check to the superblock's reported size */
		nblocks = super->s_blocks;
	}
	
	referenced_bitmap = malloc((nblocks + 7) / 8);
	if(!referenced_bitmap)
	{
		perror("malloc()");
		put_block(b);
		close(diskfd);
		return -1;
	}
	reset_block_referenced();
	
	link_counts = malloc(ninodes * sizeof(*link_counts));
	if(!link_counts)
	{
		perror("malloc()");
		free(referenced_bitmap);
		put_block(b);
		close(diskfd);
		return -1;
	}
	reset_link_counts();
	
	put_block(b);
	
	return 0;
}

static int get_block_referenced(uint32_t block)
{
	return (referenced_bitmap[block / 32] >> (block % 32)) & 1;
}

static void inode_error(uint32_t inode, const char * name, const char * message, ...)
{
	va_list ap;
	if(inode)
		fprintf(stderr, "Inode %u [%s] ", inode, current_snapshot);
	else
		fprintf(stderr, "Inode <%s> [%s] ", name, current_snapshot);
	va_start(ap, message);
	vfprintf(stderr, message, ap);
	va_end(ap);
}

/* "inode" and "name" are for messages only */
static int set_block_referenced(uint32_t block, uint32_t inode, const char * name)
{
	if(block >= nblocks)
	{
		inode_error(inode, name, "references block %u past end of disk\n", block);
		return -1;
	}
	if(get_block_referenced(block))
	{
		inode_error(inode, name, "references already-referenced block %u\n", block);
		return -1;
	}
	if(verbose > 3)
	{
		if(inode)
			printf("+ Inode %u uses block %u [%s]\n", inode, block, current_snapshot);
		else
			printf("+ Inode <%s> uses block %u [%s]\n", name, block, current_snapshot);
	}
	referenced_bitmap[block / 32] |= 1 << (block % 32);
	return 0;
}

static struct block * get_inode_block(struct waffle_inode * inode, uint32_t offset)
{
	uint32_t block;
	struct block * b;
	offset /= WAFFLE_BLOCK_SIZE;
	if(offset < WAFFLE_DIRECT_BLOCKS)
		return get_block(inode->i_direct[offset]);
	if(offset < WAFFLE_INDIRECT_BLOCKS)
	{
		b = get_block(inode->i_indirect);
		if(!b)
			return NULL;
		block = ((uint32_t *) b->data)[offset - WAFFLE_DIRECT_BLOCKS];
		put_block(b);
		return get_block(block);
	}
	b = get_block(inode->i_dindirect);
	if(!b)
		return NULL;
	block = ((uint32_t *) b->data)[(offset - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS];
	put_block(b);
	b = get_block(block);
	if(!b)
		return NULL;
	block = ((uint32_t *) b->data)[(offset - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
	put_block(b);
	return get_block(block);
}

static int block_marked_free(struct waffle_snapshot * snapshot, uint32_t number)
{
	int value;
	struct block * b = get_inode_block(&snapshot->sn_block, number / 8);
	if(!b)
	{
		fprintf(stderr, "panic: failed to read bitmap\n");
		return 0;
	}
	number %= WAFFLE_BITS_PER_BLOCK;
	value = (((uint32_t *) b->data)[number / 32] >> (number % 32)) & 1;
	put_block(b);
	return value;
}

static int mark_block(struct waffle_snapshot * snapshot, uint32_t number, int free)
{
	struct block * b;
	if(block_marked_free(snapshot, number) != !free)
		return 0;
	b = get_inode_block(&snapshot->sn_block, number / 8);
	if(!b)
	{
		fprintf(stderr, "panic: failed to read bitmap\n");
		return -1;
	}
	number %= WAFFLE_BITS_PER_BLOCK;
	((uint32_t *) b->data)[number / 32] ^= 1 << (number % 32);
	b->dirty = 1;
	put_block(b);
	return 0;
}

/* make sure all referenced blocks are not free, and all unreferenced blocks are free */
static int scan_free(struct waffle_snapshot * snapshot)
{
	uint32_t i, nbitblocks = (nblocks + WAFFLE_BITS_PER_BLOCK - 1) / WAFFLE_BITS_PER_BLOCK;
	
	for(i = 0; i <= WAFFLE_SUPER_BLOCK; i++)
		if(block_marked_free(snapshot, i))
		{
			fprintf(stderr, "Reserved block %u is marked available [%s]\n", i, current_snapshot);
			return -1;
		}
	for(; i < nblocks; i++)
		if(get_block_referenced(i))
		{
			if(block_marked_free(snapshot, i))
			{
				fprintf(stderr, "Block %u is referenced, but marked available [%s]\n", i, current_snapshot);
				return -1;
			}
		}
		else
		{
			if(!block_marked_free(snapshot, i))
			{
				fprintf(stderr, "Block %u is not referenced, but marked unavailable [%s]\n", i, current_snapshot);
				return -1;
			}
		}
	for(; i < nbitblocks * WAFFLE_BITS_PER_BLOCK; i++)
		if(block_marked_free(snapshot, i))
		{
			fprintf(stderr, "Trailing block %u is marked available [%s] (fixed)\n", i, current_snapshot);
			if(!fix || mark_block(snapshot, i, 0) < 0)
				return -1;
		}
	return 0;
}

/* make sure the size matches the block count and record what blocks are used */
static int scan_inode(struct waffle_inode * inode, uint32_t number, const char * name)
{
	uint32_t i, size_blocks;
	uint16_t type;
	if(verbose > 2)
	{
		if(number)
			printf("Scanning inode %u (size %d, %u blocks) [%s]\n", number, inode->i_size, inode->i_blocks, current_snapshot);
		else
			printf("Scanning inode <%s> (size %d, %u blocks) [%s]\n", name, inode->i_size, inode->i_blocks, current_snapshot);
	}
	
	type = inode->i_mode & WAFFLE_S_IFMT;
	if(type == WAFFLE_S_IFLNK)
	{
		if(inode->i_size < WAFFLE_INLINE_SIZE)
		{
			if(inode->i_blocks)
			{
				inode_error(number, name, "is a fast symbolic link but has nonzero block count %d\n", inode->i_blocks);
				return -1;
			}
			/* no need to check this inode further */
			return 0;
		}
		if(inode->i_size < WAFFLE_BLOCK_SIZE)
		{
			if(inode->i_blocks != 1)
			{
				inode_error(number, name, "is a slow symbolic link but has block count %d != 1\n", inode->i_blocks);
				return -1;
			}
		}
		else
		{
			inode_error(number, name, "is a symbolic link but has size %d larger than block size\n", inode->i_size);
			return -1;
		}
	}
	else if(type != WAFFLE_S_IFREG && type != WAFFLE_S_IFDIR)
	{
		struct block * b;
		if(!fix || number)
		{
			inode_error(number, name, "has invalid type 0x%04X\n", type);
			return -1;
		}
		b = get_block(WAFFLE_SUPER_BLOCK);
		if(!b)
			return -1;
		fprintf(stderr, "Inode <%s> [%s] has invalid type 0x%04X (fixed)\n", name, current_snapshot, type);
		inode->i_mode &= ~WAFFLE_S_IFMT;
		inode->i_mode |= WAFFLE_S_IFREG;
		b->dirty = 1;
		put_block(b);
	}
	
	for(i = 0; i < inode->i_blocks; i++)
	{
		uint32_t block;
		if(i < WAFFLE_DIRECT_BLOCKS)
			block = inode->i_direct[i];
		else if(i < WAFFLE_INDIRECT_BLOCKS)
		{
			struct block * b;
			if(i == WAFFLE_DIRECT_BLOCKS)
				/* mark it as referenced */
				if(set_block_referenced(inode->i_indirect, number, name) < 0)
					return -1;
			b = get_block(inode->i_indirect);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[i - WAFFLE_DIRECT_BLOCKS];
			put_block(b);
		}
		else
		{
			struct block * b;
			if(i == WAFFLE_INDIRECT_BLOCKS)
				/* mark it as referenced */
				if(set_block_referenced(inode->i_dindirect, number, name) < 0)
					return -1;
			b = get_block(inode->i_dindirect);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[(i - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS];
			put_block(b);
			if(!((i - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS))
				/* mark it as referenced */
				if(set_block_referenced(block, number, name) < 0)
					return -1;
			b = get_block(block);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[(i - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
			put_block(b);
		}
		/* mark it as referenced */
		if(set_block_referenced(block, number, name) < 0)
			return -1;
	}
	
	size_blocks = (inode->i_size + WAFFLE_BLOCK_SIZE - 1) / WAFFLE_BLOCK_SIZE;
	if(inode->i_blocks != size_blocks)
	{
		inode_error(number, name, "has %u blocks, but should have %u blocks\n", inode->i_blocks, size_blocks);
		return -1;
	}
	
	return 0;
}

static int set_bitmap_blocks_referenced(uint32_t number, const char * name)
{
	uint32_t group = number - (number % WAFFLE_BITMAP_MODULUS);
	uint32_t check, max = group + WAFFLE_BITMAP_MODULUS;
	for(check = group; check != max; check++)
	{
		if(check == number)
			continue;
		/* mark it as referenced */
		if(set_block_referenced(check, 0, name) < 0)
			return -1;
	}
	return 0;
}

static int scan_bitmap_inode(struct waffle_inode * inode, const char * name)
{
	uint32_t i;
	uint16_t type;
	if(verbose)
		printf("Scanning block bitmap inode [%s]\n", current_snapshot);
	
	type = inode->i_mode & WAFFLE_S_IFMT;
	if(type != WAFFLE_S_IFREG)
	{
		inode_error(0, name, "has invalid type 0x%04X\n", type);
		return -1;
	}
	
	for(i = 0; i < inode->i_blocks; i++)
	{
		uint32_t block;
		if(i < WAFFLE_DIRECT_BLOCKS)
			block = inode->i_direct[i];
		else if(i < WAFFLE_INDIRECT_BLOCKS)
		{
			struct block * b;
			if(i == WAFFLE_DIRECT_BLOCKS)
				/* mark it as referenced */
				if(set_bitmap_blocks_referenced(inode->i_indirect, name) < 0)
					return -1;
			b = get_block(inode->i_indirect);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[i - WAFFLE_DIRECT_BLOCKS];
			put_block(b);
		}
		else
		{
			struct block * b;
			if(i == WAFFLE_INDIRECT_BLOCKS)
				/* mark it as referenced */
				if(set_bitmap_blocks_referenced(inode->i_dindirect, name) < 0)
					return -1;
			b = get_block(inode->i_dindirect);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[(i - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS];
			put_block(b);
			if(!((i - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS))
				/* mark it as referenced */
				if(set_bitmap_blocks_referenced(block, name) < 0)
					return -1;
			b = get_block(block);
			if(!b)
				return -1;
			block = ((uint32_t *) b->data)[(i - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
			put_block(b);
		}
		/* mark it as referenced */
		if(set_bitmap_blocks_referenced(block, name) < 0)
			return -1;
	}
	
	return 0;
}

static struct waffle_inode * get_inode(struct waffle_snapshot * snapshot, uint32_t inode, struct block ** put_block)
{
	uint32_t offset = inode * sizeof(struct waffle_inode);
	if(offset + sizeof(struct waffle_inode) > snapshot->sn_inode.i_size)
		return NULL;
	*put_block = get_inode_block(&snapshot->sn_inode, offset);
	if(!*put_block)
		return NULL;
	return (struct waffle_inode *) ((*put_block)->data + (offset % WAFFLE_BLOCK_SIZE));
}

static int scan_dir(struct waffle_snapshot * snapshot, struct waffle_inode * inode, uint32_t number, const char * name)
{
	uint32_t offset;
	assert((inode->i_mode & WAFFLE_S_IFMT) == WAFFLE_S_IFDIR);
	if(verbose > 1)
		printf("Scanning directory inode %u [%s] (\"%s\")\n", number, current_snapshot, name);
	if(inode->i_size % sizeof(struct waffle_dentry))
	{
		fprintf(stderr, "Directory inode %u [%s] has invalid size %d\n", number, current_snapshot, inode->i_size);
		return -1;
	}
	for(offset = 0; offset < inode->i_size; offset += sizeof(struct waffle_dentry))
	{
		struct waffle_dentry * entry;
		struct block * b = get_inode_block(inode, offset);
		if(!b)
			return -1;
		entry = &((struct waffle_dentry *) b->data)[(offset % WAFFLE_BLOCK_SIZE) / sizeof(struct waffle_dentry)];
		if(entry->d_inode)
		{
			int i;
			struct block * ib;
			struct waffle_inode * file;
			for(i = 0; i < WAFFLE_NAME_LEN; i++)
				if(!entry->d_name[i])
					break;
			if(i == WAFFLE_NAME_LEN)
			{
				fprintf(stderr, "Directory inode %u [%s] has non-terminated entry for inode %u\n", number, current_snapshot, entry->d_inode);
				put_block(b);
				return -1;
			}
			file = get_inode(snapshot, entry->d_inode, &ib);
			if(!file)
			{
				put_block(b);
				return -1;
			}
			link_counts[entry->d_inode]--;
			if(entry->d_type == WAFFLE_S_IFDIR && strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
				if(scan_dir(snapshot, file, entry->d_inode, entry->d_name) < 0)
				{
					put_block(ib);
					put_block(b);
					return -1;
				}
			put_block(ib);
		}
		put_block(b);
	}
	if(verbose > 1)
		printf("Done scanning directory inode %u [%s]\n", number, current_snapshot);
	return 0;
}

static int scan_inodes(struct waffle_snapshot * snapshot)
{
	uint32_t inode;
	if(verbose)
		printf("Scanning inode table [%s]\n", current_snapshot);
	for(inode = WAFFLE_ROOT_INODE; inode < ninodes; inode++)
	{
		struct block * ib;
		struct waffle_inode * i = get_inode(snapshot, inode, &ib);
		if(!i)
			return -1;
		if(i->i_links)
		{
			link_counts[inode] = i->i_links;
			if(scan_inode(i, inode, NULL) < 0)
			{
				put_block(ib);
				return -1;
			}
		}
		put_block(ib);
	}
	return 0;
}

static int rescan_inodes(void)
{
	uint32_t inode;
	if(verbose)
		printf("Checking link counts in inode table [%s]\n", current_snapshot);
	for(inode = WAFFLE_ROOT_INODE; inode < ninodes; inode++)
		if(link_counts[inode])
		{
			int16_t delta = link_counts[inode];
			if(delta > 0)
				fprintf(stderr, "Inode %u [%s] link count mismatch: %d too high\n", inode, current_snapshot, delta);
			else
				fprintf(stderr, "Inode %u [%s] link count mismatch: %d too low\n", inode, current_snapshot, -delta);
			return -1;
		}
	return 0;
}

static int scan_snapshot(struct waffle_snapshot * snapshot)
{
	struct block * ib;
	struct waffle_inode * root;
	reset_block_referenced();
	reset_link_counts();
	if(snapshot->sn_blocks != nblocks || snapshot->sn_inodes != ninodes)
	{
		/* we don't support this in fsck yet */
		fprintf(stderr, "panic: snapshot block/inode count does not match file system\n");
		return -1;
	}
	if(scan_inode(&snapshot->sn_block, 0, "block bitmap inode") < 0)
		return -1;
	/* we must mark extra blocks as in use for the block bitmap inode */
	if(scan_bitmap_inode(&snapshot->sn_block, "block bitmap inode") < 0)
		return -1;
	if(scan_inode(&snapshot->sn_inode, 0, "inode table inode") < 0)
		return -1;
	if(scan_inodes(snapshot) < 0)
		return -1;
	root = get_inode(snapshot, WAFFLE_ROOT_INODE, &ib);
	if(!root)
		return -1;
	if(verbose)
		printf("Checking directory structure [%s]\n", current_snapshot);
	if(scan_dir(snapshot, root, WAFFLE_ROOT_INODE, "/") < 0)
	{
		put_block(ib);
		return -1;
	}
	put_block(ib);
	if(rescan_inodes() < 0)
		return -1;
	if(scan_free(snapshot) < 0)
		return -1;
	return 0;
}

static int scan_waffles(void)
{
	int i;
	struct waffle_super * super;
	struct block * b = get_block(WAFFLE_SUPER_BLOCK);
	if(!b)
		return -1;
	super = (struct waffle_super *) b->data;
	snprintf(current_snapshot, sizeof(current_snapshot), "checkpoint");
	if(scan_snapshot(&super->s_checkpoint) < 0)
	{
		put_block(b);
		return -1;
	}
	for(i = 0; i < WAFFLE_SNAPSHOT_COUNT; i++)
	{
		snprintf(current_snapshot, sizeof(current_snapshot), "snapshot %d", i + 1);
		if(scan_snapshot(&super->s_snapshot[i]) < 0)
		{
			put_block(b);
			return -1;
		}
	}
	put_block(b);
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

static struct {
	const char * flag;
	int * variable;
} cmd_option[] = {
	{"--verbose", &verbose},
	{"-V", &verbose},
	{"--fix", &fix}
};
#define OPTIONS (sizeof(cmd_option) / sizeof(cmd_option[0]))

int main(int argc, char * argv[])
{
	assert(WAFFLE_BLOCK_SIZE % sizeof(struct waffle_inode) == 0);
	assert(WAFFLE_BLOCK_SIZE % sizeof(struct waffle_dentry) == 0);
	
	while(argc > 1)
	{
		int i;
		for(i = 0; i != OPTIONS; i++)
			if(!strcmp(argv[1], cmd_option[i].flag))
				break;
		if(i == OPTIONS)
			break;
		argv[1] = argv[0];
		argc--;
		argv++;
		++*cmd_option[i].variable;
	}
	
	if(argc != 2)
	{
		fprintf(stderr, "Usage: %s [--verbose|-V] [--fix] <device>\n", argv[0]);
		return 1;
	}
	
	if(open_disk(argv[1], 1) < 0)
		return 1;
	
	if(scan_waffles() < 0)
		return 1;
	
	flush_cache();
	printf("File system is OK!\n");
	return 0;
}
