#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

/*
 * This file originally from fuse_ll.c.
 * Perhaps an interesting place to start from. First compile, then run.
 */

static const char *fuse_str = "Fuse World!\n";
static const char *fuse_name = "fuse";

static int fuse_stat(fuse_ino_t ino, struct stat *stbuf)
{
    stbuf->st_ino = ino;
    switch (ino) {
    case 1:
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        break;

    case 2:
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(fuse_str);
        break;

    default:
        return -1;
    }
    return 0;
}

static void fuse_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi)
{
    struct stat stbuf;

    (void) fi;

    memset(&stbuf, 0, sizeof(stbuf));
    if (fuse_stat(ino, &stbuf) == -1)
        fuse_reply_err(req, ENOENT);
    else
        fuse_reply_attr(req, &stbuf, 1.0);
}

static void fuse_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;

    if (parent != 1 || strcmp(name, fuse_name) != 0)
        fuse_reply_err(req, ENOENT);
    else {
        memset(&e, 0, sizeof(e));
        e.ino = 2;
        e.attr_timeout = 1.0;
        e.entry_timeout = 1.0;
        fuse_stat(e.ino, &e.attr);

        fuse_reply_entry(req, &e);
    }
}

struct dirbuf {
    char *p;
    size_t size;
};

static void dirbuf_add(struct dirbuf *b, const char *name, fuse_ino_t ino)
{
    struct stat stbuf;
    size_t oldsize = b->size;
    b->size += fuse_dirent_size(strlen(name));
    b->p = (char *) realloc(b->p, b->size);
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;
    fuse_add_dirent(b->p + oldsize, name, &stbuf, b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
                             off_t off, size_t maxsize)
{
    if (off < bufsize)
        return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

static void fuse_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                             off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    if (ino != 1)
        fuse_reply_err(req, ENOTDIR);
    else {
        struct dirbuf b;

        memset(&b, 0, sizeof(b));
        dirbuf_add(&b, ".", 1);
        dirbuf_add(&b, "..", 1);
        dirbuf_add(&b, fuse_name, 2);
        reply_buf_limited(req, b.p, b.size, off, size);
        free(b.p);
    }
}

static void fuse_ll_open(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
    if (ino != 2)
        fuse_reply_err(req, EISDIR);
    else if ((fi->flags & 3) != O_RDONLY)
        fuse_reply_err(req, EACCES);
    else
        fuse_reply_open(req, fi);
}

static void fuse_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                         off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    assert(ino == 2);
    reply_buf_limited(req, fuse_str, strlen(fuse_str), off, size);
}

static struct fuse_lowlevel_ops fuse_ll_oper = {
    .lookup     = fuse_ll_lookup,
    .getattr	= fuse_ll_getattr,
    .readdir    = fuse_ll_readdir,
    .open       = fuse_ll_open,
    .read	= fuse_ll_read,
};


void fuse_loop(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *mountpoint;
	int err = -1;
	int fd;

	if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
	    (fd = fuse_mount(mountpoint, &args)) != -1)
	{
		struct fuse_session *se;

		se = fuse_lowlevel_new(&args, &fuse_ll_oper, sizeof(fuse_ll_oper),
		                       NULL);
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
