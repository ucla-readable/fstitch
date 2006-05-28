#define __USE_BSD // for timersub()
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
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
// - Enable debug output for fuse_serve
// - Run with the -d flag to see FUSE messages coming in and going out

// TODOs:
// - Why does using a 0s timeout (instead of 1.0) not work? Is this a problem?
// - Propagate errors rather than assert() in places where assert() is used for errors that can happen
// - Send negative lookup answers (rather than ENOENT), right?
// - Add support for the other fuse_lowlevel_ops that make sense
// - Switch off kernel buffer cache for ourself? (direct_io)
// - Be safer; eg call open() only when we should
// - Speedup serve_readdir() when helpful (it runs O(n^2); a slightly more complex O(n) would work)
// - Support multiple hard links (how do we deal with open() and opendir()?)
// - Support more metadata; eg atime and mtime
// - Support delayed event response or multiple threads

#define FUSE_SERVE_DEBUG 0

#if FUSE_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

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

// Return the request's inode_t corresponding to the fuse_ino_t
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

static int fill_stat(mount_t * mount, inode_t cfs_ino, fuse_ino_t fuse_ino, struct stat * stbuf)
{
	Dprintf("%s(fuse_ino = %lu, cfs_ino = %u)\n", __FUNCTION__, fuse_ino, cfs_ino);
	int r;
	CFS_t * cfs = mount->cfs;
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;
	bool nlinks_supported = feature_supported(cfs, cfs_ino, KFS_feature_nlinks.id);
	bool uid_supported = feature_supported(cfs, cfs_ino, KFS_feature_uid.id);
	bool gid_supported = feature_supported(cfs, cfs_ino, KFS_feature_gid.id);
	bool perms_supported = feature_supported(cfs, cfs_ino, KFS_feature_unix_permissions.id);
	bool mtime_supported = feature_supported(cfs, cfs_ino, KFS_feature_mtime.id);
	bool atime_supported = feature_supported(cfs, cfs_ino, KFS_feature_atime.id);
	uint32_t nlinks = 0;
	mode_t perms;
	time_t mtime = time(NULL);
	time_t atime = mtime;

	r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_filetype.id, &type_size, &type.ptr);
	if (r < 0)
	{
		Dprintf("%d:cfs->get_metadata() = %d\n", __LINE__, r);
		return r;
	}

	if (nlinks_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_nlinks.id, &data_len, &data);
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
			dirent_t dirent;
			uint32_t basep = 0;
			fdesc_t * fdesc;

			r = CALL(cfs, open, cfs_ino, 0, &fdesc);
			assert(r >= 0);
			fdesc->common->parent = (inode_t) hash_map_find_val(mount->parents, (void *) cfs_ino);
			assert(fdesc->common->parent != INODE_NONE);

			while ((r = CALL(cfs, get_dirent, fdesc, &dirent, sizeof(dirent), &basep)) >= 0)
				if (dirent.d_type == TYPE_DIR)
					nlinks++;

			r = CALL(cfs, close, fdesc);
			assert(r >= 0);
		}

		stbuf->st_mode = S_IFDIR;
		perms = 0777; // default, in case permissions are not supported
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

		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_size.id, &filesize_size, &filesize.ptr);
		if (r < 0)
		{
			Dprintf("%d:cfs->get_metadata() = %d\n", __LINE__, r);
			goto err;
		}

		stbuf->st_mode = S_IFREG;
		perms = 0666; // default, in case permissions are not supported
		stbuf->st_size = (off_t) *filesize.filesize;
		free(filesize.filesize);
	}
	else if (*type.type == TYPE_INVAL)
	{
		kdprintf(STDERR_FILENO, "%s:%s(fuse_ino = %lu, cfs_ino = %u): file type is invalid\n", __FILE__, __FUNCTION__, fuse_ino, cfs_ino);
		r = -E_UNSPECIFIED;
		goto err;
	}
	else
	{
		kdprintf(STDERR_FILENO, "%s:%s(fuse_ino = %lu, cfs_ino = %u): unsupported file type %u\n", __FILE__, __FUNCTION__, fuse_ino, cfs_ino, *type.type);
		r = -E_UNSPECIFIED;
		goto err;
	}

	if (uid_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_uid.id, &data_len, &data);
		if (r >= 0)
		{
			uint32_t cfs_uid;
			assert(data_len == sizeof(cfs_uid));
			stbuf->st_uid = cfs_uid = *(uint32_t *) data;
			free(data);
			if (stbuf->st_uid != cfs_uid)
				kdprintf(STDERR_FILENO, "%s: UID not large enough to hold CFS UID %u\n", __FUNCTION__, cfs_uid);
		}
		else
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed uid but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
	}
	else
		stbuf->st_uid = 0;

	if (gid_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_gid.id, &data_len, &data);
		if (r >= 0)
		{
			uint32_t cfs_gid;
			assert(data_len == sizeof(cfs_gid));
			stbuf->st_gid = cfs_gid = *(uint32_t *) data;
			free(data);
			if (stbuf->st_gid != cfs_gid)
				kdprintf(STDERR_FILENO, "%s: GID not large enough to hold CFS GID %u\n", __FUNCTION__, cfs_gid);
		}
		else
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed gid but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
	}
	else
		stbuf->st_gid = 0;

	if (perms_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_unix_permissions.id, &data_len, &data);
		if (r >= 0)
		{
			assert(data_len == sizeof(mode_t));
			perms = *(mode_t *) data;
			free(data);
		}
		else
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed unix permissions but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
	}

	if (mtime_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_mtime.id, &data_len, &data);
		if (r >= 0)
		{
			assert(data_len == sizeof(time_t));
			mtime = *(time_t *) data;
			free(data);
		}
		else
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed mtime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
	}

	if (atime_supported)
	{
		size_t data_len;
		void * data;
		r = CALL(cfs, get_metadata, cfs_ino, KFS_feature_atime.id, &data_len, &data);
		if (r >= 0)
		{
			assert(data_len == sizeof(time_t));
			atime = *(time_t *) data;
			free(data);
		}
		else
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed atime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
	}

	stbuf->st_mode |= perms;
	stbuf->st_mtime = mtime;
	stbuf->st_atime = atime;
	stbuf->st_ino = fuse_ino;
	stbuf->st_nlink = nlinks;

	free(type.type);
	return 0;

  err:
	free(type.type);
	return r;
}

