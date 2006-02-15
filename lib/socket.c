#include <inc/lib.h>

#define debug 0

static int socketclose(struct Fd* fd);
static int socketread(struct Fd* fd, void* buf, size_t n, off_t offset);
static int socketread_nb(struct Fd* fd, void* buf, size_t n, off_t offset);
static int socketstat(struct Fd* fd, struct Stat* stat);
static int socketwrite(struct Fd* fd, const void* buf, size_t n, off_t offset);

struct Dev devsocket =
{
	.dev_id=	's',
	.dev_name=	"socket",
	.dev_read=	socketread,
	.dev_read_nb=	socketread_nb,
	.dev_read_map=	NULL,
	.dev_write=	socketwrite,
	.dev_getdirentries = NULL,
	.dev_close=	socketclose,
	.dev_stat=	socketstat,
};

#define PIPEBUFPAGES 16
#define PIPEBUFSIZ (PIPEBUFPAGES * PGSIZE - 2 * sizeof(off_t))
#define SOCKBUFPAGES (PIPEBUFPAGES * 2)

struct Socket {
	struct {
		off_t p_rpos;		// read position
		off_t p_wpos;		// write position
		uint8_t p_buf[PIPEBUFSIZ];	// data buffer
	} pipe[2];
};

int
socket(int pfd[2])
{
	int r, i;
	struct Fd *fd0, *fd1;
	uint8_t * va;

	/* make sure the PIPEBUFSIZE arithmetic is correct */
	static_assert(sizeof(struct Socket) == SOCKBUFPAGES * PGSIZE);
	
	// allocate the file descriptor table entries
	if ((r = fd_alloc(&fd0)) < 0
	    || (r = sys_page_alloc(0, fd0, PTE_P|PTE_W|PTE_U|PTE_SHARE)) < 0)
		goto err;

	if ((r = fd_alloc(&fd1)) < 0
	    || (r = sys_page_alloc(0, fd1, PTE_P|PTE_W|PTE_U|PTE_SHARE)) < 0)
		goto err1;

	// allocate the socket structure as first data page in both
	va = fd2data(fd0);
	for(i = 0; i < SOCKBUFPAGES; i++) {
		int j = (i + PIPEBUFPAGES) % SOCKBUFPAGES;
		if ((r = sys_page_alloc(0, &va[i << PGSHIFT], PTE_P|PTE_W|PTE_U|PTE_SHARE)) < 0)
			goto err2;
		if ((r = sys_page_map(0, &va[i << PGSHIFT], 0, &fd2data(fd1)[j << PGSHIFT], PTE_P|PTE_W|PTE_U|PTE_SHARE)) < 0)
			goto err3;
	}

	// set up fd structures
	fd0->fd_dev_id = devsocket.dev_id;
	fd0->fd_omode = O_RDWR;

	fd1->fd_dev_id = devsocket.dev_id;
	fd1->fd_omode = O_RDWR;

	if (debug)
		printf("[%08x] socketcreate %08x\n", env->env_id, vpt[VPN(va)]);

	pfd[0] = fd2num(fd0);
	pfd[1] = fd2num(fd1);
	return 0;

	while(i-- > 0) {
		int j = (i + PIPEBUFPAGES) % SOCKBUFPAGES;
		sys_page_unmap(0, &fd2data(fd1)[j << PGSHIFT]);
    err3:
		sys_page_unmap(0, &va[i << PGSHIFT]);
    err2:
		/* make the compiler shut up */
		(void) 0;
	}
	sys_page_unmap(0, fd1);
    err1:
	sys_page_unmap(0, fd0);
    err:
	return r;
}

static int
_socketisclosed(struct Fd* fd, struct Socket* p)
{
	int n, nn, ret;

	while (1) {
		n = env->env_runs;
		ret = pageref(fd) == pageref(p);
		nn = env->env_runs;
		if (n == nn)
			return ret;
		if (n != nn && ret == 1)
			printf("socket race avoided\n", n, env->env_runs, ret);
	}
}

int
socketisclosed(int fdnum)
{
	struct Fd* fd;
	struct Socket* p;
	int r;

	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;
	p = (struct Socket*) fd2data(fd);
	return _socketisclosed(fd, p);
}

/* return size available for writing, which, unlike the case for pipes,
 * cannot simply be calculated from the size available for reading */
