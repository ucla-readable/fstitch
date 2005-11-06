#include <inc/lib.h>
#include <lib/dirent.h>
#include <kfs/lfs.h>

static int flag[256];

static void lsdir(const char*, const char*);
static void ls1(const char*, bool, off_t, const char*);

static void
ls(const char *path, const char *prefix)
{
	int r;
	struct Stat st;

	if ((r = stat(path, &st)) < 0) {
		kdprintf(STDERR_FILENO, "stat %s: %e\n", path, r);
		exit(0);
	}
	if (st.st_isdir && !flag['d'])
		lsdir(path, prefix);
	else
		ls1(0, st.st_isdir, st.st_size, path);
}

static void
lsdir(const char *path, const char *prefix)
{
	int fd;
	int r;
	off_t base = 0;
	char buf[256];
	struct dirent *d;

	if ((fd = open(path, O_RDONLY)) < 0) {
		kdprintf(STDERR_FILENO, "open %s: %e", path, fd);
		exit(0);
	}
	for (;;) {
		r = getdirentries(fd, buf, sizeof(buf), &base);
		if (r <= 0) break;

		d = (struct dirent *) &buf;
		while((uintptr_t) d < (uintptr_t) &buf + r)
		{
			ls1(prefix, d->d_type==TYPE_DIR, d->d_filesize, d->d_name);
			d = (struct dirent *) ((uintptr_t) d + d->d_reclen);
		}
	}
}

static void
ls1(const char *prefix, bool isdir, off_t size, const char *name)
{
	char *sep;

	if(flag['l'])
		kdprintf(1, "%11d %c ", size, isdir ? 'd' : '-');
	if(prefix) {
		if (prefix[0] && prefix[strlen(prefix)-1] != '/')
			sep = "/";
		else
			sep = "";
		kdprintf(1, "%s%s", prefix, sep);
	}
	kdprintf(1, "%s", name);
	if(flag['F'] && isdir)
		kdprintf(1, "/");
	kdprintf(1, "\n");
}

static void
usage(void)
{
	kdprintf(1, "usage: ls [-dFl] [file...]\n");
	exit(0);
}

void
umain(int argc, char **argv)
{
	int i;

	ARGBEGIN{
	default:
		usage();
	case 'd':
	case 'F':
	case 'l':
		flag[(uint8_t)ARGC()]++;
		break;
	}ARGEND

	if (argc == 0)
		ls("/", "");
	else {
		for (i=0; i<argc; i++)
			ls(argv[i], argv[i]);
	}
}
