#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <lib/dirent.h>
#include <lib/fcntl.h>
#include <lib/jiffies.h>
#include <lib/kdprintf.h>
#include <lib/panic.h>
#include <lib/hash_set.h>
#include <inc/error.h>
#include <kfs/cfs.h>
#include <kfs/feature.h>
#include <kfs/kfsd.h>
#include <kfs/sync.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/fuse_serve.h>
#include <kfs/fuse_serve_mount.h>

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
// - Add support for the other fuse_lowlevel_ops that make sense
// - Switch off kernel buffer cache for ourself? (direct_io)
// - Be safer; eg call open() only when we should
// - Speedup serve_readdir() when helpful (it runs O(n^2); a slightly more complex O(n) would work)
// - Speedup fuse_serve_inode if helpful; lname_inode() is O(|dir's entries|)
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

static CFS_t * root_cfs = NULL;
static bool serving = 0;

static int shutdown_pipe[2];

static size_t channel_buf_len = 0;
static char * channel_buf = NULL;
static int remove_activity; // remove activity fd for fuse_serve_mount


fdesc_t * fi_get_fdesc(struct fuse_file_info * fi)
{
	static_assert(sizeof(fdesc_t *) == sizeof(uint32_t));
	return (fdesc_t *) (uint32_t) fi->fh;
}

void fi_set_fdesc(struct fuse_file_info * fi, fdesc_t * fdesc)
{
	static_assert(sizeof(fdesc_t *) == sizeof(uint32_t));
	fi->fh = (uint64_t) (uint32_t) fdesc;
}

