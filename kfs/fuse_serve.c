#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <lib/dirent.h>
#include <lib/fcntl.h>
#include <lib/jiffies.h>
#include <lib/kdprintf.h>
#include <lib/panic.h>
#include <kfs/cfs.h>
#include <kfs/feature.h>
#include <kfs/kfsd.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/fuse_serve.h>
#include <kfs/fuse_serve_inode.h>

// Helpful documentation: FUSE's fuse_lowlevel.h, README, and FAQ
// Helpful debugging options:
// - Enable debug output for fuse_serve_inode (to show inode alloc/free)
// - Enable debug output for fuse_serve
// - Run with the -d flag to see FUSE messages coming in and going out

// TODOs:
// - Why does FUSE stop responding if a user throws a slew of work at it?
// - Why does "echo hello > existing_file" truncate existing_file but fail to write?
// - Why does using a 0s timeout (instead of 1.0) not work? Is this a problem?
// - Send errors to fuse in more situations (and better translate KFS<->FUSE errors)
// - Propagate errors rather than assert() in places where assert() is used for errors that can happen
// - Send negative lookup answers (rather than ENOENT), right?
// - Add support for the other fuse_lowlevel_ops that make sense
// - Switch off kernel buffer cache for ourself? (direct_io)
// - Be safer; eg call open() only when we should
// - Speedup serve_readdir() when helpful (it runs O(n^2); a slightly more complex O(n) would work)
// - Speedup fuse_serve_inode if helpful; lname_inode() is O(|dir's entries|)
// - Provide mechanism to free up resources upon exit so we don't falsely trigger mem leak detectors?
// - "ls dir; sleep 5; ls dir" (for example), on the 2nd "ls dir", releases a file's inode and then recreates the inode. In this case we give a new inode number. Should we try to reuse the original inode number? (We'll probably need to unique them with the generation field, if so.) (To see this turn on fuse_serve_inode debugging and run the example command.)
// - Support multiple hard links (how do we deal with open() and opendir()?)
// - Support more metadata; eg permissions, atime, and mtime
// - Support delayed event response or multiple threads

#define FUSE_SERVE_DEBUG 0

#if FUSE_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define TODOERROR 1
#define FUSE_ERR_SUCCESS 0
#define STDTIMEOUT 1.0


// If we move to multiple fuse_serve instances we can put global fields into
// FUSE's userdata param
static CFS_t * frontend_cfs = NULL;

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


/*
static void serve_statfs(fuse_req_t req)
{
	Dprintf("%s()\n", __FUNCTION__);
	struct statvfs st;
	int r;

	// See /usr/include/bits/statvfs.h
	// Can we set just some of these?
	// What does each field mean?
	st.f_bsize;
	st.f_frsize;
	st.f_blocks;
	st.f_bfree;
	st.f_bavail;
	st.f_files;
	st.f_ffree;
	st.f_favail;
	st.f_flag;
	st.f_namemax;

	r = fuse_reply_statfs(req, &st);
	assert(!r);
}
*/

static int fill_stat(fuse_ino_t ino, struct stat * stbuf)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
	const char * full_name;
	int fid;
	int r;
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;

	// INODE TODO: make full_name a parameter
	if (!(full_name = inode_fname(ino)))
		return -1;

	r = CALL(frontend_cfs, get_metadata, full_name, KFS_feature_filetype.id, &type_size, &type.ptr);
	if (r < 0)
	{
		Dprintf("%d:frontend_cfs->get_metadata() = %d\n", __LINE__, r);
		return -1;
	}

	if (*type.type == TYPE_DIR)
	{
		char buf[1024];
		uint32_t basep;
		uint32_t nlinks = 2;

		// FIXME: we should use the same inode->file mapping throughout an
		// inode's life. Using opening by name can break this.
		fid = CALL(frontend_cfs, open, full_name, 0);
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

		r = CALL(frontend_cfs, get_metadata, full_name, KFS_feature_size.id, &filesize_size, &filesize.ptr);
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
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
	struct stat stbuf;
	int r;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (fill_stat(ino, &stbuf) == -1)
	{
		r = fuse_reply_err(req, ENOENT);
		assert(!r);
	}
	else
	{
		r = fuse_reply_attr(req, &stbuf, STDTIMEOUT);
		assert(!r);
	}
}

