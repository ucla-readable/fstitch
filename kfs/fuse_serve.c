#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <lib/dirent.h>
#include <lib/kdprintf.h>
#include <lib/panic.h>
#include <kfs/cfs.h>
#include <kfs/feature.h>
#include <kfs/kfsd.h>
#include <kfs/modman.h>
#include <kfs/fuse_serve.h>
#include <kfs/fuse_serve_inode.h>

// Helpful documentation: FUSE's fuse_lowlevel.h, README, and FAQ
// Helpful debugging options:
// - Enable debug output for fuse_serve_inode (to show inode alloc/free)
// - Enable debug output for fuse_serve
// - Run with the -d flag to see FUSE messages coming in and going out

// TODOs:
// - Why does using a 0s timeout (instead of 1.0) not work? Is this a problem?
// - Send errors to fuse in more situations (and better translate KFS<->FUSE errors)
// - Propagate errors rather than assert() in places where assert() is used for errors that can happen
// - Send negative lookup answers (rather than ENOENT), right?
// - Add support for the other fuse_lowlevel_ops that make sense (write, ...)
// - Implement sched's functionality (limit block wait on kernel communication? allow delays until we get a kernel callback?)
// - Switch off kernel buffer cache for our serves? (direct_io)
// - Be safer; eg call open() only when we should
// - Speedup serve_readdir() when helpful (it runs O(n^2); a slightly more complex O(n) would work)
// - Speedup fuse_serve_inode if helpful; lname_inode() is O(|dir's entries|)
// - Provide mechanism to free up resources upon exit so we don't falsely trigger mem leak detectors?
// - "ls dir; sleep 5; ls dir" (for example), on the 2nd "ls dir", releases a file's inode and then recreates the inode. In this case we give a new inode number. Should we try to reuse the original inode number? (We'll probably need to unique them with the generation field, if so.) (To see this turn on fuse_serve_inode debugging and run the example command.)
// - Support multiple hard links
// - Support more metadata; eg permissions, atime, and mtime
// - Support delayed event response or multiple threads

#define FUSE_SERVE_DEBUG 0

#if FUSE_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

// If we move to multiple fuse_serve instances we can put global fields into
// FUSE's userdata param
static CFS_t * frontend_cfs = NULL;

#define TODOERROR 0

void set_frontend_cfs(CFS_t * cfs)
{
	Dprintf("%s(cfs = %s)\n", __FUNCTION__, modman_name_cfs(cfs));
	frontend_cfs = cfs;
}

CFS_t * get_frontend_cfs(void)
{
	Dprintf("%s() = %s\n", __FUNCTION__, modman_name_cfs(frontend_cfs));
	return frontend_cfs;
}


static int serve_stat(fuse_ino_t ino, struct stat *stbuf)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	const char * name;
	int fid;
	int r;
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;

	if (!(name = inode_fname(ino)))
		return -1;

	r = CALL(frontend_cfs, get_metadata, name, KFS_feature_filetype.id, &type_size, &type.ptr);
	if (r < 0)
	{
		Dprintf("%d:frontend_cfs->get_metadata() = %d\n", __LINE__, r);
		return -1;
	}

	if (*type.type == TYPE_DIR)
	{
		char buf[1024];
		uint32_t basep;
		uint32_t nlinks = 0;

		// FIXME: we should use the same inode->file mapping throughout an
		// inode's life. Using opening by name can break this.
		fid = CALL(frontend_cfs, open, name, 0);
		assert(fid >= 0);

		while ((r = CALL(frontend_cfs, getdirentries, fid, buf, sizeof(buf), &basep)) > 0)
		{
			char * cur = buf;
				while (cur < buf + r)
				{
					nlinks++;
					cur += ((dirent_t *) cur)->d_reclen;
				}
		}

		r = CALL(frontend_cfs, close, fid);
		assert(r >= 0);

		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = nlinks;
	}
	else if (*type.type == TYPE_FILE)
	{
		uint32_t filesize_size;
		union {
			int32_t * filesize;
			void * ptr;
		} filesize;

		r = CALL(frontend_cfs, get_metadata, name, KFS_feature_size.id, &filesize_size, &filesize.ptr);
		if (r < 0)
		{
			Dprintf("%d:frontend_cfs->get_metadata() = %d\n", __LINE__, r);
			goto err;
		}

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1; //	TODO: KFS_feature_nlinks
		stbuf->st_size = (off_t) *filesize.filesize;
		free(filesize.filesize);
	}
	else
	{
		Dprintf("%d:file type %u unknown\n", __LINE__, *type.type);
		goto err;
	}
	stbuf->st_ino = ino;

	free(type.type);
	return 0;

  err:
	free(type.type);
	return -1;
}

static void serve_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	struct stat stbuf;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (serve_stat(ino, &stbuf) == -1)
		fuse_reply_err(req, ENOENT);
	else
		fuse_reply_attr(req, &stbuf, 1.0);
}

