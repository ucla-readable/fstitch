#include <inc/lib.h>
#include <inc/mmu.h>

#define debug		0

// Maximum number of file descriptors a program may hold open concurrently
#define MAXFD		32
// Bottom of file data area
#define FILEBASE	0xd0000000
// Bottom of file descriptor area
#define FDTABLE		(FILEBASE - PTSIZE)

// Return the 'struct Fd*' for file descriptor index i
#define INDEX2FD(i)	((struct Fd*) (FDTABLE + (i)*PGSIZE))
// Return the file data pointer for file descriptor index i
#define INDEX2DATA(i)	((char*) (FILEBASE + (i)*PTSIZE))


/********************************
 * FILE DESCRIPTOR MANIPULATORS *
 *                              *
 ********************************/

char*
fd2data(struct Fd* fd)
{
	return INDEX2DATA(fd2num(fd));
}

int
fd2num(struct Fd* fd)
{
	return ((uintptr_t) fd - FDTABLE) / PGSIZE;
}

// Finds the smallest i from 0 to MAXFD-1 that doesn't have
// its fd page mapped.
// Sets *fd_store to the corresponding fd page virtual address.
//
// fd_alloc does NOT actually allocate an fd page.
// It is up to the caller to allocate the page somehow.
// This means that if someone calls fd_alloc twice in a row
// without allocating the first page we return, we'll return the same
// page the second time.
//
// Hint: Use INDEX2FD.
//
// Returns fd index on success, < 0 on error.  Errors are:
//	-E_MAX_FD: no more file descriptors
// On error, *fd_store is set to 0.
int
fd_alloc(struct Fd** fd_store)
{
	int i;
	
	for(i = 0; i != MAXFD; i++)
	{
		struct Fd * fd = INDEX2FD(i);
		
		if(!(vpd[PDX(fd)] & PTE_P) || !(vpt[VPN(fd)] & PTE_P))
		{
			*fd_store = fd;
			return i;
		}
	}
	
	*fd_store = NULL;
	
	return -E_MAX_OPEN;
}

// Check that fdnum is in range and mapped.
// If it is, set *fd_store to the fd page virtual address.
//
// Returns 0 on success (the page is in range and mapped), < 0 on error.
// Errors are:
//	-E_INVAL: fdnum was either not in range or not mapped.
int
fd_lookup(int fdnum, struct Fd** fd_store)
{
	struct Fd * fd = INDEX2FD(fdnum);
	
	if(fdnum < 0 || fdnum >= MAXFD)
		return -E_INVAL;
	if(!(vpd[PDX(fd)] & PTE_P) || !(vpt[VPN(fd)] & PTE_P))
		return -E_INVAL;
	
	*fd_store = fd;
	
	return 0;
}

// Frees file descriptor 'fd' by closing the corresponding file
// and unmapping the file descriptor page.
// If 'must_exist' is 0, then fd can be a closed or nonexistent file
// descriptor; the function will return 0 and have no other effect.
// If 'must_exist' is 1, then fd_close returns -E_INVAL when passed a
// closed or nonexistent file descriptor.
// Returns 0 on success, < 0 on error.
int
fd_close(struct Fd* fd, bool must_exist)
{
	struct Fd* fd2;
	struct Dev* dev;
	int r;
	if ((r = fd_lookup(fd2num(fd), &fd2)) < 0
	    || fd != fd2)
		return (must_exist ? r : 0);
	if ((r = dev_lookup(fd->fd_dev_id, &dev)) >= 0)
		r = (*dev->dev_close)(fd);
	sys_page_unmap(0, fd);
	return r;
}


/******************
 * FILE FUNCTIONS *
 *                *
 ******************/

static struct Dev* devtab[] =
{
	&devfile,
	&devpipe,
	&devsocket,
	&devcons,
	&devkpl,
	0
};

int
dev_lookup(int dev_id, struct Dev** dev)
{
	int i;
	for (i = 0; devtab[i]; i++)
		if (devtab[i]->dev_id == dev_id) {
			*dev = devtab[i];
			return 0;
		}
	kdprintf(STDERR_FILENO, "[%08x] unknown device type %d\n", env->env_id, dev_id);
	*dev = 0;
	return -E_INVAL;
}