int fuse_serve_add_mount(const char * path, CFS_t * cfs)
{
	int r;
	Dprintf("%s(\"%s\", %s)\n", __FUNCTION__, path, modman_name_cfs(cfs));
	// We could easily allow mount adds from within sched callbacks if this
	// becomes useful.
	// With a good bit of work we can probably allow mount adds from
	// within fuse requests.
	if (serving)
		return -E_BUSY;

	if (!cfs)
		return -E_INVAL;

	if (!strcmp("", path) || !strcmp("/", path))
	{
		if ((r = fuse_serve_mount_set_root(cfs)) < 0)
			return r;
		root_cfs = cfs;
		return 0;
	}

	return fuse_serve_mount_add(cfs, path);
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

// Return the fuse_ino_t corresponding to the given request's inode_t
static fuse_ino_t cfsfuseino(fuse_req_t req, inode_t cfs_ino)
{
	inode_t root_cfs_ino = ((mount_t *) fuse_req_userdata(req))->root_ino;
	if (cfs_ino == root_cfs_ino)
		return FUSE_ROOT_ID;
	else if (cfs_ino == FUSE_ROOT_ID)
		return (fuse_ino_t) root_cfs_ino;
	else
		return (fuse_ino_t) cfs_ino;
}

// Return the request's inode_t corresponding to the fues_ino_t
static inode_t fusecfsino(fuse_req_t req, fuse_ino_t fuse_ino)
{
	inode_t root_cfs_ino = ((mount_t *) fuse_req_userdata(req))->root_ino;
	if (fuse_ino == root_cfs_ino)
		return FUSE_ROOT_ID;
	if (fuse_ino == FUSE_ROOT_ID)
		return root_cfs_ino;
	else
		return (inode_t) fuse_ino;
}

// Return the request's corresponding mount_t*
static mount_t * reqmount(fuse_req_t req)
{
	assert(req);
	return (mount_t *) fuse_req_userdata(req);
}

// Return the request's corresponding frontend cfs
static CFS_t * reqcfs(fuse_req_t req)
{
	assert(reqmount(req));
	return reqmount(req)->cfs;
}

static bool feature_supported(CFS_t * cfs, inode_t cfs_ino, int feature_id)
{
	const size_t num_features = CALL(cfs, get_num_features, cfs_ino);
	size_t i;

	for (i=0; i < num_features; i++)
		if (CALL(cfs, get_feature, cfs_ino, i)->id == feature_id)
			return 1;

	return 0;
}

static int fill_stat(fuse_req_t req, inode_t cfs_ino, fuse_ino_t fuse_ino, struct stat * stbuf)
{
	Dprintf("%s(fuse_ino = %lu, cfs_ino = %u)\n", __FUNCTION__, fuse_ino, cfs_ino);
	int r;
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;
	bool nlinks_supported = feature_supported(reqcfs(req), cfs_ino, KFS_feature_nlinks.id);
	uint32_t nlinks = 0;

	r = CALL(reqcfs(req), get_metadata, cfs_ino, KFS_feature_filetype.id, &type_size, &type.ptr);
	if (r < 0)
	{
		Dprintf("%d:reqcfs(req)->get_metadata() = %d\n", __LINE__, r);
		return -1;
	}

	if (nlinks_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(reqcfs(req), get_metadata, cfs_ino, KFS_feature_nlinks.id, &data_len, &data);
		if (r >= 0)
		{
			assert(data_len == sizeof(nlinks));
			nlinks = *(uint32_t *) data;
			free(data);
		}
		else
			kdprintf(STDERR_FILENO, "%s: get_metadata for nlinks failed, manually counting links for directories and assuming files have 1 link\n", __FUNCTION__);
	}

	if (*type.type == TYPE_DIR)
	{
		if (!nlinks)
		{
			char buf[1024];
			uint32_t basep = 0;
			fdesc_t * fdesc;

			r = CALL(reqcfs(req), open, cfs_ino, 0, &fdesc);
			assert(r >= 0);
			fdesc->common->parent = (inode_t) hash_map_find_val(reqmount(req)->parents, (void *) cfs_ino);
			assert(fdesc->common->parent != INODE_NONE);

			while ((r = CALL(reqcfs(req), getdirentries, fdesc, buf, sizeof(buf), &basep)) > 0)
			{
				char * cur = buf;
				while (cur < buf + r)
				{
					if (((dirent_t *) cur)->d_type == TYPE_DIR)
						nlinks++;
					cur += ((dirent_t *) cur)->d_reclen;
				}
			}

			r = CALL(reqcfs(req), close, fdesc);
			assert(r >= 0);
		}

		stbuf->st_mode = S_IFDIR | 0755;
	}
	else if (*type.type == TYPE_FILE || *type.type == TYPE_DEVICE)
	{
		uint32_t filesize_size;
		union {
			int32_t * filesize;
			void * ptr;
		} filesize;

		if (!nlinks)
			nlinks = 1;

		r = CALL(reqcfs(req), get_metadata, cfs_ino, KFS_feature_size.id, &filesize_size, &filesize.ptr);
		if (r < 0)
		{
			Dprintf("%d:reqcfs(req)->get_metadata() = %d\n", __LINE__, r);
			goto err;
		}

		stbuf->st_mode = S_IFREG | 0644;
		stbuf->st_size = (off_t) *filesize.filesize;
		free(filesize.filesize);
	}
	else if (*type.type == TYPE_INVAL)
	{
		kdprintf(STDERR_FILENO, "%s:%s(fuse_ino = %lu, cfs_ino = %u): file type is invalid\n", __FILE__, __FUNCTION__, fuse_ino, cfs_ino);
		goto err;
	}
	else
	{
		kdprintf(STDERR_FILENO, "%s:%s(fuse_ino = %lu, cfs_ino = %u): unsupported file type %u\n", __FILE__, __FUNCTION__, fuse_ino, cfs_ino, *type.type);
		goto err;
	}

	stbuf->st_ino = fuse_ino;
	stbuf->st_nlink = nlinks;

	free(type.type);
	return 0;

  err:
	free(type.type);
	return -1;
}

static void serve_getattr(fuse_req_t req, fuse_ino_t fuse_ino, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, fuse_ino);
	struct stat stbuf;
	int r;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (fill_stat(req, fusecfsino(req, fuse_ino), fuse_ino, &stbuf) == -1)
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

static void serve_setattr(fuse_req_t req, fuse_ino_t fuse_ino, struct stat * attr,
                          int to_set, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, to_set = %d)\n", __FUNCTION__, fuse_ino, to_set);
	int r;
	fdesc_t * fdesc;
	inode_t cfs_ino;
	uint32_t size;
	struct stat stbuf;

	if (to_set != FUSE_SET_ATTR_SIZE)
	{
		r = fuse_reply_err(req, ENOSYS);
		assert(!r);
		return;
	}

	size = (uint32_t) attr->st_size;
	assert(size == attr->st_size);
	Dprintf("\tsize = %u\n", size);

	cfs_ino = fusecfsino(req, fuse_ino);

	if (fi)
		fdesc = fi_get_fdesc(fi);
	else
	{
		r = CALL(reqcfs(req), open, cfs_ino, 0, &fdesc);
		if (r < 0)
		{
			r = fuse_reply_err(req, TODOERROR);
			assert(!r);
			return;
		}
		fdesc->common->parent = (inode_t) hash_map_find_val(reqmount(req)->parents, (void *) cfs_ino);
		assert(fdesc->common->parent != INODE_NONE);
	}

	r = CALL(reqcfs(req), truncate, fdesc, size);

	if (!fi)
	{
		if (CALL(reqcfs(req), close, fdesc) < 0)
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

	memset(&stbuf, 0, sizeof(stbuf));
	if (fill_stat(req, cfs_ino, fuse_ino, &stbuf) == -1)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
	}
	else
	{
		r = fuse_reply_attr(req, &stbuf, STDTIMEOUT);
		assert(!r);
	}
}