static void serve_lookup(fuse_req_t req, fuse_ino_t parent, const char *local_name)
{
	Dprintf("%s(parent_ino = %d, local_name = \"%s\")\n", __FUNCTION__, (int) parent, local_name);
	int r;
	fuse_ino_t ino;
	bool new_ino = 0;
	int fid;
	struct fuse_entry_param e;

	ino = lname_inode(parent, local_name);
	if (ino == FAIL_INO)
	{
		char * full_name = fname(parent, local_name);

		// Check that name exists
		fid = CALL(frontend_cfs, open, full_name, 0);
		free(full_name);
		if (fid < 0)
		{
			if (fid == -12) // FIXME: 12 is E_NOT_FOUND
				fuse_reply_err(req, ENOENT);
			else
				fuse_reply_err(req, TODOERROR);
			return;
		}
		r = CALL(frontend_cfs, close, fid);
		assert(r >= 0);

		r = add_inode(parent, local_name, &ino);
		assert(r == 0);
		new_ino = 1;
	}

	memset(&e, 0, sizeof(e));
	e.ino = ino;
	e.attr_timeout = 1.0;
	e.entry_timeout = 1.0;
	serve_stat(e.ino, &e.attr);

	fuse_reply_entry(req, &e);
}

static void serve_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	Dprintf("%s(ino = %d, nlookup = %u)\n", __FUNCTION__, (int) ino, nlookup);
	remove_inode(ino);
	fuse_reply_none(req);
}

static int read_single_dir(int fid, off_t k, dirent_t * dirent)
{
	Dprintf("%s(fid = %d, k = %lld)\n", __FUNCTION__, fid, k);
	uint32_t basep = 0;
	char buf[sizeof(dirent_t)];
	char * cur = buf;
	off_t dirno = 0;
	int eof = 0;
	int r;

	assert(fid >= 0 && k >= 0 && dirent != NULL);
	memset(buf, 0, sizeof(buf));

	while (dirno <= k)
	{
		uint32_t nbytes;
		cur = buf;

		r = CALL(frontend_cfs, getdirentries, fid, buf, sizeof(buf), &basep);
		if (r == -1) // -E_UNSPECIFIED, should imply eof
		{
			eof = 1;
			break;
		}
		else if (r < 0)
			return r;

		nbytes = r;
		while (dirno < k
		       && ((cur - buf) + ((dirent_t*) cur)->d_reclen) < nbytes)
		{
			cur += ((dirent_t*) cur)->d_reclen;
			dirno++;
		}
		dirno++;
	}

	memcpy(dirent, cur, sizeof(buf));
	return eof;
}

static void serve_opendir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	const char * name;
	int fid;

	name = inode_fname(ino);
	assert(name);
	fid = CALL(frontend_cfs, open, name, 0);
	if (fid < 0)
	{
		// TODO: fid could be E_NOT_FOUND, E_NOT_FOUND, or other
		// TODO: fuse_reply_err(req, ENOTDIR);
		fuse_reply_err(req, -1);
		return;
	}

	fi->fh = (uint64_t) fid;

	fuse_reply_open(req, fi);
}

static void serve_releasedir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d, fid = %d)\n", __FUNCTION__, (int) ino, fid);
	int fid = (int) fi->fh;
	int r;

	r = CALL(frontend_cfs, close, fid);
	if (r < 0)
		fuse_reply_err(req, -1);
}

static void serve_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d, size = %u, off = %lld)\n", __FUNCTION__, (int) ino, size, off);
	int fid = (int) fi->fh;
	uint32_t total_size = 0;
	char * buf = NULL;
	struct stat stbuf;

	if (off == 0)
	{
		const char * name = ".";
		size_t name_len = 1;
		if (total_size + fuse_dirent_size(name_len) <= size)
		{
			total_size += fuse_dirent_size(name_len);
			buf = (char *) realloc(buf, total_size);
			assert(buf);
			memset(&stbuf, 0, sizeof(stbuf));
			stbuf.st_ino = ino;
			fuse_add_dirent(buf, name, &stbuf, ++off);
		}
	}

	if (off == 1)
	{
		const char * name = "..";
		size_t name_len = 2;
		if (total_size + fuse_dirent_size(name_len) <= size)
		{
			size_t oldsize = total_size;
			total_size += fuse_dirent_size(name_len);
			buf = (char *) realloc(buf, total_size);
			assert(buf);
			memset(&stbuf, 0, sizeof(stbuf));
			stbuf.st_ino = inode_parent(ino);
			assert(stbuf.st_ino != FAIL_INO);
			fuse_add_dirent(buf + oldsize, name, &stbuf, ++off);
		}
	}

	while (off >= 2)
	{
		dirent_t dirent;
		size_t oldsize = total_size;
		fuse_ino_t entry_ino;
		int r;

		r = read_single_dir(fid, off - 2, &dirent);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "%d:read_single_dir(%d, %lld, 0x%08x) = %d\n",
					 __LINE__, fid, off - 2, &dirent, r);
			assert(r >= 0);
		}
		if (r == 1)
			break;

		total_size += fuse_dirent_size(dirent.d_namelen);

		if (total_size > size)
		{
			total_size = oldsize;
			break;
		}

		buf = (char *) realloc(buf, total_size);
		assert(buf);

		memset(&stbuf, 0, sizeof(stbuf));
		entry_ino = lname_inode(ino, dirent.d_name);
		if (entry_ino == FAIL_INO)
		{
			r = add_inode(ino, dirent.d_name, &entry_ino);
			assert(r == 0);
		}

		stbuf.st_ino = entry_ino;
		fuse_add_dirent(buf + oldsize, dirent.d_name, &stbuf, ++off);
	}

	fuse_reply_buf(req, buf, total_size);
	free(buf);
}