int
close(int fdnum)
{
	struct Fd *fd = NULL;
	(void) fd_lookup(fdnum, &fd);
	return fd_close(fd, 1);
}

void
close_all(void)
{
	int i;
	for (i = 0; i < MAXFD; i++)
		close(i);
}

// Make file descriptor 'newfdnum' a duplicate of file descriptor 'oldfdnum'.
// For instance, writing onto either file descriptor will affect the
// file and the file offset of the other.
// Closes any previously open file descriptor at 'newfdnum'.
// This is implemented using virtual memory tricks (of course!).
int
dup2(int oldfdnum, int newfdnum)
{
	int i, r;
	char* ova, *nva;
	pte_t pte;
	struct Fd* oldfd, *newfd;

	if ((r = fd_lookup(oldfdnum, &oldfd)) < 0)
		return r;
	close(newfdnum);

	newfd = INDEX2FD(newfdnum);
	ova = fd2data(oldfd);
	nva = fd2data(newfd);

	if (vpd[PDX(ova)]) {
		for (i = 0; i < PTSIZE; i += PGSIZE) {
			pte = vpt[VPN(ova + i)];
			if (pte&PTE_P) {
				// should be no error here -- pd is already allocated
				if ((r = sys_page_map(0, ova + i, 0, nva + i, pte & PTE_USER)) < 0)
					goto err;
			}
		}
	}
	if ((r = sys_page_map(0, oldfd, 0, newfd, vpt[VPN(oldfd)] & PTE_USER)) < 0)
		goto err;

	return newfdnum;

err:
	sys_page_unmap(0, newfd);
	for (i = 0; i < PTSIZE; i += PGSIZE)
		sys_page_unmap(0, nva + i);
	return r;
}

// Return a new file descriptor that is a duplicate of file descriptor 'fdnum'.
int
dup(int fdnum)
{
	struct Fd * fd;
	int newfdnum;

	newfdnum = fd_alloc(&fd);
	if (newfdnum < 0)
		return newfdnum;

	return dup2(fdnum, newfdnum);
}

// 'dup', but dup to another environment.
// The other environment needs to actively receive the ipc, use dup2env_recv().
// Returns 0 on success, <0 on fd_lookup(fdnum) failure. Blocks unitl
// destination environment has received everything we want to send it.
//
// NOTE: as is, this function only works on fds that do not have holes
// in their data regions (non-PTE_P pages).
int
dup2env_send(int fdnum, envid_t envid)
{
	int i, r;
	uint8_t* va;
	pte_t pte;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;

	// Send the fd page
	va = (uint8_t*) fd;
	pte = vpt[VPN(va)];
	ipc_send(envid, 0, va, pte & PTE_USER, NULL);

	// Send the data pages
	va = fd2data(fd);
	if (vpd[PDX(va)])
	{
		for (i = 0; i < PTSIZE; i += PGSIZE)
		{
			pte = vpt[VPN(va + i)];
			if (!(pte & PTE_P))
			{
				// NOTE: The other side must know where to put the next
				// page so we cannot simply skip over unmapped pages and
				// then send mapped pages that follow.
				break;
			}
			ipc_send(envid, i, va+i, pte & PTE_USER, NULL);
		}
	}

	// Note end of data
	ipc_send(envid, 0, NULL, 0, NULL);

	return 0;
}

// Create a new fd that is a dup of another env's fd, sent using
// dup2env_send().
// Returns the new fd, or <0 if failure to allocate a fd.
int
dup2env_recv(envid_t from_env)
{
	int i, r;
	uint8_t* va;
	int perm;
	int fdnum;
	struct Fd* fd;

	if ((r = fd_alloc(&fd)) < 0)
		return r;

	// Receive the fd page
	(void) ipc_recv(from_env, NULL, fd, NULL, NULL, 0);

	// Receive the data pages
	va = fd2data(fd);
	for (i = 0; i < PTSIZE; i += PGSIZE)
	{
		(void) ipc_recv(from_env, NULL, va+i, &perm, NULL, 0);
		if (!perm)
			break; // !perm signifies end of data
	}

	fdnum = fd2num(fd);
	return fdnum;
}

int
read(int fdnum, void* buf, size_t n)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if ((fd->fd_omode & O_ACCMODE) == O_WRONLY) {
		kdprintf(STDERR_FILENO, "[%08x] read %d -- bad mode\n", env->env_id, fdnum); 
		return -E_INVAL;
	}
	r = (*dev->dev_read)(fd, buf, n, fd->fd_offset);
	if (r >= 0)
		fd->fd_offset += r;
	return r;
}

