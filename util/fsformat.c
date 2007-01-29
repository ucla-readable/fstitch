/*
 * KudOS file system format
 */

#define _BSD_EXTENSION

/* We don't actually want to define off_t or register_t! */
#define off_t		xxx_off_t
#define register_t	xxx_register_t
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#undef off_t
#undef register_t

#include <inc/fs.h>

#define nelem(x)	(sizeof(x) / sizeof((x)[0]))
typedef struct Super Super;
typedef struct File File;

struct Super super;
int diskfd;
uint32_t nblock;
uint32_t nbitblock;
uint32_t nextb;

enum
{
	BLOCK_SUPER,
	BLOCK_DIR,
	BLOCK_FILE,
	BLOCK_BITS
};

typedef struct Block Block;
struct Block
{
	uint32_t busy;
	uint32_t bno;
	uint32_t used;
	uint8_t buf[BLKSIZE];
	uint32_t type;
};

struct Block cache[16];

ssize_t
readn(int f, void* av, size_t n)
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
void
swizzle(uint32_t* x)
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

void
swizzlefile(struct File* f)
{
	int i;

	if (f->f_name[0] == 0)
		return;
	swizzle((uint32_t*) &f->f_size);
	swizzle(&f->f_type);
	for (i = 0; i < NDIRECT; i++)
		swizzle(&f->f_direct[i]);
	swizzle(&f->f_indirect);
	swizzle(&f->f_mtime);
	swizzle(&f->f_atime);
}

void
swizzleblock(Block* b)
{
	int i;
	struct Super* s;
	struct File* f;
	uint32_t* u;

	switch (b->type) {
	case BLOCK_SUPER:
		s = (struct Super*) b->buf;
		swizzle(&s->s_magic);
		swizzle(&s->s_nblocks);
		swizzlefile(&s->s_root);
		break;
	case BLOCK_DIR:
		f = (struct File*) b->buf;
		for (i = 0; i < BLKFILES; i++)
			swizzlefile(f + i);
		break;
	case BLOCK_BITS:
		u = (uint32_t*) b->buf;
		for(i = 0; i < BLKSIZE / 4; i++)
			swizzle(u + i);
		break;
	}
}

void
flushb(Block* b)
{
	swizzleblock(b);
	if (lseek(diskfd, b->bno * BLKSIZE, 0) < 0
	    || write(diskfd, b->buf, BLKSIZE) != BLKSIZE) {
		perror("flushb");
		fprintf(stderr, "\n");
		exit(1);
	}
	swizzleblock(b);
}

Block*
getblk(uint32_t bno, uint32_t clr, uint32_t type)
{
	int i, least;
	static int t = 1;
	Block* b;

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

	if (lseek(diskfd, bno*BLKSIZE, 0) < 0
	    || readn(diskfd, b->buf, BLKSIZE) != BLKSIZE) {
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

void
putblk(Block* b)
{
	b->busy = 0;
}

void
opendisk(const char* name)
{
	int i;
	struct stat s;
	Block* b;

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

	if (s.st_size < 1024 || s.st_size > 128*1024*1024) {
		fprintf(stderr, "bad disk size %d\n", (int) s.st_size);
		exit(1);
	}

	nblock = s.st_size/BLKSIZE;
	nbitblock = (nblock + BLKBITSIZE - 1) / BLKBITSIZE;
	for (i = 0; i < nbitblock; i++){
		b = getblk(2 + i, 0, BLOCK_BITS);
		memset(b->buf, 0xFF, BLKSIZE);
		putblk(b);
	}

	nextb = 2 + nbitblock;

	super.s_magic = FS_MAGIC;
	super.s_nblocks = nblock;
	super.s_root.f_type = FTYPE_DIR;
	strcpy(super.s_root.f_name, "/");
}

void
writefile(char* name)
{
	int fd;
	char *last;
	File *f;
	int i, n, nblk;
	Block *dirb, *b, *bindir;

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
		dirb = getblk(super.s_root.f_direct[super.s_root.f_size/BLKSIZE-1], 0, BLOCK_DIR);
		f = (File*)dirb->buf;
		for (i = 0; i < BLKFILES; i++)
			if (f[i].f_name[0] == 0) {
				f = &f[i];
				goto gotit;
			}
	}
	/* allocate new block */
	dirb = getblk(nextb, 1, BLOCK_DIR);
	super.s_root.f_direct[super.s_root.f_size / BLKSIZE] = nextb++;
	super.s_root.f_size += BLKSIZE;
	f = (File*)dirb->buf;
	
gotit:
	strcpy(f->f_name, last);
	n = 0;
	for(nblk=0;; nblk++){
		b = getblk(nextb, 1, BLOCK_FILE);
		n = readn(fd, b->buf, BLKSIZE);
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
		if(nblk < NDIRECT)
			f->f_direct[nblk] = b->bno;
		else if(nblk < NINDIRECT){
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
		if(n < BLKSIZE)
			break;
	}
	f->f_size = nblk*BLKSIZE + n;
	f->f_mtime = time(NULL);
	f->f_atime = f->f_mtime;
	putblk(dirb);
	close(fd);
}

void
makedir(char* name)
{
	char *last;
	File *f;
	int i;
	Block *dirb;

	last = strrchr(name, '/');
	if(last)
		last++;
	else
		last = name;

	if(super.s_root.f_size > 0){
		dirb = getblk(super.s_root.f_direct[super.s_root.f_size/BLKSIZE-1], 0, BLOCK_DIR);
		f = (File*)dirb->buf;
		for (i = 0; i < BLKFILES; i++)
			if (f[i].f_name[0] == 0) {
				f = &f[i];
				goto gotit;
			}
	}
	/* allocate new block */
	dirb = getblk(nextb, 1, BLOCK_DIR);
	super.s_root.f_direct[super.s_root.f_size / BLKSIZE] = nextb++;
	super.s_root.f_size += BLKSIZE;
	f = (File*)dirb->buf;
	
gotit:
	strcpy(f->f_name, last);
	f->f_size = 0;
	f->f_type = FTYPE_DIR;
	putblk(dirb);
}

void
finishfs(void)
{
	int i;
	Block* b;

	for (i = 0; i < nextb; i++) {
		b = getblk(2 + i/BLKBITSIZE, 0, BLOCK_BITS);
		((u_int*)b->buf)[(i%BLKBITSIZE)/32] &= ~(1<<(i%32));
		putblk(b);
	}

	/* this is slow but not too slow.  i do not care */
	if(nblock != nbitblock*BLKBITSIZE){
		b = getblk(2+nbitblock-1, 0, BLOCK_BITS);
		for (i = nblock % BLKBITSIZE; i < BLKBITSIZE; i++)
			((u_int*)b->buf)[i/32] &= ~(1<<(i%32));
		putblk(b);
	}

	b = getblk(1, 1, BLOCK_SUPER);
	memmove(b->buf, &super, sizeof(Super));
	putblk(b);
}

void
flushdisk(void)
{
	int i;

	for(i=0; i<nelem(cache); i++)
		if(cache[i].used)
			flushb(&cache[i]);
}

int
main(int argc, char **argv)
{
	int i;

	assert(BLKSIZE % sizeof(struct File) == 0);

	if(argc < 2)
	{
		fprintf(stderr, "usage: fsformat kern/fs.img [files...]\n");
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