static void serve_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	uint32_t size;
	void * data;
	uint32_t type;
	int fid;
	int r;

//	else if ((fi->flags & 3) != O_RDONLY)
//		fuse_reply_err(req, EACCES);

	const char * name = inode_fname(ino);
	assert(name);

	r = CALL(frontend_cfs, get_metadata, name, KFS_feature_filetype.id, &size, &data);
	assert(r >= 0);
	type = *((uint32_t*) data);

	if (type == TYPE_DIR)
		fuse_reply_err(req, EISDIR);

	fid = CALL(frontend_cfs, open, name, 0);
	assert(r >= 0);
	fi->fh = (uint64_t) fid;
	
	fuse_reply_open(req, fi);
}

static void serve_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	int fid = (int) fi->fh;
	int r;

	r = CALL(frontend_cfs, close, fid);
	assert(r >= 0);
}

static void serve_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d, fid = %d, size = %u, off = %lld)\n", __FUNCTION__, (int) ino, fid, size, off);
	int fid = (int) fi->fh;
	char * buf;
	int r;

	buf = malloc(size);
	assert(buf);

	r = CALL(frontend_cfs, read, fid, buf, off, size);
	if (r <= 0)
	{
		// TODO: handle -E_EOF?
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	r = fuse_reply_buf(req, buf, r);
	free(buf);
	assert(r >= 0);
	return;
}


static struct fuse_lowlevel_ops serve_oper =
{
	.lookup     = serve_lookup,
	.forget     = serve_forget,
	.getattr    = serve_getattr,
	.opendir    = serve_opendir,
	.releasedir = serve_releasedir,
	.readdir    = serve_readdir,
	.open       = serve_open,
	.release    = serve_release,
	.read	    = serve_read,
};

static struct fuse_args fuse_args;
static char * fuse_mountpoint = NULL;
static int fuse_fd = -1;
static struct fuse_session * fuse_session = NULL;
static bool fuse_signal_handlers_set = 0;

static void fuse_serve_shutdown(void * arg)
{
	if (fuse_session)
	{
		if (fuse_signal_handlers_set)
			fuse_remove_signal_handlers(fuse_session);
		fuse_session_destroy(fuse_session);
	}

	if (fuse_fd != -1)
	{
		close(fuse_fd);
		fuse_unmount(fuse_mountpoint);
	}

	fuse_opt_free_args(&fuse_args);

	inodes_shutdown();
}

int fuse_serve_init(int argc, char ** argv)
{
	struct fuse_chan * channel;
	int r;

	assert(!fuse_mountpoint && fuse_fd == -1 && !fuse_session && !fuse_signal_handlers_set);

	// We can't use FUSE_ARGS_INIT() here so assert we are initing the whole structure
	static_assert(sizeof(fuse_args) == sizeof(argc) + sizeof(argv) + sizeof(int));
	fuse_args.argc = argc;
	fuse_args.argv = argv;
	fuse_args.allocated = 0;

	if ((r = kfsd_register_shutdown_module(fuse_serve_shutdown, NULL)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): kfsd_register_shutdown_module() = %d\n", __FUNCTION__, r);
		return r;
	}

	if ((r = inodes_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): init_inodes() = %d\n", __FUNCTION__, r);
		return r;
	}

	if (fuse_parse_cmdline(&fuse_args, &fuse_mountpoint, NULL, NULL) == -1)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_parse_cmdline() failed\n", __FUNCTION__);
		return -1;
	}

	if ((fuse_fd = fuse_mount(fuse_mountpoint, &fuse_args)) == -1)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_mount() failed\n", __FUNCTION__);
		return -1;
	}

	fuse_session = fuse_lowlevel_new(&fuse_args, &serve_oper, sizeof(serve_oper), NULL);
	if (!fuse_session)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_lowlevel_new() failed\n", __FUNCTION__);
		return -1;
	}

	if (fuse_set_signal_handlers(fuse_session) == -1)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_set_signal_handlers() failed\n", __FUNCTION__);
		return -1;
	}
	fuse_signal_handlers_set = 1;

	if (!(channel = fuse_kern_chan_new(fuse_fd)))
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_kern_chan_new() failed\n", __FUNCTION__);
		return -1;
	}

	fuse_session_add_chan(fuse_session, channel);

	return 0;
}

int fuse_serve_loop(void)
{
	Dprintf("%s()\n", __FUNCTION__);
	assert(fuse_session);
	assert(frontend_cfs);
	return fuse_session_loop(fuse_session);
}