int
read_nb(int fdnum, void* buf, size_t n)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if ((fd->fd_omode & O_ACCMODE) == O_WRONLY) {
		kdprintf(STDERR_FILENO, "[%08x] read %d -- bad mode\n", env->env_id, fdnum); 
		return -E_INVAL;
	}
	r = (*dev->dev_read_nb)(fd, buf, n, fd->fd_offset);
	if (r >= 0)
		fd->fd_offset += r;
	return r;
}

/*
int
readn(int fdnum, void* buf, size_t n)
{
	int m, tot;

	for (tot = 0; tot < n; tot += m) {
		m = read(fdnum, (char*)buf + tot, n - tot);
		if (m < 0)
			return m;
		if (m == 0)
			break;
	}
	return tot;
}
*/

int
read_map(int fdnum, off_t offset, void** blk)
{
	int r;
	struct Dev* dev;
	struct Fd *fd;
	
	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if(!dev->dev_read_map)
		return -E_INVAL;
	if ((fd->fd_omode & O_ACCMODE) == O_WRONLY) {
		kdprintf(STDERR_FILENO, "[%08x] read %d -- bad mode\n", env->env_id, fdnum); 
		return -E_INVAL;
	}
	return (*dev->dev_read_map)(fd, offset, blk);
}

int
write(int fdnum, const void* buf, size_t n)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if ((fd->fd_omode & O_ACCMODE) == O_RDONLY) {
		kdprintf(STDERR_FILENO, "[%08x] write %d -- bad mode\n", env->env_id, fdnum);
		return -E_INVAL;
	}
	if (debug)
		printf("write %d %p %d via dev %s\n",
		       fdnum, buf, n, dev->dev_name);
	r = (*dev->dev_write)(fd, buf, n, fd->fd_offset);
	if (r > 0)
		fd->fd_offset += r;
	return r;
}

int
getdirentries(int fdnum, void* buf, int nbytes, uint32_t* basep)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if (!dev->dev_write)
		return -E_INVAL;
	return (*dev->dev_getdirentries)(fd, buf, nbytes, basep);
}

int
seek(int fdnum, off_t offset)
{
	int r;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;
	if (fd->fd_dev_id != 'f' && fd->fd_dev_id != 'k')
		return -E_INVAL;
	fd->fd_offset = offset;
	return 0;
}

int
ftruncate(int fdnum, off_t newsize)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;
	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	if ((fd->fd_omode & O_ACCMODE) == O_RDONLY) {
		kdprintf(STDERR_FILENO, "[%08x] ftruncate %d -- bad mode\n",
		       env->env_id, fdnum); 
		return -E_INVAL;
	}
	return (*dev->dev_trunc)(fd, newsize);
}

int
fstat(int fdnum, struct Stat* stat)
{
	int r;
	struct Dev* dev;
	struct Fd* fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0
	    || (r = dev_lookup(fd->fd_dev_id, &dev)) < 0)
		return r;
	stat->st_name[0] = 0;
	stat->st_size = 0;
	stat->st_isdir = 0;
	stat->st_dev = dev;
	return (*dev->dev_stat)(fd, stat);
}

int
stat(const char* path, struct Stat* stat)
{
	int fd, r;

	if ((fd = open(path, O_RDONLY)) < 0)
		return fd;
	r = fstat(fd, stat);
	close(fd);
	return r;
}

// wait until fdnum has <= nrefs to its memory.
// Returns 0 on success and after fdnum has <= nrefs,
// or <0 on failure to lookup fdnum.
int
wait_fd(int fdnum, size_t nrefs)
{
	struct Fd* fd;
	physaddr_t fd_pa;
	volatile struct Page* p;
	int r;

	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;

	fd_pa = PTE_ADDR(vpt[VPN(fd)]);
	p = &((struct Page*) UPAGES)[fd_pa / PGSIZE];

	while (p->pp_ref > nrefs)
	{
		kdprintf(STDERR_FILENO, "wait_fd(%d, %d) = %d\n", fdnum, nrefs, p->pp_ref);
		sys_yield();
	}

	return 0;
}
