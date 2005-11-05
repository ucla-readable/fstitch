#include <inc/fs.h>
#include <inc/string.h>
#include <inc/lib.h>
#include <lib/dirent.h>

#define debug 0

static int file_close(struct Fd* fd);
static ssize_t file_read(struct Fd* fd, void* buf, size_t n, off_t offset);
static int file_read_map(struct Fd* fd, off_t offset, void** blk);
static ssize_t file_write(struct Fd* fd, const void* buf, size_t n, off_t offset);
static ssize_t file_getdirentries(struct Fd* fd, void* buf, int nbytes, uint32_t* basep);
static int file_stat(struct Fd* fd, struct Stat* stat);
static int file_trunc(struct Fd* fd, off_t newsize);

struct Dev devfile =
{
	.dev_id =	'f',
	.dev_name =	"file",
	.dev_read =	file_read,
	.dev_read_nb =	file_read,
	.dev_read_map =	file_read_map,
	.dev_write =	file_write,
	.dev_getdirentries = file_getdirentries,
	.dev_close =	file_close,
	.dev_stat =	file_stat,
	.dev_trunc =	file_trunc
};

// Helper functions for file access
static int fmap(struct Fd* fd, off_t oldsize, off_t newsize);
static int funmap(struct Fd* fd, off_t oldsize, off_t newsize, bool dirty);

// Open a file (or directory),
// returning the file descriptor index on success, < 0 on failure.
int
jfs_open(const char* path, int mode)
{
	// Find an unused file descriptor page using fd_alloc.
	// Then send a message to the file server to open a file
	// using a function in fsipc.c.
	// (fd_alloc does not allocate a page, it just returns an
	// unused fd address.  Do you need to allocate a page?  Look
	// at fsipc.c if you aren't sure.)
	// Then map the file data (you may find fmap() helpful).
	// Return the file descriptor index.
	// If any step fails, use fd_close to free the file descriptor.
	struct Fd * fd;
	int i, r;
	
	i = fd_alloc(&fd);
	if(i < 0)
		return i;
	
	r = fsipc_open(path, mode, fd);
	if(r)
		return r;
	
	r = fmap(fd, 0, fd->fd_file.file.f_size);
	if(r)
	{
		fd_close(fd, 0);
		return r;
	}

	return i;
}

// Clean up a file-server file descriptor.
// This function is called by fd_close.
static int
file_close(struct Fd* fd)
{
	// Unmap any data mapped for the file,
	// then tell the file server that we have closed the file
	// (to free up its resources).

	funmap(fd, fd->fd_file.file.f_size, 0, 1);
	return fsipc_close(fd->fd_file.id);
}

// Read 'n' bytes from 'fd' at the current seek position into 'buf'.
// Since files are memory-mapped, this amounts to a memcpy()
// surrounded by a little red tape to handle the file size and seek pointer.
static ssize_t
file_read(struct Fd* fd, void* buf, size_t n, off_t offset)
{
	size_t size;

	// avoid reading past the end of file
	size = fd->fd_file.file.f_size;
	if (offset > size)
		return 0;
	if (offset + n > size)
		n = size - offset;

	// read the data by copying from the file mapping
	memcpy(buf, fd2data(fd) + offset, n);
	return n;
}

// Find the page that maps the file block starting at 'offset',
// and store its address in '*blk'.
static int
file_read_map(struct Fd* fd, off_t offset, void** blk)
{
	char* va;

	va = fd2data(fd) + offset;
	if (offset >= MAXFILESIZE)
		return -E_NO_DISK;
	if (!(vpd[PDX(va)] & PTE_P) || !(vpt[VPN(va)] & PTE_P))
		return -E_NO_DISK;
	*blk = (void*) va;
	return 0;
}

// Write 'n' bytes from 'buf' to 'fd' at the current seek position.
static ssize_t
file_write(struct Fd* fd, const void* buf, size_t n, off_t offset)
{
	int r;
	size_t tot;

	// don't write past the maximum file size
	tot = offset + n;
	if (tot > MAXFILESIZE)
		return -E_NO_DISK;

	// increase the file's size if necessary
	if (tot > fd->fd_file.file.f_size) {
		if ((r = file_trunc(fd, tot)) < 0)
			return r;
	}

	// write the data
	memcpy(fd2data(fd) + offset, buf, n);
	return n;
}

