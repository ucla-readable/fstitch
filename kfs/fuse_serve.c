#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <lib/hash_map.h>
#include <lib/dirent.h>
#include <lib/panic.h>
#include <lib/kdprintf.h>
#include <kfs/feature.h>
#include <kfs/cfs.h>
#include <kfs/modman.h>

#define FUSE_SERVE_DEBUG 0


#if FUSE_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

// If we move to multiple fuse_serve instances we can put global fields into
// FUSE's userdata param
static CFS_t * frontend_cfs = NULL;
static hash_map_t * names; // fuse_ino_t ino -> char * filename
static hash_map_t * parents; // fuse_ino_t child -> fuse_ino_t parent

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
	uint32_t * type;

	name = hash_map_find_val(names, (void *) ino);
	if (!name)
		return -1;

	fid = (int) ino;
	if (fid == FUSE_ROOT_ID)
	{
		name = "/";
		fid = (int) name;
	}

	r = CALL(frontend_cfs, get_metadata, name, KFS_feature_filetype.id, &type_size, (void **) &type);
	if (r < 0)
	{
		Dprintf("%d:frontend_cfs->get_metadata() = %d\n", __LINE__, r);
		return -1;
	}

	if (*type == TYPE_DIR)
	{
		char buf[1024];
		uint32_t basep;
		uint32_t nlinks = 0;

		while ((r = CALL(frontend_cfs, getdirentries, fid, buf, sizeof(buf), &basep)) > 0)
		{
			char * cur = buf;
				while (cur < buf + r)
				{
					nlinks++;
					cur += ((dirent_t *) cur)->d_reclen;
				}
		}

		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = nlinks;
	}
	else if (*type == TYPE_FILE)
	{
		uint32_t filesize_size;
		int32_t  * filesize;
		r = CALL(frontend_cfs, get_metadata, name, KFS_feature_size.id, &filesize_size, (void **) &filesize);
		if (r < 0)
		{
			Dprintf("%d:frontend_cfs->get_metadata() = %d\n", __LINE__, r);
			goto err;
		}

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1; //	TODO: KFS_feature_nlinks
		stbuf->st_size = (off_t) *filesize;
		free(filesize);
	}
	else
	{
		Dprintf("%d:file type %u unknown\n", __LINE__, *type);
		goto err;
	}
	stbuf->st_ino = ino;

	free(type);
	return 0;

  err:
	free(type);
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

static void serve_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	Dprintf("%s(parent_ino = %d, name = \"%s\")\n", __FUNCTION__, (int) parent, name);
	int fid;

	if (name && name[0] != '/')
	{
		char * nname = malloc(strlen(name) + 2);
		assert(nname);
		nname[0] = '/';
		strcpy(nname+1, name);
		name = nname;
	}

	// FIXME: a fid is semantically different from an inode
	fid = CALL(frontend_cfs, open, name, 0);

	if (fid < 0)
	{
		if (fid == -12) // FIXME: 12 is E_NOT_FOUND
			fuse_reply_err(req, ENOENT);
		else
			fuse_reply_err(req, TODOERROR);
	}
	else
	{
		struct fuse_entry_param e;
		int r;

		r = hash_map_insert(names, (void*) fid, (void*) name);
		assert(r == 0);
		r = hash_map_insert(parents, (void*) fid, (void*) parent);
		assert(r == 0);

		memset(&e, 0, sizeof(e));
		static_assert(sizeof(fid) == sizeof(fuse_ino_t));
		e.ino = (fuse_ino_t) fid;
		e.attr_timeout = 1.0;
		e.entry_timeout = 1.0;
		serve_stat(e.ino, &e.attr);

		fuse_reply_entry(req, &e);

		Dprintf("%s(ino = %d, parent = %d, name = \"%s\")\n",
		       __FUNCTION__, fid, (int) parent, name);
	}
}

static void serve_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	Dprintf("%s(ino = %d, nlookup = %u)\n", __FUNCTION__, (int) ino, nlookup);
	char *name;
	int r;

	if ((r = CALL(frontend_cfs, close, (int) ino)) < 0)
		kdprintf(STDERR_FILENO, "%s:%d: close(ino = %d): %d\n", 
				 __FILE__, __LINE__, (int) ino);

	if (!(name = hash_map_erase(names, (void*) ino)))
		kdprintf(STDERR_FILENO, "%s:%d: %s(ino = %d) on inode not in names table\n",
		         __FILE__, __LINE__, (int) ino);
	free(name);

	if (!hash_map_erase(parents, (void*) ino))
		kdprintf(STDERR_FILENO, "%s:%d: %s(ino = %d) on inode not in parents table\n",
		         __FILE__, __LINE__, (int) ino);

	fuse_reply_none(req);
}