static void serve_setattr(fuse_req_t req, fuse_ino_t ino, struct stat * attr,
                          int to_set, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, to_set = %d)\n", __FUNCTION__, ino, to_set);
	int r;
	int fid;
	uint32_t size;

	if (to_set != FUSE_SET_ATTR_SIZE)
	{
		r = fuse_reply_err(req, ENOSYS);
		assert(!r);
		return;
	}

	if (fi)
		fid = (int) fi->fh;
	else
	{
		// INODE TODO: get full_name from fi->fh
		const char * full_name = inode_fname(ino);
		assert(full_name);
		fid = CALL(frontend_cfs, open, full_name, 0);
		if (fid < 0)
		{
			r = fuse_reply_err(req, TODOERROR);
			assert(!r);
			return;
		}
	}

	size = (uint32_t) attr->st_size;
	assert(size == attr->st_size);
	Dprintf("\tsize = %u\n", size);

	r = CALL(frontend_cfs, truncate, fid, size);

	if (!fi)
	{
		if (CALL(frontend_cfs, close, fid) < 0)
		{
			r = fuse_reply_err(req, TODOERROR);
			assert(!r);
			return;
		}
	}

	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_attr(req, attr, STDTIMEOUT);
	assert(!r);
}

static void serve_lookup(fuse_req_t req, fuse_ino_t parent, const char *local_name)
{
	Dprintf("%s(parent_ino = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
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
		full_name = NULL;
		if (fid < 0)
		{
			if (fid == -12) // FIXME: 12 is E_NOT_FOUND
				r = fuse_reply_err(req, ENOENT);
			else
				r = fuse_reply_err(req, TODOERROR);
			assert(!r);
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
	e.attr_timeout = STDTIMEOUT;
	e.entry_timeout = STDTIMEOUT;
	r = fill_stat(e.ino, &e.attr);
	assert(r >= 0);

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static void serve_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	Dprintf("%s(ino = %lu, nlookup = %lu)\n", __FUNCTION__, ino, nlookup);
	remove_inode(ino);
	fuse_reply_none(req);
}

static void serve_mkdir(fuse_req_t req, fuse_ino_t parent,
                        const char * local_name, mode_t mode)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	fuse_ino_t ino;
	char * full_name;
	struct fuse_entry_param e;
	int r;

	full_name = fname(parent, local_name);
	assert(full_name);

	r = CALL(frontend_cfs, mkdir, full_name);
	free(full_name);
	full_name = NULL;
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = add_inode(parent, local_name, &ino);
	assert(r == 0);

	memset(&e, 0, sizeof(e));
	e.ino = ino;
	e.attr_timeout = STDTIMEOUT;
	e.entry_timeout = STDTIMEOUT;
	// ignore mode parameter for now
	r = fill_stat(e.ino, &e.attr);
	assert(r >= 0);

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static int create(fuse_req_t req, fuse_ino_t parent, const char * local_name,
                  mode_t mode, fuse_ino_t * ino, struct fuse_entry_param * e)
{
	int r;
	const char * full_name;
	int fid;

	r = add_inode(parent, local_name, ino);
	if (r == -1)
	{
		r = fuse_reply_err(req, TODOERROR); // exists
		assert(!r);
		return -1;
	}
	else if (r == -ENOMEM)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return -1;
	}
	else if (r < 0)
		assert(0);

	// INODE TODO: use fname()
	full_name = inode_fname(*ino);
	assert(full_name);

	fid = CALL(frontend_cfs, open, full_name, O_CREAT);
	if (fid < 0)
	{
		remove_inode(*ino);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return -1;
	}

	memset(e, 0, sizeof(*e));
	e->ino = *ino;
	e->attr_timeout = STDTIMEOUT;
	e->entry_timeout = STDTIMEOUT;
	// ignore mode for now
	r = fill_stat(e->ino, &e->attr);
	assert(r >= 0);

	return fid;
}

static void serve_create(fuse_req_t req, fuse_ino_t parent,
                         const char * local_name, mode_t mode,
                         struct fuse_file_info * fi)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__,
	        parent, local_name);
	fuse_ino_t ino;
	int fid;
	int r;
	struct fuse_entry_param e;

	fid = create(req, parent, local_name, mode, &ino, &e);
	if (fid < 0)
		return;

	fi->fh = (uint64_t) fid;

	r = fuse_reply_create(req, &e, fi);
	assert(!r);
}

