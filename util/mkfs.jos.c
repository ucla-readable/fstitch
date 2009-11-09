/* This file is part of Featherstitch. Featherstitch is copyright 2005-2009 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

/*
 * JOS file system format
 */

#define _BSD_EXTENSION

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <lib/partition.h>
#include <modules/josfs_lfs.h>

#define nelem(x)	(sizeof(x) / sizeof((x)[0]))

static struct JOSFS_Super super;
static int diskfd;
static off_t diskoff;
static uint32_t nblock;
static uint32_t nbitblock;
static uint32_t nextb;

enum
{
	BLOCK_SUPER,
	BLOCK_DIR,
	BLOCK_FILE,
	BLOCK_BITS
};

struct Block
{
	uint32_t busy;
	uint32_t bno;
	uint32_t used;
	uint8_t buf[JOSFS_BLKSIZE];
	uint32_t type;
};

static struct Block cache[16];

static ssize_t readn(int f, void* av, size_t n)
{
	uint8_t* a;
	size_t t;

	a = av;
	t = 0;
	while (t < n) {
		size_t m = read(f, a + t, n - t);
		if (m <= 0) {
			if (t == 0)
				return m;
			break;
		}
		t += m;
	}
	return t;
}

/* make little-endian */
static void swizzle(uint32_t* x)
{
	uint32_t y;
	uint8_t* z;

	z = (uint8_t*) x;
	y = *x;
	z[0] = y & 0xFF;
	z[1] = (y >> 8) & 0xFF;
	z[2] = (y >> 16) & 0xFF;
	z[3] = (y >> 24) & 0xFF;
}

static void swizzlefile(struct JOSFS_File* f)
{
	int i;

	if (f->f_name[0] == 0)
		return;
	swizzle((uint32_t*) &f->f_size);
	swizzle(&f->f_type);
	for (i = 0; i < JOSFS_NDIRECT; i++)
		swizzle(&f->f_direct[i]);
	swizzle(&f->f_indirect);
	swizzle(&f->f_mtime);
	swizzle(&f->f_atime);
}

static void swizzleblock(struct Block* b)
{
	int i;
	struct JOSFS_Super* s;
	struct JOSFS_File* f;
	uint32_t* u;

	switch (b->type) {
	case BLOCK_SUPER:
		s = (struct JOSFS_Super*) b->buf;
		swizzle(&s->s_magic);
		swizzle(&s->s_nblocks);
		swizzlefile(&s->s_root);
		break;
	case BLOCK_DIR:
		f = (struct JOSFS_File*) b->buf;
		for (i = 0; i < JOSFS_BLKFILES; i++)
			swizzlefile(f + i);
		break;
	case BLOCK_BITS:
		u = (uint32_t*) b->buf;
		for(i = 0; i < JOSFS_BLKSIZE / 4; i++)
			swizzle(u + i);
		break;
	}
}

static void flushb(struct Block* b)
{
	swizzleblock(b);
	if (lseek(diskfd, diskoff + b->bno * JOSFS_BLKSIZE, 0) < 0
	    || write(diskfd, b->buf, JOSFS_BLKSIZE) != JOSFS_BLKSIZE) {
		perror("flushb");
		fprintf(stderr, "\n");
		exit(1);
	}
	swizzleblock(b);
}

static struct Block* getblk(uint32_t bno, uint32_t clr, uint32_t type)
{
	int i, least;
	static int t = 1;
	struct Block* b;

	if (bno >= nblock) {
		fprintf(stderr, "attempt to access past end of disk bno=%d\n", bno);
		*(int*) 0 = 0;
		exit(1);
	}

	least = -1;
	for (i = 0; i < nelem(cache); i++) {
		if (cache[i].bno == bno) {
			b = &cache[i];
			goto out;
		}
		if (!cache[i].busy
		    && (least == -1 || cache[i].used < cache[least].used))
			least = i;
	}

	if (least == -1) {
		fprintf(stderr, "panic: block cache full\n");
		exit(1);
	}

	b = &cache[least];
	if(b->used)
		flushb(b);

