#include <inc/lib.h>
#include <inc/fd.h>
#include <inc/cfs_ipc_client.h>
#include <kfs/feature.h>
#include <kfs/lfs.h>

static int kpl_close(struct Fd* fd);
static ssize_t kpl_read(struct Fd* fd, void* buf, size_t n, off_t offset);
static ssize_t kpl_write(struct Fd* fd, const void* buf, size_t n, off_t offset);
static int kpl_stat(struct Fd* fd, struct Stat* stat);
static int kpl_trunc(struct Fd* fd, off_t newsize);

struct Dev devkpl =
{
	.dev_id =	'k',
	.dev_name =	"kpl",
	.dev_read =	kpl_read,
	.dev_read_nb =	kpl_read,
	.dev_write =	kpl_write,
	.dev_close =	kpl_close,
	.dev_stat =	kpl_stat,
	.dev_trunc =	kpl_trunc
};

// Open a file (or directory),
// returning the file descriptor index on success, < 0 on failure.
int kpl_open(const char* path, int mode)
{
	struct Fd * fd;
	char * namecopy;
	int i, r;
	
	i = fd_alloc(&fd);
	if(i < 0)
		return i;
	
	/* unlike the original JOS filesystem server, which allocates the page
	 * for the struct Fd and sends it to the client, we allocate the page
	 * in the client and send it to the server */
	/* FIXME? this opens the way for us to send the same page to the file server
	 * for different open requests, thus preventing the file server from ever
	 * cleaning the data up as the reference count will always be > 1 */
	r = sys_page_alloc(0, fd, PTE_SHARE | PTE_U | PTE_W | PTE_P);
	if(r)
		return r;
	
	namecopy = (char *) fd2data(fd);
	r = sys_page_alloc(0, namecopy, PTE_SHARE | PTE_U | PTE_W | PTE_P);
	if(r)
	{
		sys_page_unmap(0, fd);
		return r;
	}
	/*store the file name for this file descriptor */
	strncpy(namecopy, path, SCFSMAXNAMELEN);
	
	r = cfs_open(path, mode, fd);
	if(r < 0)
	{
		sys_page_unmap(0, namecopy);
		sys_page_unmap(0, fd);
		return r;
	}
	
	fd->fd_dev_id = devkpl.dev_id;
	fd->fd_offset = 0;
	fd->fd_omode = mode;
	fd->fd_kpl.fid = r;
	
	return i;
}

// Clean up a file-server file descriptor.
// This function is called by fd_close.
static int kpl_close(struct Fd* fd)
{
	sys_page_unmap(0, fd2data(fd));
	return cfs_close(fd->fd_kpl.fid);
}

// Read 'n' bytes from 'fd' at the current seek position into 'buf'.
static ssize_t kpl_read(struct Fd* fd, void* buf, size_t n, off_t offset)
{
	return cfs_read(fd->fd_kpl.fid, offset, n, buf);
}

// Write 'n' bytes from 'buf' to 'fd' at the current seek position.
static ssize_t kpl_write(struct Fd* fd, const void* buf, size_t n, off_t offset)
{
	return cfs_write(fd->fd_kpl.fid, offset, n, buf);
}

/* warning: not multithread safe! */
static struct Scfs_metadata kpl_stat_md;
static int kpl_stat(struct Fd* fd, struct Stat* st)
{
	int r;

	/* use the stored file name for this file descriptor */
	strncpy(st->st_name, fd2data(fd), MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	r = cfs_get_metadata(st->st_name, KFS_feature_size.id, &kpl_stat_md);
	if (r < 0) return r;
	st->st_size = *(off_t *) &kpl_stat_md.data;
	r = cfs_get_metadata(st->st_name, KFS_feature_filetype.id, &kpl_stat_md);
	if (r < 0) return r;
	st->st_isdir = (*(int *) &kpl_stat_md.data == TYPE_DIR);
	return 0;
}

// Truncate or extend an open file to 'size' bytes
static int kpl_trunc(struct Fd* fd, off_t newsize)
{
	return cfs_truncate(fd->fd_kpl.fid, newsize);
}

// Delete a file
int kpl_remove(const char* path)
{
	return cfs_unlink(path);
}

// Synchronize disk with buffer cache
int kpl_sync(void)
{
	return cfs_sync(NULL);
}

int kpl_shutdown(void)
{
	return cfs_shutdown();
}

int kpl_link(const char * oldname, const char * newname)
{
	return cfs_link(oldname, newname);
}

int kpl_rename(const char * oldname, const char * newname)
{
	return cfs_rename(oldname, newname);
}

int kpl_mkdir(const char * name)
{
	return cfs_mkdir(name);
}

int kpl_rmdir(const char * name)
{
	return cfs_rmdir(name);
}
