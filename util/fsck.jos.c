/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

/*
 * JOS file system fsck
 */

#define _BSD_EXTENSION

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <lib/partition.h>
#include <modules/josfs_lfs.h>

static int fix = 0;

static int diskfd;
static off_t diskoff;
static int nblocks;
static int nbitblocks;

typedef enum {
	BLOCK_SUPER,
	BLOCK_DIR,
	BLOCK_BITS,
	BLOCK_INDIR,
	BLOCK_DATA
} Type;

typedef struct {
	Type type;
	uint32_t dirty:1, busy:31;
	uint32_t used;
	uint32_t bno;
	uint8_t buf[JOSFS_BLKSIZE];
} Block;

#define CACHE_BLOCKS 64
static Block cache[CACHE_BLOCKS];
static uint32_t * referenced_bitmap = NULL;

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

/* make little-endian */
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

static void swizzle_file(struct JOSFS_File * f)
{
	int i;

	if(!f->f_name[0])
		return;
	swizzle((uint32_t *) &f->f_size);
	swizzle(&f->f_type);
	for(i = 0; i < JOSFS_NDIRECT; i++)
		swizzle(&f->f_direct[i]);
	swizzle(&f->f_indirect);
}

static void swizzle_block(Block * b)
{
	int i;
	struct JOSFS_Super * s;
	struct JOSFS_File * f;
	uint32_t * u;

	switch (b->type)
	{
		case BLOCK_SUPER:
			s = (struct JOSFS_Super *) b->buf;
			swizzle(&s->s_magic);
			swizzle(&s->s_nblocks);
			swizzle_file(&s->s_root);
			break;
		case BLOCK_DIR:
			f = (struct JOSFS_File *) b->buf;
			for (i = 0; i < JOSFS_BLKFILES; i++)
				swizzle_file(&f[i]);
			break;
		case BLOCK_BITS:
		case BLOCK_INDIR:
			u = (uint32_t *) b->buf;
			for(i = 0; i < JOSFS_BLKSIZE / 4; i++)
				swizzle(&u[i]);
			break;
		case BLOCK_DATA:
			break;
	}
}