static int read_single_dir(const char * name, int fid, off_t k, dirent_t * dirent)
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

static void serve_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
	int fid;
	Dprintf("%s(ino = %d, size = %u, off = %lld)\n", __FUNCTION__, (int) ino, size, off);
	(void) fi;

	fid = (int) ino;
	if (fid == FUSE_ROOT_ID)
		fid = (int) hash_map_find_val(names, (void*) ino);

//		fuse_reply_err(req, ENOTDIR);

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
			stbuf.st_ino = (fuse_ino_t) hash_map_find_val(parents, (void*) ino);
			assert(stbuf.st_ino != 0);
			fuse_add_dirent(buf + oldsize, name, &stbuf, ++off);
		}
	}

	while (off >= 2)
	{
		dirent_t dirent;
		size_t oldsize = total_size;
		int r;

		const char * name = hash_map_find_val(names, (void*) fid);
		if ((int) ino == FUSE_ROOT_ID)
			name = "/";
		assert(name != NULL);

		r = read_single_dir(name, fid, off - 2, &dirent);
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
		stbuf.st_ino = ino;
		fuse_add_dirent(buf + oldsize, dirent.d_name, &stbuf, ++off);
	}

	fuse_reply_buf(req, buf, total_size);
	free(buf);
}

static void serve_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi)
{
//	else if ((fi->flags & 3) != O_RDONLY)
//		fuse_reply_err(req, EACCES);

	uint32_t size;
	void * data;
	uint32_t type;
	int r;

	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);

	const char * name = hash_map_find_val(names, (void *) ino);
	assert(name);

	if (ino == FUSE_ROOT_ID)
		name = "/";

	r = CALL(frontend_cfs, get_metadata, name, KFS_feature_filetype.id, &size, &data);
	assert(r >= 0);
	type = *((uint32_t*) data);

	if (type == TYPE_DIR)
		fuse_reply_err(req, EISDIR);
	fuse_reply_open(req, fi);
}

static void serve_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	Dprintf("%s(ino = %d)\n", __FUNCTION__, (int) ino);
	// nothing to release for open files
}

static void serve_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
	int fid = (int) ino;
	char * buf;
	int r;
	Dprintf("%s(ino = %d, size = %u, off = %lld)\n", __FUNCTION__, (int) ino, size, off);

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
	.lookup  = serve_lookup,
	.forget  = serve_forget,
	.getattr = serve_getattr,
	.readdir = serve_readdir,
	.open    = serve_open,
	.release = serve_release,
	.read	 = serve_read,
};


void fuse_serve_loop(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *mountpoint;
	int err = -1;
	int fd;
	int rootfid;
	int r;

	Dprintf("%s()\n", __FUNCTION__);

	assert(frontend_cfs);

	names = hash_map_create();
	assert(names);
	parents = hash_map_create();
	assert(parents);

	rootfid = CALL(frontend_cfs, open, "/", 0);
	assert(rootfid >= 0);
	r = hash_map_insert(names, (void*) FUSE_ROOT_ID, (void*) rootfid); // store actual fid, not name
	assert(r == 0);
	r = hash_map_insert(parents, (void*) FUSE_ROOT_ID, (void*) FUSE_ROOT_ID);
	assert(r == 0);

	if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
	    (fd = fuse_mount(mountpoint, &args)) != -1)
	{
		struct fuse_session *se;

		se = fuse_lowlevel_new(&args, &serve_oper, sizeof(serve_oper), NULL);
		if (se)
		{
			if (fuse_set_signal_handlers(se) != -1)
			{
				struct fuse_chan *ch = fuse_kern_chan_new(fd);
				if (ch)
				{
					fuse_session_add_chan(se, ch);
					err = fuse_session_loop(se);
				}
				fuse_remove_signal_handlers(se);
			}
			fuse_session_destroy(se);
		}
		close(fd);
	}
	fuse_unmount(mountpoint);
	fuse_opt_free_args(&args);

	exit(err ? 1 : 0);
}
