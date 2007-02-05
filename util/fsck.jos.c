/*
 * JOS file system fsck
 */

#define _BSD_EXTENSION

/* We don't actually want to define off_t or register_t! */
#define off_t		xxx_off_t
#define register_t	xxx_register_t
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#undef off_t
#undef register_t

#include <kfs/josfs_base.h>

static int diskfd;
static int nblocks;
static int nbitblocks;

typedef enum {
	BLOCK_SUPER,
	BLOCK_DIR,
	BLOCK_BITS,
	BLOCK_INDIR
} Type;

typedef struct {
	Type type;
	uint32_t busy;
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
	
	/* if b->used, evict block b... nothing to do though */
	
	if(lseek(diskfd, bno * JOSFS_BLKSIZE, SEEK_SET) < 0 || readn(diskfd, b->buf, JOSFS_BLKSIZE) != JOSFS_BLKSIZE)
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

/* open the disk, check the superblock, and check the block bitmap for sanity */
static int open_disk(const char * name)
{
	int i;
	struct stat s;
	Block * b;
	struct JOSFS_Super * super;
	
	if((diskfd = open(name, O_RDONLY)) < 0)
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
				fprintf(stderr, "Block %u is not referenced, but marked unavailable\n", i);
				return -1;
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

int main(int argc, char * argv[])
{
	assert(JOSFS_BLKSIZE % sizeof(struct JOSFS_File) == 0);
	
	if(argc != 2)
	{
		fprintf(stderr, "Usage: %s <device>\n", argv[0]);
		return 1;
	}
	
	if(open_disk(argv[1]) < 0)
		return 1;
	
	if(scan_tree() < 0)
		return 1;
	
	if(scan_free() < 0)
		return 1;
	
	printf("File system is OK!\n");
	return 0;
}
