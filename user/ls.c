#include <inc/lib.h>
#include <inc/dirent.h>
#include <kfs/lfs.h>

int flag[256];

void lsdir(const char*, const char*);
void ls1(const char*, bool, const char*);

void
ls(const char *path, const char *prefix)
{
	int r;
	struct Stat st;

	if ((r = stat(path, &st)) < 0) {
		fprintf(STDERR_FILENO, "stat %s: %e\n", path, r);
		exit();
	}
	if (st.st_isdir && !flag['d'])
		lsdir(path, prefix);
	else
		ls1(0, st.st_isdir, path);
}

void
lsdir(const char *path, const char *prefix)
{
	int fd;
	int r;
	off_t base = 0;
	char buf[256];
	struct dirent *d;

	if ((fd = open(path, O_RDONLY)) < 0) {
		fprintf(STDERR_FILENO, "open %s: %e", path, fd);
		exit();
	}
	for (;;) {
		r = getdirentries(fd, buf, sizeof(buf), &base);
		if (r <= 0) break;

		d = (struct dirent *) &buf;
		while((uintptr_t) d < (uintptr_t) &buf + r)
		{
			ls1(prefix, d->d_type==TYPE_DIR, d->d_name);
			d = (struct dirent *) ((uintptr_t) d + d->d_reclen);
		}
	}
}

void
ls1(const char *prefix, bool isdir, const char *name)
{
	char *sep;
	struct Stat s;
	int r;

	if(flag['l'])
	{
		if ((r = stat(name, &s)) < 0)
			fprintf(STDERR_FILENO, "ls: stat failed: %e\n", r);
		else
			printf("%11d %c ", s.st_size, isdir ? 'd' : '-');
	}
	if(prefix) {
		if (prefix[0] && prefix[strlen(prefix)-1] != '/')
			sep = "/";
		else
			sep = "";
		fprintf(1, "%s%s", prefix, sep);
	}
	fprintf(1, "%s", name);
	if(flag['F'] && isdir)
		fprintf(1, "/");
	fprintf(1, "\n");
}

void
usage(void)
{
	fprintf(1, "usage: ls [-dFl] [file...]\n");
	exit();
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