static int init_fuse_entry(mount_t * mount, inode_t parent, inode_t cfs_ino, fuse_ino_t fuse_ino, struct fuse_entry_param * e)
{
	int r;

	r = hash_map_insert(mount->parents, (void *) cfs_ino, (void *) parent);
	if (r < 0)
		return r;

	memset(e, 0, sizeof(*e));
	e->ino = fuse_ino;
	e->attr_timeout = STDTIMEOUT;
	e->entry_timeout = STDTIMEOUT;
	r = fill_stat(mount, cfs_ino, e->ino, &e->attr);
	assert(r >= 0);

	return 0;
}

static void serve_statfs(fuse_req_t req)
{
	Dprintf("%s()\n", __FUNCTION__);
	struct statvfs st; // For more info, see: man 2 statvfs
	int r;
	size_t size;
	void * data;

	r = CALL(reqcfs(req), get_metadata, 0, KFS_feature_blocksize.id, &size, &data);
	if (r < 0)
		goto serve_statfs_err;
	else if (sizeof(st.f_bsize) != size)
	{
		r = -E_UNSPECIFIED;
		goto serve_statfs_err;
	}
	st.f_bsize = st.f_frsize = *(uint32_t *) data;
	free(data);

	r = CALL(reqcfs(req), get_metadata, 0, KFS_feature_devicesize.id, &size, &data);
	if (r < 0 || sizeof(st.f_blocks) < size)
		st.f_blocks = st.f_bfree = st.f_bavail = 0;
	else
	{
		st.f_blocks = *(uint32_t *) data;
		free(data);
		r = CALL(reqcfs(req), get_metadata, 0, KFS_feature_freespace.id, &size, &data);
		if (r < 0 || sizeof(st.f_bfree) < size)
			st.f_bfree = st.f_bavail = 0;
		else
		{
			st.f_bfree = st.f_bavail = *(uint32_t *) data;
			free(data);
		}
	}

	// TODO - add lfs features for these guys
	st.f_files = 0;
	st.f_ffree = st.f_favail = 0;
	st.f_flag = 0;
	st.f_namemax = 256;

	r = fuse_reply_statfs(req, &st);
	assert(!r);
	return;

serve_statfs_err:
	r = fuse_reply_err(req, -r);
	assert(!r);
	return;
}