static Block * get_block(uint32_t bno, Type type)
{
	int i, least;
	/* implement an LRU cache */
	static int t = 1;
	Block * b;
	
	if(!bno)
	{
		fprintf(stderr, "Request for reserved block 0\n");
		return NULL;
	}
	
	if(bno >= nblocks)
	{
		fprintf(stderr, "Reference to block %u past end of disk\n", bno);
		return NULL;
	}
	
	least = -1;
	for(i = 0; i < CACHE_BLOCKS; i++)
	{
		if(cache[i].bno == bno)
		{
			/* we could warn about this, but it wouldn't be reliable */
			assert(cache[i].type == type);
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
		swizzle_block(b);
		if(lseek(diskfd, diskoff + b->bno * JOSFS_BLKSIZE, SEEK_SET) < 0 || write(diskfd, b->buf, JOSFS_BLKSIZE) != JOSFS_BLKSIZE)
		{
			fprintf(stderr, "write block %d: ", b->bno);
			perror("");
			return NULL;
		}
		b->dirty = 0;
	}
	
	if(lseek(diskfd, diskoff + bno * JOSFS_BLKSIZE, SEEK_SET) < 0 || readn(diskfd, b->buf, JOSFS_BLKSIZE) != JOSFS_BLKSIZE)
	{
		fprintf(stderr, "read block %d: ", bno);
		perror("");
		return NULL;
	}
	b->type = type;
	b->bno = bno;
	swizzle_block(b);
	
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

static void put_block(Block * b)
{
	b->busy--;
}

static int block_marked_free(uint32_t bno)
{
	uint32_t bitblk = bno / JOSFS_BLKBITSIZE;
	uint32_t offset = bno % JOSFS_BLKBITSIZE;
	Block * b = get_block(2 + bitblk, BLOCK_BITS);
	if(!b)
		return -1;
	put_block(b);
	return (((uint32_t *) b->buf)[offset / 32] >> (offset % 32)) & 1;
}

static void mark_block_free(uint32_t bno)
{
	uint32_t bitblk = bno / JOSFS_BLKBITSIZE;
	uint32_t offset = bno % JOSFS_BLKBITSIZE;
	Block * b = get_block(2 + bitblk, BLOCK_BITS);
	assert(b);
	b->dirty = 1;
	put_block(b);
	((uint32_t *) b->buf)[offset / 32] |= 1 << (offset % 32);
}

/* check for a partition table and use the first JOSFS partition if there is one */
static void partition_adjust(off_t * size)
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
	printf("Using JOSFS partition %d, sector offset %d, size %d (%d blocks)\n", i + 1,
	       ptable[i].lba_start, ptable[i].lba_length, ptable[i].lba_length / (JOSFS_BLKSIZE / 512));
	diskoff = ptable[i].lba_start << 9;
	*size = ptable[i].lba_length << 9;
}

/* open the disk, check the superblock, and check the block bitmap for sanity */
static int open_disk(const char * name)
{
	int i;
	struct stat s;
	Block * b;
	struct JOSFS_Super * super;
	
	if((diskfd = open(name, fix ? O_RDWR : O_RDONLY)) < 0)
	{
		fprintf(stderr, "open: ");
		perror(name);
		return -1;
	}
	
	if(fstat(diskfd, &s) < 0)
	{
		fprintf(stderr, "stat: ");
		perror(name);
		return -1;
	}
	
	/* if there is a partition table, use only the JOSFS partition */
	partition_adjust(&s.st_size);
	
	/* minimally, we have a reserved block, a superblock, and a bitmap block */
	if(s.st_size < 3 * JOSFS_BLKSIZE)
	{
		fprintf(stderr, "Bad disk size %lu\n", (unsigned long) s.st_size);
		return -1;
	}
	nblocks = s.st_size / JOSFS_BLKSIZE;
	
	/* superblock */
	b = get_block(1, BLOCK_SUPER);
	if(!b)
		return -1;
	super = (struct JOSFS_Super *) b->buf;
	
	if(super->s_magic != JOSFS_FS_MAGIC)
	{
		fprintf(stderr, "Bad magic number 0x%08x\n", super->s_magic);
		return -1;
	}
	
	if(super->s_nblocks > nblocks)
	{
		fprintf(stderr, "Bad superblock block count %u\n", super->s_nblocks);
		return -1;
	}
	else if(super->s_nblocks != nblocks)
	{
		fprintf(stderr, "Warning: superblock block count (%u) is smaller than device (%u)\n", super->s_nblocks, nblocks);
		/* restrict the check to the superblock's reported size */
		nblocks = super->s_nblocks;
	}
	
	referenced_bitmap = malloc((nblocks + 7) / 8);
	if(!referenced_bitmap)
	{
		perror("malloc");
		return -1;
	}
	memset(referenced_bitmap, 0, (nblocks + 7) / 8);
	
	if(super->s_root.f_type != JOSFS_TYPE_DIR)
	{
		fprintf(stderr, "Bad file type %u on root entry\n", super->s_root.f_type);
		return -1;
	}
	
	if(strcmp(super->s_root.f_name, "/"))
	{
		fprintf(stderr, "Bad file name \"%s\" on root entry\n", super->s_root.f_name);
		return -1;
	}
	
	put_block(b);
	
	nbitblocks = (nblocks + JOSFS_BLKBITSIZE - 1) / JOSFS_BLKBITSIZE;
	
	for(i = 0; i < 2 + nbitblocks; i++)
		if(block_marked_free(i))
		{
			fprintf(stderr, "Reserved block %u is marked available\n", i);
			return -1;
		}
	for(i = nblocks; i < nbitblocks * JOSFS_BLKBITSIZE; i++)
		if(block_marked_free(i))
		{
			fprintf(stderr, "Trailing block %u is marked available\n", i);
			return -1;
		}
	
	return 0;
}

static int get_block_referenced(uint32_t block)
{
	return (referenced_bitmap[block / 32] >> (block % 32)) & 1;
}

/* use "file" only for error messages */
static int set_block_referenced(uint32_t block, const char * file)
{
	if(block >= nblocks)
	{
		fprintf(stderr, "File %s references block %u past end of disk\n", file, block);
		return -1;
	}
	if(get_block_referenced(block))
	{
		fprintf(stderr, "File %s references already-referenced block %u\n", file, block);
		return -1;
	}
	referenced_bitmap[block / 32] |= 1 << (block % 32);
	return 0;
}

/* make sure all referenced blocks are not free, and all unreferenced blocks are free */
static int scan_free(void)
{
	int i;
	for(i = 2 + nbitblocks; i < nblocks; i++)
		if(get_block_referenced(i))
		{
			if(block_marked_free(i))
			{
				fprintf(stderr, "Block %u is referenced, but marked available\n", i);
				return -1;
			}
		}
		else
		{
			if(!block_marked_free(i))
			{
				fprintf(stderr, "Block %u is not referenced, but marked unavailable%s\n", i, fix ? " (fixed)" : "");
				if(!fix)
					return -1;
				else
					mark_block_free(i);
			}
		}
	return 0;
}

/* make sure the size matches the block count and record what blocks are used */
static int scan_file(struct JOSFS_File * file)
{
	int i, size_blocks, count_blocks = 0;
	for(i = 0; i < JOSFS_MAXNAMELEN; i++)
		if(!file->f_name[i])
			break;
	if(i == JOSFS_MAXNAMELEN)
	{
		fprintf(stderr, "File name is not null-terminated\n");
		return -1;
	}
	printf("Scanning file %s\n", file->f_name);
	
	if(file->f_type != JOSFS_TYPE_FILE && file->f_type != JOSFS_TYPE_DIR)
	{
		fprintf(stderr, "File %s has invalid type %d\n", file->f_name, file->f_type);
		return -1;
	}
	
	for(i = 0; i < JOSFS_NDIRECT; i++)
	{
		if(!file->f_direct[i])
			break;
		count_blocks++;
		/* mark it as referenced */
		if(set_block_referenced(file->f_direct[i], file->f_name) < 0)
			return -1;
	}
	if(i == JOSFS_NDIRECT)
	{
		if(file->f_indirect)
		{
			Block * b = get_block(file->f_indirect, BLOCK_INDIR);
			uint32_t * blocks;
			if(!b)
				return -1;
			blocks = (uint32_t *) b->buf;
			set_block_referenced(file->f_indirect, file->f_name);
			for(i = 0; i < JOSFS_NDIRECT; i++)
				if(blocks[i])
				{
					fprintf(stderr, "File %s has hidden indirect block %u\n", file->f_name, blocks[i]);
					return -1;
				}
			for(; i < JOSFS_NINDIRECT; i++)
			{
				if(!blocks[i])
					break;
				count_blocks++;
				/* mark it as referenced */
				if(set_block_referenced(blocks[i], file->f_name) < 0)
					return -1;
			}
			for(; i < JOSFS_NINDIRECT; i++)
				if(blocks[i])
				{
					fprintf(stderr, "File %s has sparse indirect blocks\n", file->f_name);
					return -1;
				}
			put_block(b);
		}
	}
	else
	{
		for(; i < JOSFS_NDIRECT; i++)
			if(file->f_direct[i])
			{
				fprintf(stderr, "File %s has sparse direct blocks\n", file->f_name);
				return -1;
			}
		if(file->f_indirect)
		{
			fprintf(stderr, "File %s has indirect block but is missing direct blocks\n", file->f_name);
			return -1;
		}
	}
	
	size_blocks = (file->f_size + JOSFS_BLKSIZE - 1) / JOSFS_BLKSIZE;
	if(count_blocks != size_blocks)
	{
		fprintf(stderr, "File %s has %d blocks, but should have %d blocks\n", file->f_name, count_blocks, size_blocks);
		return -1;
	}
	
	return 0;
}

static Block * get_dir_block(struct JOSFS_File * file, uint32_t offset)
{
	offset /= JOSFS_BLKSIZE;
	if(offset < JOSFS_NDIRECT)
		return get_block(file->f_direct[offset], BLOCK_DIR);
	if(offset < JOSFS_NINDIRECT)
	{
		uint32_t * blocks;
		uint32_t block;
		Block * b = get_block(file->f_indirect, BLOCK_INDIR);
		if(!b)
			return NULL;
		blocks = (uint32_t *) b->buf;
		block = blocks[offset];
		put_block(b);
		return get_block(block, BLOCK_DIR);
	}
	fprintf(stderr, "Request for block %u of %s past maximum indirect block number\n", offset, file->f_name);
	return NULL;
}

static int scan_dir(struct JOSFS_File * file)
{
	uint32_t offset;
	assert(file->f_type == JOSFS_TYPE_DIR);
	printf("Scanning directory %s\n", file->f_name);
	if(file->f_size % sizeof(struct JOSFS_File))
	{
		fprintf(stderr, "Directory %s has invalid size %d\n", file->f_name, file->f_size);
		return -1;
	}
	for(offset = 0; offset < file->f_size; offset += sizeof(struct JOSFS_File))
	{
		struct JOSFS_File * entry;
		Block * b = get_dir_block(file, offset);
		if(!b)
			return -1;
		entry = &((struct JOSFS_File *) b->buf)[(offset / sizeof(struct JOSFS_File)) % JOSFS_BLKFILES];
		if(entry->f_name[0])
		{
			if(scan_file(entry) < 0)
				return -1;
			if(entry->f_type == JOSFS_TYPE_DIR)
				if(scan_dir(entry) < 0)
					return -1;
		}
		put_block(b);
	}
	printf("Done scanning directory %s\n", file->f_name);
	return 0;
}

static int scan_tree(void)
{
	struct JOSFS_Super * super;
	Block * b = get_block(1, BLOCK_SUPER);
	if(!b)
		return -1;
	super = (struct JOSFS_Super *) b->buf;
	if(scan_file(&super->s_root) < 0)
		return -1;
	if(scan_dir(&super->s_root) < 0)
		return -1;
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
			if(cache[k].bno == i)
				break;
		if(k < CACHE_BLOCKS)
			continue;
		/* then read it */
		put_block(get_block(i, BLOCK_DATA));
		j++;
	}
}

int main(int argc, char * argv[])
{
	assert(JOSFS_BLKSIZE % sizeof(struct JOSFS_File) == 0);
	
	if(argc > 1 && !strcmp(argv[1], "-fix"))
	{
		int i;
		argc--;
		for(i = 1; i < argc; i++)
			argv[i] = argv[i + 1];
		fix = 1;
	}
	
	if(argc != 2)
	{
		fprintf(stderr, "Usage: %s [-fix] <device>\n", argv[0]);
		return 1;
	}
	
	if(open_disk(argv[1]) < 0)
		return 1;
	
	if(scan_tree() < 0)
		return 1;
	
	if(scan_free() < 0)
		return 1;
	
	flush_cache();
	printf("File system is OK!\n");
	return 0;
}