static void serve_mknod(fuse_req_t req, fuse_ino_t parent,
                        const char * local_name, mode_t mode, dev_t rdev)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	fuse_ino_t ino;
	int fid;
	int r;
	struct fuse_entry_param e;

	if (!(mode & S_IFREG))
	{
		r = fuse_reply_err(req, ENOSYS);
		assert(!r);
		return;
	}

	fid = create(req, parent, local_name, mode, &ino, &e);
	if (fid < 0)
		return;

	fid = CALL(frontend_cfs, close, fid);
	assert(fid >= 0);

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static void serve_unlink(fuse_req_t req, fuse_ino_t parent, const char * local_name)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__,
	        parent, local_name);
	char * full_name;
	int r;

	full_name = fname(parent, local_name);
	assert(full_name);

	r = CALL(frontend_cfs, unlink, full_name);
	free(full_name);
	full_name = NULL;
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
	
}

static void serve_rmdir(fuse_req_t req, fuse_ino_t parent, const char * local_name)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	char * full_name;
	int r;

	full_name = fname(parent, local_name);
	assert(full_name);

	r = CALL(frontend_cfs, rmdir, full_name);
	free(full_name);
	full_name = NULL;
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_rename(fuse_req_t req,
                         fuse_ino_t old_parent, const char * old_local_name,
                         fuse_ino_t new_parent, const char * new_local_name)
{
	Dprintf("%s(oldp = %lu, oldln = \"%s\", newp = %lu, newln = \"%s\")\n",
	        __FUNCTION__, old_parent, old_local_name, new_parent, new_local_name);
	char * old_full_name;
	char * new_full_name;
	fuse_ino_t old_ino;
	int r;

	old_full_name = fname(old_parent, old_local_name);
	assert(old_full_name);
	new_full_name = fname(new_parent, new_local_name);
	assert(new_local_name);

	r = CALL(frontend_cfs, rename, old_full_name, new_full_name);
	free(old_full_name);
	old_full_name = NULL;
	free(new_full_name);
	new_full_name = NULL;
	if (r < 0)
	{
		// TODO: case -E_FILE_EXISTS: should we allow overwriting?
		// TODO: case -E_INVAL: might mean files are on different filesystems
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	// Remove old_ino from inode cache, fuse won't tell us to forget
	old_ino = lname_inode(old_parent, old_local_name);
	if (old_ino != FAIL_INO)
		remove_inode(old_ino);

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
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

	memcpy(dirent, cur, ((dirent_t*) cur)->d_reclen);
	return eof;
}

static void ssync(fuse_req_t req, fuse_ino_t ino, int datasync,
                  struct fuse_file_info * fi)
{
	const char * full_name;
	int r;

	// INODE TODO: get full_name from fi->fh
	full_name = inode_fname(ino);
	assert(full_name);
	// ignore datasync
	r = CALL(frontend_cfs, sync, full_name);
	if (r == -18)
	{
		r = fuse_reply_err(req, TODOERROR); // device busy
		assert(!r);
		return;
	}
	else if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}
	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                        struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, datasync = %d)\n", __FUNCTION__, ino, datasync);
	ssync(req, ino, datasync, fi);
}

static void serve_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                           struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, datasync = %d)\n", __FUNCTION__, ino, datasync);
	ssync(req, ino, datasync, fi);	
}

static void serve_opendir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
	const char * full_name;
	int fid;
	int r;

	// INODE TODO: how could we avoid ino -> full_name?
	full_name = inode_fname(ino);
	assert(full_name);
	fid = CALL(frontend_cfs, open, full_name, 0);
	if (fid < 0)
	{
		// TODO: fid could be E_NOT_FOUND, E_NOT_FOUND, or other
		// TODO: fuse_reply_err(req, ENOTDIR);
		r = fuse_reply_err(req, -1);
		assert(!r);
		return;
	}

	fi->fh = (uint64_t) fid;

	r = fuse_reply_open(req, fi);
	assert(!r);
}