static void serve_lookup(fuse_req_t req, fuse_ino_t parent, const char *local_name)
{
	Dprintf("%s(parent_ino = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	inode_t parent_cfs_ino;
	int r;
	inode_t cfs_ino;
	struct fuse_entry_param e;

	parent_cfs_ino = fusecfsino(req, parent);
	assert(parent_cfs_ino != INODE_NONE);

	r = CALL(reqcfs(req), lookup, parent_cfs_ino, local_name, &cfs_ino);
	if (r < 0)
	{
		if (r == -E_NOT_FOUND)
			r = fuse_reply_err(req, ENOENT);
		else
			r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = hash_map_insert(reqmount(req)->parents, (void *) cfs_ino, (void *) parent_cfs_ino);
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	memset(&e, 0, sizeof(e));
	e.ino = cfsfuseino(req, cfs_ino);
	e.attr_timeout = STDTIMEOUT;
	e.entry_timeout = STDTIMEOUT;
	r = fill_stat(req, cfs_ino, e.ino, &e.attr);
	if (r < 0)
	{
		// TODO: is it safe to remove cfs_ino from the parents map?
		kdprintf(STDERR_FILENO, "%s(): possible parents entry leak for cfs inode %u\n", cfs_ino);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static void serve_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	Dprintf("%s(ino = %lu, nlookup = %lu)\n", __FUNCTION__, ino, nlookup);
	(void) hash_map_erase(reqmount(req)->parents, (void *) ino);
	fuse_reply_none(req);
}

static void serve_mkdir(fuse_req_t req, fuse_ino_t parent,
                        const char * local_name, mode_t mode)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	inode_t cfs_ino;
	inode_t parent_cfs_ino = fusecfsino(req, parent);
	int r;
	struct fuse_entry_param e;

	r = CALL(reqcfs(req), mkdir, parent_cfs_ino, local_name, &cfs_ino);
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = hash_map_insert(reqmount(req)->parents, (void *) cfs_ino, (void *) parent_cfs_ino);
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	memset(&e, 0, sizeof(e));
	e.ino = cfsfuseino(req, cfs_ino);
	e.attr_timeout = STDTIMEOUT;
	e.entry_timeout = STDTIMEOUT;
	// ignore mode parameter for now
	r = fill_stat(req, cfs_ino, e.ino, &e.attr);
	assert(r >= 0);

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static int create(fuse_req_t req, fuse_ino_t parent, const char * local_name,
                  mode_t mode, struct fuse_entry_param * e, fdesc_t ** fdesc)
{
	inode_t cfs_ino;
	int r;

	r = CALL(reqcfs(req), create, fusecfsino(req, parent), local_name, 0, fdesc, &cfs_ino);
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return -1;
	}
	assert(cfs_ino != INODE_NONE);

	(*fdesc)->common->parent = fusecfsino(req, parent);
	memset(e, 0, sizeof(*e));
	e->ino = cfsfuseino(req, cfs_ino);
	e->attr_timeout = STDTIMEOUT;
	e->entry_timeout = STDTIMEOUT;
	// ignore mode for now
	r = fill_stat(req, cfs_ino, e->ino, &e->attr);
	assert(r >= 0);

	return r;
}

static void serve_create(fuse_req_t req, fuse_ino_t parent,
                         const char * local_name, mode_t mode,
                         struct fuse_file_info * fi)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__,
	        parent, local_name);
	fdesc_t * fdesc;
	int r;
	struct fuse_entry_param e;

	r = create(req, parent, local_name, mode, &e, &fdesc);
	if (r < 0)
		return;

	fi_set_fdesc(fi, fdesc);

	r = fuse_reply_create(req, &e, fi);
	assert(!r);
}