size_t
socketfree(int fdnum)
{
	struct Fd* fd;
	struct Socket* p;
	int r;
	
	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;
	p = (struct Socket*) fd2data(fd);
	/* return size available for writing */
	return PIPEBUFSIZ - (p->pipe[1].p_wpos - p->pipe[1].p_rpos);
}

static int
socketread(struct Fd* fd, void* vbuf, size_t n, off_t offset)
{
	uint8_t* buf;
	size_t i;
	struct Socket* p;

	(void) offset;	// shut up compiler

	p = (struct Socket*)fd2data(fd);
	if (debug)
		printf("[%08x] socketread %08x %d rpos %d wpos %d\n",
		       env->env_id, vpt[VPN(p)], n, p->pipe[0].p_rpos, p->pipe[0].p_wpos);

	buf = vbuf;
	for (i = 0; i < n; i++) {
		while (p->pipe[0].p_rpos == p->pipe[0].p_wpos) {
			// socket is empty
			// if we got any data, return it
			if (i > 0)
				return i;
			// if all the writers are gone, note eof
			if (_socketisclosed(fd, p))
				return 0;
			// yield and see what happens
			if (debug)
				printf("socketread yield\n");
			sys_yield();
		}
		// there's a byte.  take it.
		buf[i] = p->pipe[0].p_buf[p->pipe[0].p_rpos++ % PIPEBUFSIZ];
	}
	return i;
}

static int
socketread_nb(struct Fd* fd, void* vbuf, size_t n, off_t offset)
{
	uint8_t* buf;
	size_t i;
	struct Socket* p;

	(void) offset; // shut up compiler

	p = (struct Socket*)fd2data(fd);
	if (debug)
		printf("[%08x] socketread %08x %d rpos %d wpos %d\n",
		       env->env_id, vpt[VPN(p)], n, p->pipe[0].p_rpos, p->pipe[0].p_wpos);

	buf = vbuf;
	for (i = 0; i < n; i++) {
		if (p->pipe[0].p_rpos == p->pipe[0].p_wpos) {
			// socket is empty
			// if we got any data, return it
			if (i > 0)
				return i;
			// if all the writers are gone, note eof
			if (_socketisclosed(fd, p))
				return 0;

			// more data not yet ready
			return -1;
		}
		// there's a byte. take it.
		buf[i] = p->pipe[0].p_buf[p->pipe[0].p_rpos++ % PIPEBUFSIZ];
	}

	return i;
}

static int
socketwrite(struct Fd* fd, const void* vbuf, size_t n, off_t offset)
{
	const uint8_t* buf;
	size_t i;
	struct Socket* p;

	(void) offset;	// shut up compiler

	p = (struct Socket*) fd2data(fd);
	if (debug)
		printf("[%08x] socketwrite %08x %d rpos %d wpos %d\n",
		       env->env_id, vpt[VPN(p)], n, p->pipe[1].p_rpos, p->pipe[1].p_wpos);

	buf = vbuf;
	for (i = 0; i < n; i++) {
		while (p->pipe[1].p_wpos >= p->pipe[1].p_rpos + sizeof(p->pipe[1].p_buf)) {
			// socket is full
			// if all the readers are gone
			// (it's only writers like us now),
			// note eof
			if (_socketisclosed(fd, p))
				return 0;
			// yield and see what happens
			if (debug)
				printf("socketwrite yield\n");
			sys_yield();
		}
		// there's room for a byte.  store it.
		p->pipe[1].p_buf[p->pipe[1].p_wpos++ % PIPEBUFSIZ] = buf[i];
	}
	
	return i;
}

static int
socketstat(struct Fd* fd, struct Stat* stat)
{
	struct Socket *p = (struct Socket*) fd2data(fd);
	strcpy(stat->st_name, "<socket>");
	/* return size available for reading */
	stat->st_size = p->pipe[0].p_wpos - p->pipe[0].p_rpos;
	stat->st_isdir = 0;
	stat->st_dev = &devsocket;
	return 0;
}

static int
socketclose(struct Fd* fd)
{
	int i;
	uint8_t * va = fd2data(fd);
	sys_page_unmap(0, fd);
	for(i = 0; i < SOCKBUFPAGES; i++)
		sys_page_unmap(0, &va[i << PGSHIFT]);
	return 0;
}