	if (lseek(diskfd, diskoff + bno * JOSFS_BLKSIZE, 0) < 0
	    || readn(diskfd, b->buf, JOSFS_BLKSIZE) != JOSFS_BLKSIZE) {
		fprintf(stderr, "read block %d: ", bno);
		perror("");
		fprintf(stderr, "\n");
		exit(1);
	}
	b->bno = bno;
	if (!clr)
		swizzleblock(b);

out:
	if (clr)
		memset(b->buf, 0, sizeof(b->buf));
	b->used = ++t;
	if (b->busy) {
		fprintf(stderr, "panic: b is busy\n");
		exit(1);
	}
	/* it is important to reset b->type in case we reuse a block for a
	 * different purpose while it is still in the cache - this can happen
	 * for example if a file ends exactly on a block boundary */
	b->type = type;
	b->busy = 1;
	return b;
}

static void putblk(struct Block* b)
{
	b->busy = 0;
}

/* check for a partition table and use the first JOSFS partition if there is one */
static void partition_adjust(off_t * size)
{
	unsigned char mbr[512];
	struct pc_ptable * ptable;
	int i = read(diskfd, mbr, 512);
	off_t end;
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
	/* make sure the file is large enough */
	end = (ptable[i].lba_start + ptable[i].lba_length) * 512;
	if(*size < end)
		ftruncate(diskfd, end);
	diskoff = ptable[i].lba_start * 512;
	*size = ptable[i].lba_length * 512;
}

static void opendisk(const char* name)
{
	int i;
	struct stat s;
	struct Block* b;

	if ((diskfd = open(name, O_RDWR)) < 0) {
		fprintf(stderr, "open %s: ", name);
		perror("");
		fprintf(stderr, "\n");
		exit(1);
	}
	
	if (fstat(diskfd, &s) < 0) {
		fprintf(stderr, "cannot stat %s: ", name);
		perror("");
		fprintf(stderr, "\n");
		exit(1);
	}

	/* if there is a partition table, use only the JOSFS partition */
	partition_adjust(&s.st_size);

	if (s.st_size < 1024 || s.st_size > 512*1024*1024) {
		fprintf(stderr, "bad disk size %d\n", (int) s.st_size);
		exit(1);
	}

	nblock = s.st_size/JOSFS_BLKSIZE;
	nbitblock = (nblock + JOSFS_BLKBITSIZE - 1) / JOSFS_BLKBITSIZE;
	for (i = 0; i < nbitblock; i++){
		b = getblk(2 + i, 0, BLOCK_BITS);
		memset(b->buf, 0xFF, JOSFS_BLKSIZE);
		putblk(b);
	}

	nextb = 2 + nbitblock;

	super.s_magic = JOSFS_FS_MAGIC;
	super.s_nblocks = nblock;
	super.s_root.f_type = JOSFS_TYPE_DIR;
	strcpy(super.s_root.f_name, "/");
}

static void writefile(char* name)
{
	int fd;
	char *last;
	struct JOSFS_File *f;
	int i, n, nblk;
	struct Block *dirb, *b, *bindir;

	if((fd = open(name, O_RDONLY)) < 0){
		fprintf(stderr, "open %s:", name);
		perror("");
		exit(1);
	}

	last = strrchr(name, '/');
	if(last)
		last++;
	else
		last = name;

	if(super.s_root.f_size > 0){
		dirb = getblk(super.s_root.f_direct[super.s_root.f_size/JOSFS_BLKSIZE-1], 0, BLOCK_DIR);
		f = (struct JOSFS_File*)dirb->buf;
		for (i = 0; i < JOSFS_BLKFILES; i++)
			if (f[i].f_name[0] == 0) {
				f = &f[i];
				goto gotit;
			}
	}
	/* allocate new block */
	dirb = getblk(nextb, 1, BLOCK_DIR);
	super.s_root.f_direct[super.s_root.f_size / JOSFS_BLKSIZE] = nextb++;
	super.s_root.f_size += JOSFS_BLKSIZE;
	f = (struct JOSFS_File*)dirb->buf;
	
gotit:
	strcpy(f->f_name, last);
	n = 0;
	for(nblk=0;; nblk++){
		b = getblk(nextb, 1, BLOCK_FILE);
		n = readn(fd, b->buf, JOSFS_BLKSIZE);
		if(n < 0){
			fprintf(stderr, "reading %s: ", name);
			perror("");
			exit(1);
		}
		if(n == 0){
			putblk(b);
			break;
		}
		nextb++;
		if(nblk < JOSFS_NDIRECT)
			f->f_direct[nblk] = b->bno;
		else if(nblk < JOSFS_NINDIRECT){
			if(f->f_indirect == 0){
				bindir = getblk(nextb++, 1, BLOCK_BITS);
				f->f_indirect = bindir->bno;
			}else
				bindir = getblk(f->f_indirect, 0, BLOCK_BITS);
			((u_int*)bindir->buf)[nblk] = b->bno;
			putblk(bindir);
		}else{
			fprintf(stderr, "%s: file too large\n", name);
			exit(1);
		}
		
		putblk(b);
		if(n < JOSFS_BLKSIZE)
			break;
	}
	f->f_size = nblk*JOSFS_BLKSIZE + n;
	f->f_type = JOSFS_TYPE_FILE;
	f->f_mtime = time(NULL);
	f->f_atime = f->f_mtime;
	putblk(dirb);
	close(fd);
}