static void serve_releasedir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi)
{
	int fid = (int) fi->fh;
	int r;
	Dprintf("%s(ino = %lu, fid = %d)\n", __FUNCTION__, ino, fid);

	r = CALL(frontend_cfs, close, fid);
	if (r < 0)
	{
		r = fuse_reply_err(req, -1);
		assert(!r);
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
	int fid = (int) fi->fh;
	uint32_t total_size = 0;
	char * buf = NULL;
	struct stat stbuf;
	int r;
	Dprintf("%s(ino = %lu, size = %u, off = %lld)\n", __FUNCTION__, ino, size, off);

	if (off == 0)
	{
		const char * local_name = ".";
		size_t local_name_len = 1;
		if (total_size + fuse_dirent_size(local_name_len) <= size)
		{
			total_size += fuse_dirent_size(local_name_len);
			buf = (char *) realloc(buf, total_size);
			assert(buf);
			memset(&stbuf, 0, sizeof(stbuf));
			stbuf.st_ino = ino;
			fuse_add_dirent(buf, local_name, &stbuf, ++off);
		}
	}

	if (off == 1)
	{
		const char * local_name = "..";
		size_t local_name_len = 2;
		if (total_size + fuse_dirent_size(local_name_len) <= size)
		{
			size_t oldsize = total_size;
			total_size += fuse_dirent_size(local_name_len);
			buf = (char *) realloc(buf, total_size);
			assert(buf);
			memset(&stbuf, 0, sizeof(stbuf));
			stbuf.st_ino = inode_parent(ino);
			assert(stbuf.st_ino != FAIL_INO);
			fuse_add_dirent(buf + oldsize, local_name, &stbuf, ++off);
		}
	}

	while (off >= 2)
	{
		dirent_t dirent;
		size_t oldsize = total_size;
		fuse_ino_t entry_ino;

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

	r = fuse_reply_buf(req, buf, total_size);
	assert(!r);
	free(buf);
}

static void serve_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
	uint32_t size;
	void * data;
	uint32_t type;
	int fid;
	int r;

//	else if ((fi->flags & 3) != O_RDONLY)
//		fuse_reply_err(req, EACCES);

	// INODE TODO: how could we avoid ino -> full_name?
	const char * full_name = inode_fname(ino);
	assert(full_name);

	r = CALL(frontend_cfs, get_metadata, full_name, KFS_feature_filetype.id, &size, &data);
	assert(r >= 0);

	type = *((uint32_t*) data);
	free(data);
	data = NULL;

	if (type == TYPE_DIR)
	{
		r = fuse_reply_err(req, EISDIR);
		assert(!r);
		return;
	}

	fid = CALL(frontend_cfs, open, full_name, 0);
	assert(r >= 0);
	fi->fh = (uint64_t) fid;
	
	r = fuse_reply_open(req, fi);
	assert(!r);
}

static void serve_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
	int fid = (int) fi->fh;
	int r;

	r = CALL(frontend_cfs, close, fid);
	assert(r >= 0);
	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
	int fid = (int) fi->fh;
	uint32_t offset = off;
	char * buf;
	int r;
	Dprintf("%s(ino = %lu, fid = %d, size = %u, off = %lld)\n", __FUNCTION__, ino, fid, size, off);

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	buf = malloc(size);
	assert(buf);

	r = CALL(frontend_cfs, read, fid, buf, off, size);
	if (r <= 0)
	{
		// TODO: handle -E_EOF?
		r = fuse_reply_buf(req, NULL, 0);
		assert(!r);
		return;
	}

	r = fuse_reply_buf(req, buf, r);
	free(buf);
	assert(r >= 0);
	return;
}

static void serve_write(fuse_req_t req, fuse_ino_t ino, const char * buf,
                        size_t size, off_t off, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, size = %u, off = %lld, buf = \"%s\")\n",
	        __FUNCTION__, ino, size, off, buf);
	uint32_t offset = off;
	int fid;
	int nbytes;
	int r;

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	fid = (int) fi->fh;

	static_assert(sizeof(uint32_t) == sizeof(size));
	nbytes = CALL(frontend_cfs, write, fid, buf, offset, size);
	if (nbytes < size)
	{
		r = fuse_reply_write(req, TODOERROR);
		assert(!r);
		return;
	}
	
	r = fuse_reply_write(req, nbytes);
	assert(!r);
}


