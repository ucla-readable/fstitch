// Public definitions for the POSIX-like file descriptor emulation layer
// that our user-land support library implements for the use of applications.
// See the code in the lib directory for the implementation details.

#ifndef KUDOS_INC_FD_H
#define KUDOS_INC_FD_H

#include <inc/types.h>
#include <inc/fs.h>

// pre-declare for forward references
struct Fd;
struct Stat;
struct Dev;

struct Dev
{
	int dev_id;
	char* dev_name;
	int (*dev_read)(struct Fd* fd, void* buf, size_t len, off_t offset);
	int (*dev_read_nb)(struct Fd* fd, void* buf, size_t len, off_t offset);
	int (*dev_write)(struct Fd* fd, const void* buf, size_t len, off_t offset);
	int (*dev_close)(struct Fd* fd);
	int (*dev_stat)(struct Fd* fd, struct Stat* stat);
	int (*dev_seek)(struct Fd* fd, off_t pos);
	int (*dev_trunc)(struct Fd* fd, off_t length);
};

struct Fd
{
	int fd_dev_id;
	off_t fd_offset;
	int fd_omode;
	union {
		// File server files
		struct {
			int id;
			struct File file;
		} fd_file;
		// KPL files
		struct {
			int fid;
			int index;
		} fd_kpl;
	};
};

struct Stat
{
	char st_name[MAXNAMELEN];
	off_t st_size;
	int st_isdir;
	struct Dev* st_dev;
};

char*	fd2data(struct Fd*);
int	fd2num(struct Fd*);
int	fd_alloc(struct Fd** fd);
int	fd_close(struct Fd* fd, bool must_exist);
int	fd_lookup(int fdnum, struct Fd** fd);
int	dev_lookup(int devid, struct Dev** dev);

extern struct Dev devcons;
extern struct Dev devfile;
extern struct Dev devpipe;
extern struct Dev devkpl;

#endif	// not KUDOS_INC_FD_H