static void makedir(char* name)
{
	char *last;
	struct JOSFS_File *f;
	int i;
	struct Block *dirb;

	last = strrchr(name, '/');
	if(last)
		last++;
	else
		last = name;

	if(super.s_root.f_size > 0){
		dirb = getblk(super.s_root.f_direct[super.s_root.f_size/JOSFS_BLKSIZE-1], 0, BLOCK_DIR);
		f = (struct JOSFS_File*)dirb->buf;
		for (i = 0; i < JOSFS_BLKFILES; i++)
			if (f[i].f_name[0] == 0) {
				f = &f[i];
				goto gotit;
			}
	}
	/* allocate new block */
	dirb = getblk(nextb, 1, BLOCK_DIR);
	super.s_root.f_direct[super.s_root.f_size / JOSFS_BLKSIZE] = nextb++;
	super.s_root.f_size += JOSFS_BLKSIZE;
	f = (struct JOSFS_File*)dirb->buf;
	
gotit:
	strcpy(f->f_name, last);
	f->f_size = 0;
	f->f_type = JOSFS_TYPE_DIR;
	putblk(dirb);
}

static void finishfs(void)
{
	int i;
	struct Block* b;

	for (i = 0; i < nextb; i++) {
		b = getblk(2 + i/JOSFS_BLKBITSIZE, 0, BLOCK_BITS);
		((u_int*)b->buf)[(i%JOSFS_BLKBITSIZE)/32] &= ~(1<<(i%32));
		putblk(b);
	}

	/* this is slow but not too slow.  i do not care */
	if(nblock != nbitblock*JOSFS_BLKBITSIZE){
		b = getblk(2+nbitblock-1, 0, BLOCK_BITS);
		for (i = nblock % JOSFS_BLKBITSIZE; i < JOSFS_BLKBITSIZE; i++)
			((u_int*)b->buf)[i/32] &= ~(1<<(i%32));
		putblk(b);
	}

	b = getblk(1, 1, BLOCK_SUPER);
	memmove(b->buf, &super, sizeof(struct JOSFS_Super));
	putblk(b);
}

static void flushdisk(void)
{
	int i;

	for(i=0; i<nelem(cache); i++)
		if(cache[i].used)
			flushb(&cache[i]);
}

int main(int argc, char **argv)
{
	int i;

	assert(JOSFS_BLKSIZE % sizeof(struct JOSFS_File) == 0);

	if(argc < 2)
	{
		fprintf(stderr, "usage: fsformat fs.img [files...]\n");
		return 1;
	}

	opendisk(argv[1]);
	for(i=2; i<argc; i++)
	{
		struct stat s;
		if(stat(argv[i], &s) < 0)
		{
			perror(argv[i]);
			continue;
		}
		if(S_ISDIR(s.st_mode))
			makedir(argv[i]);
		else
			writefile(argv[i]);
	}
	finishfs();
	flushdisk();
	exit(0);
	return 0;
}