static struct fuse_lowlevel_ops serve_oper =
{
//	.statfs     = serve_statfs,
	.lookup     = serve_lookup,
	.forget     = serve_forget,
	.getattr    = serve_getattr,
	.setattr    = serve_setattr,
	.create     = serve_create,
	.mknod      = serve_mknod,
	.mkdir      = serve_mkdir,
	.unlink     = serve_unlink,
	.rmdir      = serve_rmdir,
	.rename     = serve_rename,
	.opendir    = serve_opendir,
	.releasedir = serve_releasedir,
	.fsyncdir   = serve_fsyncdir,
	.readdir    = serve_readdir,
	.open       = serve_open,
	.release    = serve_release,
	.fsync      = serve_fsync,
	.read	    = serve_read,
	.write      = serve_write,
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


// Return end - start
struct timeval time_elapsed(struct timeval start, struct timeval end)
{
	struct timeval diff;

	assert(start.tv_sec < end.tv_sec
	       || (start.tv_sec == end.tv_sec && start.tv_usec <= end.tv_usec));

	diff.tv_sec = end.tv_sec - start.tv_sec;
	if (end.tv_usec > start.tv_usec)
		diff.tv_usec = end.tv_usec - start.tv_usec;
	else {
		diff.tv_sec--;
		diff.tv_usec = (1000000 - start.tv_usec) + end.tv_usec;
	}
	return diff;
}

// Return MAX(remaining - elapsed, 0)
struct timeval time_subtract(struct timeval remaining, struct timeval elapsed)
{
	struct timeval n;
	if (remaining.tv_sec < elapsed.tv_sec
	    || (remaining.tv_sec == elapsed.tv_sec
	        && remaining.tv_usec <= elapsed.tv_usec))
		n.tv_sec = n.tv_usec = 0;
	else
	{
		n.tv_sec = remaining.tv_sec - elapsed.tv_sec;
		if (remaining.tv_usec > elapsed.tv_usec)
			n.tv_usec = remaining.tv_usec - elapsed.tv_usec;
		else {
			n.tv_sec--;
			n.tv_usec = (1000000 - elapsed.tv_usec) + remaining.tv_usec;
		}
	}
	return n;
}

// Return the amount of time to wait between sched_iteration() calls
static struct timeval fuse_serve_timeout(void)
{
	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000000/HZ };
	return tv;
}

int fuse_serve_loop(void)
{
	Dprintf("%s()\n", __FUNCTION__);
	assert(fuse_session);
	assert(frontend_cfs);

	// Adapted from FUSE's lib/fuse_loop.c to also support sched callbacks

    int res = 0;
    struct fuse_chan *ch = fuse_session_next_chan(fuse_session, NULL);
    size_t bufsize = fuse_chan_bufsize(ch);
    char *buf = (char *) malloc(bufsize);
    if (!buf) {
        fprintf(stderr, "fuse: failed to allocate read buffer\n");
        return -1;
    }
	int fd = fuse_chan_fd(ch);
	struct timeval tv = fuse_serve_timeout();

    while (!fuse_session_exited(fuse_session)) {
		fd_set rfds;
		int r;
		struct timeval it_start, it_end;

		fd = fuse_chan_fd(ch);
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		r = select(fd+1, &rfds, NULL, NULL, &tv);

		if (r == 0 && !FD_ISSET(fd, &rfds)) {
			sched_iteration();
			tv = fuse_serve_timeout();
		} else if (r < 0) {
			if (errno != EINTR)
				perror("select");
			tv = fuse_serve_timeout(); // tv may have become undefined
		} else {
			r = gettimeofday(&it_start, NULL);
			if (r == -1) {
				perror("gettimeofday");
				break;
			}

			r = fuse_chan_receive(ch, buf, bufsize);
			if (r) {
				if (r == -1)
					break;
				fuse_session_process(fuse_session, buf, r, ch);
			}

			r = gettimeofday(&it_end, NULL);
			if (r == -1) {
				perror("gettimeofday");
				break;
			}
			tv = time_subtract(tv, time_elapsed(it_start, it_end));
		}
    }

    free(buf);
    fuse_session_reset(fuse_session);
    return res;
}