static ssize_t
file_getdirentries(struct Fd* fd, void* buf, int nbytes, uint32_t* basep)
{
	int r = 0, nbytes_read = 0;

	fd->fd_offset = *basep;

	while (nbytes_read < nbytes)
	{
		int i;
		struct File f;
		uint16_t namelen, reclen;
		dirent_t * ent = (dirent_t *) (buf + nbytes_read);

		// Read a dirent
		if ((r = file_read(fd, &f, sizeof(struct File), fd->fd_offset)) <= 0)
			break;
		assert(r == sizeof(struct File));
		fd->fd_offset += sizeof(struct File);
		if (!f.f_name[0])
		{
			*basep += sizeof(struct File);
			continue;
		}

		namelen = strlen(f.f_name);
		namelen = MIN(namelen, sizeof(ent->d_name) - 1);
		reclen = sizeof(*ent) - sizeof(ent->d_name) + namelen + 1;
		
		// Make sure it's not too long
		if(nbytes_read + reclen > nbytes)
			break;
		
		// Pseudo unique fileno generator
		ent->d_fileno = 0;
		for (i = 0; f.f_name[i]; i++)
		{
			ent->d_fileno *= 5;
			ent->d_fileno += f.f_name[i];
		}

		// Store the dirent into *ent
		ent->d_filesize = f.f_size;
		ent->d_reclen = reclen;
		ent->d_type = f.f_type;
		ent->d_namelen = namelen;
		strncpy(ent->d_name, f.f_name, sizeof(ent->d_name));

		// Update position variables
		nbytes_read += reclen;
		*basep += sizeof(struct File);
	}

	return nbytes_read ? nbytes_read : (r < 0) ? r : 0;
}

static int
file_stat(struct Fd* fd, struct Stat* st)
{
	strcpy(st->st_name, fd->fd_file.file.f_name);
	st->st_size = fd->fd_file.file.f_size;
	st->st_isdir = (fd->fd_file.file.f_type == FTYPE_DIR);
	return 0;
}

// Truncate or extend an open file to 'size' bytes
static int
file_trunc(struct Fd* fd, off_t newsize)
{
	int r;
	off_t oldsize;
	uint32_t fileid;

	if (newsize > MAXFILESIZE)
		return -E_NO_DISK;

	fileid = fd->fd_file.id;
	oldsize = fd->fd_file.file.f_size;
	if ((r = fsipc_set_size(fileid, newsize)) < 0)
		return r;
	assert(fd->fd_file.file.f_size == newsize);

	if ((r = fmap(fd, oldsize, newsize)) < 0)
		return r;
	funmap(fd, oldsize, newsize, 0);
	
	return 0;
}

// Call the file system server to obtain and map file pages
// when the size of the file as mapped in our memory increases.
// Harmlessly does nothing if oldsize >= newsize.
// Returns 0 on success, < 0 on error.
// If there is an error, unmaps any newly allocated pages.
static int
fmap(struct Fd* fd, off_t oldsize, off_t newsize)
{
	size_t i;
	char* va;
	int r;

	va = fd2data(fd);
	for (i = ROUNDUP32(oldsize, PGSIZE); i < newsize; i += PGSIZE) {
		if ((r = fsipc_map(fd->fd_file.id, i, va + i)) < 0) {
			// unmap anything we may have mapped so far
			funmap(fd, i, oldsize, 0);
			return r;
		}
	}
	return 0;
}

// Unmap any file pages that no longer represent valid file pages
// when the size of the file as mapped in our address space decreases.
// Harmlessly does nothing if newsize >= oldsize.
static int
funmap(struct Fd* fd, off_t oldsize, off_t newsize, bool dirty)
{
	size_t i;
	char* va;
	int r, ret;

	va = fd2data(fd);

	// Check vpd to see if anything is mapped.
	if (!(vpd[VPD(va)] & PTE_P))
		return 0;

	ret = 0;
	for (i = ROUNDUP32(newsize, PGSIZE); i < oldsize; i += PGSIZE)
		if (vpt[VPN(va + i)] & PTE_P) {
			if (dirty
			    && (vpt[VPN(va + i)] & PTE_D)
			    && (r = fsipc_dirty(fd->fd_file.id, i)) < 0)
				ret = r;
			sys_page_unmap(0, va + i);
		}
	return ret;
}

// Delete a file
int
jfs_remove(const char* path)
{
	return fsipc_remove(path);
}

// Synchronize disk with buffer cache
int
jfs_sync(void)
{
	return fsipc_sync();
}

uint32_t
jfs_disk_avail_space(void)
{
	return fsipc_avail_space();
}

int
jfs_shutdown(void)
{
	return fsipc_shutdown();
}