static void serve_mknod(fuse_req_t req, fuse_ino_t parent,
                        const char * local_name, mode_t mode, dev_t rdev)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__, parent, local_name);
	fdesc_t * fdesc;
	int r;
	struct fuse_entry_param e;

	if (!(mode & S_IFREG))
	{
		r = fuse_reply_err(req, ENOSYS);
		assert(!r);
		return;
	}

	r = create(req, parent, local_name, mode, &e, &fdesc);
	if (r < 0)
		return;

	r = CALL(reqcfs(req), close, fdesc);
	assert(r >= 0);

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static void serve_unlink(fuse_req_t req, fuse_ino_t parent, const char * local_name)
{
	Dprintf("%s(parent = %lu, local_name = \"%s\")\n", __FUNCTION__,
	        parent, local_name);
	int r;

	r = CALL(reqcfs(req), unlink, fusecfsino(req, parent), local_name);
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
	int r;

	r = CALL(reqcfs(req), rmdir, fusecfsino(req, parent), local_name);
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
	int r;

	r = CALL(reqcfs(req), rename, fusecfsino(req, old_parent), old_local_name, fusecfsino(req, new_parent), new_local_name);
	if (r < 0)
	{
		// TODO: case -E_FILE_EXISTS: should we allow overwriting?
		// TODO: case -E_INVAL: might mean files are on different filesystems
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static int read_single_dir(CFS_t * cfs, fdesc_t * fdesc, off_t k, dirent_t * dirent)
{
	Dprintf("%s(fdesc = 0x%08x, k = %lld)\n", __FUNCTION__, fdesc, k);
	uint32_t basep = 0;
	char buf[sizeof(dirent_t)];
	char * cur = buf;
	off_t dirno = 0;
	int eof = 0;
	int r;

	assert(fdesc != NULL && k >= 0 && dirent != NULL);
	memset(buf, 0, sizeof(buf));

	while (dirno <= k)
	{
		uint32_t nbytes;
		cur = buf;

		r = CALL(cfs, getdirentries, fdesc, buf, sizeof(buf), &basep);
		assert(dirent); // catch some stack overwrites in getdirentries()
		if (r == -E_UNSPECIFIED) // should imply eof
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

static void ssync(fuse_req_t req, fuse_ino_t fuse_ino, int datasync,
                  struct fuse_file_info * fi)
{
	int r;

	// ignore datasync
	r = kfs_sync();
	if (r == -E_BUSY)
	{
		r = fuse_reply_err(req, TODOERROR);
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

static void serve_fsync(fuse_req_t req, fuse_ino_t fuse_ino, int datasync,
                        struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, datasync = %d)\n", __FUNCTION__, fuse_ino, datasync);
	ssync(req, fuse_ino, datasync, fi);
}

static void serve_fsyncdir(fuse_req_t req, fuse_ino_t fuse_ino, int datasync,
                           struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, datasync = %d)\n", __FUNCTION__, fuse_ino, datasync);
	ssync(req, fuse_ino, datasync, fi);	
}

static void serve_opendir(fuse_req_t req, fuse_ino_t fuse_ino,
                          struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, fuse_ino);
	fdesc_t * fdesc;
	inode_t cfs_ino;
	inode_t parent_cfs_ino;
	int r;

	cfs_ino = fusecfsino(req, fuse_ino);

	r = CALL(reqcfs(req), open, cfs_ino, 0, &fdesc);
	if (r < 0)
	{
		// TODO: fid could be E_NOT_FOUND, E_NOT_FOUND, or other
		// TODO: fuse_reply_err(req, ENOTDIR);
		r = fuse_reply_err(req, -1);
		assert(!r);
		return;
	}

	parent_cfs_ino = (inode_t) hash_map_find_val(reqmount(req)->parents, (void *) cfs_ino);
	if (parent_cfs_ino == INODE_NONE)
	{
		kdprintf(STDERR_FILENO, "%s(): no parent ino for ino %u\n", __FUNCTION__, cfs_ino);
		(void) CALL(reqcfs(req), close, fdesc);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}
	fdesc->common->parent = parent_cfs_ino;

	fi_set_fdesc(fi, fdesc);

	r = fuse_reply_open(req, fi);
	assert(!r);
}

static void serve_releasedir(fuse_req_t req, fuse_ino_t fuse_ino,
                             struct fuse_file_info * fi)
{
	fdesc_t * fdesc = fi_get_fdesc(fi);
	int r;
	Dprintf("%s(ino = %lu, fdesc = 0x%08x)\n", __FUNCTION__, fuse_ino, fdesc);

	r = CALL(reqcfs(req), close, fdesc);
	if (r < 0)
	{
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_readdir(fuse_req_t req, fuse_ino_t fuse_ino, size_t size,
                          off_t off, struct fuse_file_info * fi)
{
	fdesc_t * fdesc = fi_get_fdesc(fi);
	uint32_t total_size = 0;
	char * buf = NULL;
	struct stat stbuf;
	int r;
	Dprintf("%s(ino = %lu, size = %u, off = %lld)\n", __FUNCTION__, fuse_ino, size, off);

	while (1)
	{
		dirent_t dirent;
		size_t oldsize = total_size;
		inode_t entry_cfs_ino;

		r = read_single_dir(reqcfs(req), fdesc, off, &dirent);
		if (r == 1 || r == -E_NOT_FOUND)
			break;
		if (r == -E_EOF)
			break;
		else if (r < 0)
		{
			kdprintf(STDERR_FILENO, "%s:%s(): read_single_dir(0x%08x, %lld, 0x%08x) = %d\n",
					 __FILE__, __FUNCTION__, fdesc, off, &dirent, r);
			assert(r >= 0);
		}

		total_size += fuse_dirent_size(dirent.d_namelen);

		if (total_size > size)
		{
			total_size = oldsize;
			break;
		}

		buf = (char *) realloc(buf, total_size);
		assert(buf);

		memset(&stbuf, 0, sizeof(stbuf));
		// Generate "." and ".." here rather than in the base file system
		// because they are not able to find ".."'s inode from just
		// "."'s inode
		if (!strcmp(dirent.d_name, "."))
			entry_cfs_ino = fusecfsino(req, fuse_ino);
		else if (!strcmp(dirent.d_name, ".."))
			entry_cfs_ino = fdesc->common->parent;
		else
		{
			r = CALL(reqcfs(req), lookup, fusecfsino(req, fuse_ino), dirent.d_name, &entry_cfs_ino);
			assert(r >= 0);
		}
		stbuf.st_ino = cfsfuseino(req, entry_cfs_ino);
		fuse_add_dirent(buf + oldsize, dirent.d_name, &stbuf, ++off);
	}

	r = fuse_reply_buf(req, buf, total_size);
	assert(!r);
	free(buf);
}

static void serve_open(fuse_req_t req, fuse_ino_t fuse_ino,
                       struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, fuse_ino);
	uint32_t size;
	inode_t cfs_ino;
	void * data;
	uint32_t type;
	fdesc_t * fdesc;
	int r;

//	else if ((fi->flags & 3) != O_RDONLY)
//		fuse_reply_err(req, EACCES);

	cfs_ino = fusecfsino(req, fuse_ino);

	r = CALL(reqcfs(req), get_metadata, cfs_ino, KFS_feature_filetype.id, &size, &data);
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

	r = CALL(reqcfs(req), open, cfs_ino, 0, &fdesc);
	assert(r >= 0);
	fi_set_fdesc(fi, fdesc);
	
	r = fuse_reply_open(req, fi);
	assert(!r);
}

static void serve_release(fuse_req_t req, fuse_ino_t fuse_ino, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, fuse_ino);
	fdesc_t * fdesc = fi_get_fdesc(fi);
	int r;

	r = CALL(reqcfs(req), close, fdesc);
	assert(r >= 0);
	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_read(fuse_req_t req, fuse_ino_t fuse_ino, size_t size,
                       off_t off, struct fuse_file_info * fi)
{
	fdesc_t * fdesc = fi_get_fdesc(fi);
	uint32_t offset = off;
	char * buf;
	int r;
	Dprintf("%s(ino = %lu, fdesc = 0x%08x, size = %u, off = %lld)\n", __FUNCTION__, fuse_ino, fdesc, size, off);

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	buf = malloc(size);
	assert(buf);

	r = CALL(reqcfs(req), read, fdesc, buf, off, size);
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

static void serve_write(fuse_req_t req, fuse_ino_t fuse_ino, const char * buf,
                        size_t size, off_t off, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu, size = %u, off = %lld, buf = \"%s\")\n",
	        __FUNCTION__, fuse_ino, size, off, buf);
	uint32_t offset = off;
	fdesc_t * fdesc;
	int nbytes;
	int r;

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, TODOERROR);
		assert(!r);
		return;
	}

	fdesc = fi_get_fdesc(fi);

	static_assert(sizeof(uint32_t) == sizeof(size));
	nbytes = CALL(reqcfs(req), write, fdesc, buf, offset, size);
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


static void signal_handler(int sig)
{
	char buf = 1;
	if (shutdown_pipe[1] == -1)
		return;
	if (write(shutdown_pipe[1], &buf, sizeof(buf)) != sizeof(buf))
	{
		kdprintf(STDERR_FILENO, "%s(%d): write() failed\n", __FUNCTION__, sig);
		perror("write");
	}
	printf("Shutdown started.\n");
	fflush(stdout);
}

static int set_signal_handler(int sig, void (*handler)(int))
{
	struct sigaction sa;
	struct sigaction prev_sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(sig, NULL, &prev_sa) == -1)
	{
		perror("sigaction");
		return -1;
	}
	if (prev_sa.sa_handler == SIG_DFL && sigaction(sig, &sa, NULL) == -1)
	{
		perror("Cannot set signal handler");
		return -1;
	}
	return 0;
}

static int set_signal_handlers(void)
{
	if ((set_signal_handler(SIGHUP, signal_handler) == -1)
	     || (set_signal_handler(SIGINT, signal_handler) == -1)
	     || (set_signal_handler(SIGTERM, signal_handler) == -1)
	     || (set_signal_handler(SIGPIPE, SIG_IGN) == -1))
		return -1;
	return 0;
}

static void ignore_shutdown_signals(void)
{
	int shutdown_pipe_write;

	if (shutdown_pipe[1] == -1)
		return; // shutdown signals are already ignored

	// Close the shutdown pipe; remove access from the signal handler before closing
	shutdown_pipe_write = shutdown_pipe[1];
	shutdown_pipe[1] = -1;
	if (close(shutdown_pipe_write) == -1)
		perror("fuse_serve_shutdown(): close(shutdown_pipe_write)");
	if (close(shutdown_pipe[0]) == -1)
		perror("fuse_serve_shutdown(): close(shutdown_pipe[0])");
	shutdown_pipe[0] = -1;
}


static void fuse_serve_shutdown(void * arg)
{
	ignore_shutdown_signals();

	root_cfs = NULL;
	serving = 0;

	free(channel_buf);
	channel_buf = NULL;
	channel_buf_len = 0;

	fuse_serve_mount_instant_shutdown();
	if (remove_activity != -1 && close(remove_activity) < 0)
		perror("fuse_serve_shutdown: close");
	remove_activity = -1;
}


int fuse_serve_init(int argc, char ** argv)
{
	int r;

	root_cfs = NULL;
	serving = 0;

	if ((r = kfsd_register_shutdown_module(fuse_serve_shutdown, NULL)) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): kfsd_register_shutdown_module() = %d\n", __FUNCTION__, r);
		return r;
	}

	if ((r = pipe(shutdown_pipe)) < 0)
	{
		perror("fuse_serve_init(): pipe:");
		return -E_UNSPECIFIED;
	}

	if ((r = fuse_serve_mount_init(argc, argv, &serve_oper, sizeof(serve_oper))) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_serve_mount_init() = %d\n", __FUNCTION__, r);
		goto error_pipe;
	}
	remove_activity = r;

	channel_buf_len = fuse_serve_mount_chan_bufsize();
    if (!(channel_buf = (char *) malloc(channel_buf_len)))
	{
        kdprintf(STDERR_FILENO, "%s(): malloc(%u) failed to allocate read buffer\n", __FUNCTION__, channel_buf_len);
		goto error_buf_len;
    }

	if ((r = set_signal_handlers()) < 0)
		goto error_buf_malloc;

	return 0;

  error_buf_malloc:
	free(channel_buf);
	channel_buf = NULL;
  error_buf_len:
	channel_buf_len = 0;
  error_pipe:
	close(shutdown_pipe[1]);
	close(shutdown_pipe[0]);
	return r;
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


// Adapted from FUSE's lib/fuse_loop.c to support sched callbacks and multiple mounts
int fuse_serve_loop(void)
{
	struct timeval tv;
	int r;
	Dprintf("%s()\n", __FUNCTION__);

	if (!root_cfs)
	{
		kdprintf(STDERR_FILENO, "%s(): no root cfs was specified; not running.\n", __FUNCTION__);
		return -E_UNSPECIFIED;
	}

	if ((r = fuse_serve_mount_load_mounts()) < 0)
	{
		kdprintf(STDERR_FILENO, "%s(): fuse_serve_load_mounts: %d\n", __FUNCTION__, r);
		return r;
	}

	serving = 1;
	tv = fuse_serve_timeout();

    while (fuse_serve_mounts() && hash_set_size(fuse_serve_mounts()))
	{
		fd_set rfds;
		int max_fd = 0;
		hash_set_it_t mounts_it;
		mount_t * mount;
		struct timeval it_start, it_end;

		FD_ZERO(&rfds);

		if (shutdown_pipe[0] != -1)
		{
			FD_SET(shutdown_pipe[0], &rfds);
			if (shutdown_pipe[0] > max_fd)
				max_fd = shutdown_pipe[0];
		}

		FD_SET(remove_activity, &rfds);
		if (remove_activity > max_fd)
			max_fd = remove_activity;

		hash_set_it_init(&mounts_it, fuse_serve_mounts());
		while ((mount = hash_set_next(&mounts_it)))
		{
			if (mount->mounted && !fuse_session_exited(mount->session))
			{
				//printf("[\"%s\"]", mount->kfs_path); fflush(stdout); // debug
				int mount_fd = fuse_chan_fd(mount->channel);
				FD_SET(mount_fd, &rfds);
				if (mount_fd > max_fd)
					max_fd = mount_fd;
			}
		}

		r = select(max_fd+1, &rfds, NULL, NULL, &tv);

		if (r == 0)
		{
			//printf("."); fflush(stdout); // debugging output
			sched_iteration();
			tv = fuse_serve_timeout();
		}
		else if (r < 0)
		{
			if (errno != EINTR)
				perror("select");
			//printf("!\n"); fflush(stdout); // debugging output
			tv = fuse_serve_timeout(); // tv may have become undefined
		}
		else
		{
			if (gettimeofday(&it_start, NULL) == -1)
			{
				perror("gettimeofday");
				break;
			}

			hash_set_it_init(&mounts_it, fuse_serve_mounts());
			while ((mount = hash_set_next(&mounts_it)))
			{
				if (mount->mounted && FD_ISSET(mount->channel_fd, &rfds))
				{
					r = fuse_chan_receive(mount->channel, channel_buf, channel_buf_len);
					assert(r > 0); // what would this error mean?

					Dprintf("fuse_serve: request for mount \"%s\"\n", mount->kfs_path);
					fuse_session_process(mount->session, channel_buf, r, mount->channel);
				}
			}

			if (shutdown_pipe[0] != -1 && FD_ISSET(shutdown_pipe[0], &rfds))
			{
				// Start unmounting all filesystems
				// Looping will stop once all filesystems are unmounted
				ignore_shutdown_signals();
				if (fuse_serve_mount_start_shutdown() < 0)
				{
					kdprintf(STDERR_FILENO, "fuse_serve_mount_start_shutdown() failed, exiting fuse_serve_loop()\n");
					return -E_UNSPECIFIED;
				}
			}

			if (FD_ISSET(remove_activity, &rfds))
			{
				if (fuse_serve_mount_step_remove() < 0)
				{
					kdprintf(STDERR_FILENO, "fuse_serve_mount_step_remove() failed, exiting fuse_serve_loop()\n");
					return -E_UNSPECIFIED;
				}
			}


			if (gettimeofday(&it_end, NULL) == -1)
			{
				perror("gettimeofday");
				break;
			}
			tv = time_subtract(tv, time_elapsed(it_start, it_end));
		}
    }

	serving = 0;

    return 0;
}