static void serve_getattr(fuse_req_t req, fuse_ino_t fuse_ino, struct fuse_file_info * fi)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, fuse_ino);
	struct stat stbuf;
	int r;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	r = fill_stat(reqmount(req), fusecfsino(req, fuse_ino), fuse_ino, &stbuf);
	if (r < 0)
		r = fuse_reply_err(req, -r);
	else
		r = fuse_reply_attr(req, &stbuf, STDTIMEOUT);
	assert(!r);
}

static void serve_setattr(fuse_req_t req, fuse_ino_t fuse_ino, struct stat * attr,
                          int to_set, struct fuse_file_info * fi)
{
	inode_t cfs_ino = fusecfsino(req, fuse_ino);
	int supported = FUSE_SET_ATTR_SIZE;
	bool uid_supported   = feature_supported(reqcfs(req), cfs_ino, KFS_feature_uid.id);
	bool gid_supported   = feature_supported(reqcfs(req), cfs_ino, KFS_feature_gid.id);
	bool perms_supported = feature_supported(reqcfs(req), cfs_ino, KFS_feature_unix_permissions.id);
	bool mtime_supported = feature_supported(reqcfs(req), cfs_ino, KFS_feature_mtime.id);
	bool atime_supported = feature_supported(reqcfs(req), cfs_ino, KFS_feature_mtime.id);
	struct stat stbuf;
	int r;
	Dprintf("%s(ino = %lu, to_set = %d)\n", __FUNCTION__, fuse_ino, to_set);

	if (uid_supported)
		supported |= FUSE_SET_ATTR_UID;
	if (gid_supported)
		supported |= FUSE_SET_ATTR_GID;
	if (perms_supported)
		supported |= FUSE_SET_ATTR_MODE;
	if (mtime_supported)
		supported |= FUSE_SET_ATTR_MTIME;
	if (atime_supported)
		supported |= FUSE_SET_ATTR_ATIME;

	if (to_set != (to_set & supported))
	{
		r = fuse_reply_err(req, E_NO_SYS);
		assert(!r);
		return;
	}

	if (to_set & FUSE_SET_ATTR_SIZE)
	{
		fdesc_t * fdesc;
		uint32_t size;

		size = (uint32_t) attr->st_size;
		assert(size == attr->st_size);
		Dprintf("\tsize = %u\n", size);

		if (fi)
			fdesc = fi_get_fdesc(fi);
		else
		{
			r = CALL(reqcfs(req), open, cfs_ino, 0, &fdesc);
			if (r < 0)
			{
				r = fuse_reply_err(req, -r);
				assert(!r);
				return;
			}
			fdesc->common->parent = (inode_t) hash_map_find_val(reqmount(req)->parents, (void *) cfs_ino);
			assert(fdesc->common->parent != INODE_NONE);
		}

		r = CALL(reqcfs(req), truncate, fdesc, size);

		if (!fi)
		{
			r = CALL(reqcfs(req), close, fdesc);
			if (r < 0)
			{
				r = fuse_reply_err(req, -r);
				assert(!r);
				return;
			}
		}

		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	if (to_set & FUSE_SET_ATTR_MODE)
	{
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_unix_permissions.id, sizeof(attr->st_mode), &attr->st_mode);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	if (to_set & FUSE_SET_ATTR_UID)
	{
		uint32_t cfs_uid = attr->st_uid;
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_uid.id, sizeof(cfs_uid), &cfs_uid);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	if (to_set & FUSE_SET_ATTR_GID)
	{
		uint32_t cfs_gid = attr->st_gid;
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_gid.id, sizeof(cfs_gid), &cfs_gid);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	if (to_set & FUSE_SET_ATTR_MODE)
	{
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_unix_permissions.id, sizeof(attr->st_mode), &attr->st_mode);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	if (to_set & FUSE_SET_ATTR_MTIME)
	{
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_mtime.id, sizeof(attr->st_mtime), &attr->st_mtime);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}
	
	if (to_set & FUSE_SET_ATTR_ATIME)
	{
		r = CALL(reqcfs(req), set_metadata, cfs_ino, KFS_feature_atime.id, sizeof(attr->st_mtime), &attr->st_mtime);
		if (r < 0)
		{
			r = fuse_reply_err(req, -r);
			assert(!r);
			return;
		}
	}

	memset(&stbuf, 0, sizeof(stbuf));
	r = fill_stat(reqmount(req), cfs_ino, fuse_ino, &stbuf);
	if (r < 0)
		r = fuse_reply_err(req, -r);
	else
		r = fuse_reply_attr(req, &stbuf, STDTIMEOUT);
	assert(!r);
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
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = init_fuse_entry(reqmount(req), parent_cfs_ino, cfs_ino, cfsfuseino(req, cfs_ino), &e);
	if (r < 0)
	{
		// TODO: is it safe to remove cfs_ino from the parents map if fill_stat() failed?
		kdprintf(STDERR_FILENO, "%s(): possible parents entry leak for cfs inode %u\n", cfs_ino);

		r = fuse_reply_err(req, -r);
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
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	// FIXME: set uid, gid, and mode

	r = init_fuse_entry(reqmount(req), parent_cfs_ino, cfs_ino, cfsfuseino(req, cfs_ino), &e);
	if (r < 0)
	{
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static int create(fuse_req_t req, fuse_ino_t parent, const char * local_name,
                  mode_t mode, struct fuse_entry_param * e, fdesc_t ** fdesc)
{
	inode_t cfs_parent = fusecfsino(req, parent);
	inode_t cfs_ino;
	int r;

	r = CALL(reqcfs(req), create, cfs_parent, local_name, 0, fdesc, &cfs_ino);
	if (r < 0)
		return r;
	assert(cfs_ino != INODE_NONE);

	// FIXME: set uid, gid, and mode

	r = init_fuse_entry(reqmount(req), cfs_parent, cfs_ino, cfsfuseino(req, cfs_ino), e);
	if (r < 0)
	{
		(void) CALL(reqmount(req)->cfs, close, *fdesc);
		*fdesc = NULL;
		(void) CALL(reqmount(req)->cfs, unlink, parent, local_name);
		return r;
	}
	(*fdesc)->common->parent = cfs_parent;

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
	{
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

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
		r = fuse_reply_err(req, E_NO_SYS);
		assert(!r);
		return;
	}

	r = create(req, parent, local_name, mode, &e, &fdesc);
	if (r < 0)
	{
		r = fuse_reply_err(req, -r);
		assert(r);
		return;
	}

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
		r = fuse_reply_err(req, -r);
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
		r = fuse_reply_err(req, -r);
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
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

static void serve_link(fuse_req_t req, fuse_ino_t fuse_ino,
                       fuse_ino_t new_parent, const char * new_local_name)
{
	Dprintf("%s(ino = %lu, newp = %lu, newln = \"%s\")\n",
	        __FUNCTION__, fuse_ino, new_parent, new_local_name);
	inode_t cfs_ino = fusecfsino(req, fuse_ino);
	inode_t new_cfs_parent = fusecfsino(req, new_parent);
	struct fuse_entry_param e;
	int r;

	r = CALL(reqcfs(req), link, cfs_ino, new_cfs_parent, new_local_name);
	if (r < 0)
	{
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = init_fuse_entry(reqmount(req), new_cfs_parent, cfs_ino, fuse_ino, &e);
	if (r < 0)
	{
		(void) CALL(reqmount(req)->cfs, unlink, new_cfs_parent, new_local_name);
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = fuse_reply_entry(req, &e);
	assert(!r);
}

static void ssync(fuse_req_t req, fuse_ino_t fuse_ino, int datasync,
                  struct fuse_file_info * fi)
{
	int r;

	// ignore datasync
	r = kfs_sync();
	if (r < 0)
	{
		r = fuse_reply_err(req, -r);
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
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	parent_cfs_ino = (inode_t) hash_map_find_val(reqmount(req)->parents, (void *) cfs_ino);
	if (parent_cfs_ino == INODE_NONE)
	{
		kdprintf(STDERR_FILENO, "%s(): no parent ino for ino %u\n", __FUNCTION__, cfs_ino);
		(void) CALL(reqcfs(req), close, fdesc);
		r = fuse_reply_err(req, E_UNSPECIFIED);
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
	Dprintf("%s(ino = %lu, fdesc = %p)\n", __FUNCTION__, fuse_ino, fdesc);

	r = CALL(reqcfs(req), close, fdesc);
	if (r < 0)
	{
		r = fuse_reply_err(req, -r);
		assert(!r);
		return;
	}

	r = fuse_reply_err(req, FUSE_ERR_SUCCESS);
	assert(!r);
}

#define RECLEN_MIN_SIZE (sizeof(((dirent_t *) NULL)->d_reclen) + (int) &((dirent_t *) NULL)->d_reclen)

static void serve_readdir(fuse_req_t req, fuse_ino_t fuse_ino, size_t size,
                          off_t foff, struct fuse_file_info * fi)
{
	fdesc_t * fdesc = fi_get_fdesc(fi);
	uint32_t off = foff;
	uint32_t total_size = 0;
	char * buf = NULL;
	int r;
	Dprintf("%s(ino = %lu, size = %u, off = %lld)\n", __FUNCTION__, fuse_ino, size, foff);

	while (1)
	{
		dirent_t dirent;
		int nbytes;
		struct stat stbuf;
		inode_t entry_cfs_ino;
		size_t oldsize = total_size;

		nbytes = CALL(reqcfs(req), get_dirent, fdesc, &dirent, sizeof(dirent), &off);
		if (nbytes == -E_UNSPECIFIED)
			break;
		else if (nbytes < 0)
		{
			kdprintf(STDERR_FILENO, "%s:%s(): CALL(cfs, get_dirent, fdesc = %p, off = %lld) = %d\n",
					 __FILE__, __FUNCTION__, fdesc, off, nbytes);
			assert(nbytes >= 0);
		}

		if (total_size + fuse_dirent_size(dirent.d_namelen) > size)
			break;
		Dprintf("%s: \"%s\"\n", __FUNCTION__, dirent.d_name);

		total_size += fuse_dirent_size(dirent.d_namelen);
		buf = (char *) realloc(buf, total_size);
		if (!buf)
			panic("realloc() failed");

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
		fuse_add_dirent(buf + oldsize, dirent.d_name, &stbuf, off);
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
	Dprintf("%s(ino = %lu, fdesc = %p, size = %u, off = %lld)\n", __FUNCTION__, fuse_ino, fdesc, size, off);

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, E_INVAL);
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
	Dprintf("%s(ino = %lu, size = %u, off = %lld)\n",
	        __FUNCTION__, fuse_ino, size, off);
	uint32_t offset = off;
	fdesc_t * fdesc;
	int nbytes;
	int r;

	if (offset != off)
	{
		kdprintf(STDERR_FILENO, "%s:%d: KFSD offset not able to satisfy request for %lld\n", __FILE__, __LINE__, off);
		r = fuse_reply_err(req, E_INVAL);
		assert(!r);
		return;
	}

	fdesc = fi_get_fdesc(fi);

	static_assert(sizeof(uint32_t) == sizeof(size));
	nbytes = CALL(reqcfs(req), write, fdesc, buf, offset, size);
	if (nbytes < size)
	{
		r = fuse_reply_write(req, nbytes);
		assert(!r);
		return;
	}
	
	r = fuse_reply_write(req, nbytes);
	assert(!r);
}


static struct fuse_lowlevel_ops serve_oper =
{
	.statfs     = serve_statfs,
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
	.link       = serve_link,
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

	if ((r = kfsd_register_shutdown_module(fuse_serve_shutdown, NULL, SHUTDOWN_PREMODULES)) < 0)
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
static struct timeval time_elapsed(struct timeval start, struct timeval end)
{
	struct timeval diff;
	assert(start.tv_sec < end.tv_sec
	       || (start.tv_sec == end.tv_sec && start.tv_usec <= end.tv_usec));
	timersub(&end, &start, &diff);
	return diff;
}

// Return MAX(remaining - elapsed, 0)
static struct timeval time_subtract(struct timeval remaining, struct timeval elapsed)
{
	struct timeval n;
	if (remaining.tv_sec < elapsed.tv_sec
	    || (remaining.tv_sec == elapsed.tv_sec
	        && remaining.tv_usec <= elapsed.tv_usec))
		n.tv_sec = n.tv_usec = 0;
	else
		timersub(&remaining, &elapsed, &n);
	return n;
}

// Return the amount of time to wait between sched_run_callbacks() calls
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
			sched_run_callbacks();
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
					/* starting a new request, so set a new request ID */
					kfsd_next_request_id();
					r = fuse_chan_receive(mount->channel, channel_buf, channel_buf_len);
					assert(r > 0); // what would this error mean?

					Dprintf("fuse_serve: request for mount \"%s\"\n", mount->kfs_path);
					fuse_session_process(mount->session, channel_buf, r, mount->channel);
					sched_run_cleanup();
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
