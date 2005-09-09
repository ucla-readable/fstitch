#include <inc/lib.h>
#include <inc/fs.h>

static void
usage(char *s)
{
	printf("usage: %s [existing filename|devicename]\n", s);
	exit();
}

void
umain(int argc, char *argv[])
{
	struct Super s;
	struct File *f;
	struct Stat st;
	int flen;
	int r;
	int fd;
	int i;

	if (argc != 2) usage(argv[0]);
	fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		fprintf(STDERR_FILENO, "Unable to open %s\n", argv[1]);
		exit();
	}
	r = fstat(fd, &st);
	if (r < 0) {
		fprintf(STDERR_FILENO, "Unable to stat %s\n", argv[1]);
		exit();
	}
	if (st.st_isdir) {
		fprintf(STDERR_FILENO, "Error: %s is a directory\n", argv[1]);
		exit();
	}

	flen = st.st_size;
	
	s.s_magic = FS_MAGIC;
	s.s_nblocks = flen/4096;

	f = &s.s_root;

	strcpy(f->f_name, "/");
	f->f_size = 0;
	f->f_type = FTYPE_DIR;
	for (i = 0; i < NDIRECT; i++)
		f->f_direct[i] = 0;
	f->f_indirect = 0;
	f->f_dir = 0;

	r = write(fd, &s, sizeof(s));
	if (r < sizeof(s)) {
		fprintf(STDERR_FILENO, "Error: Only wrote %d bytes to %s, needed %d\n",
				r, argv[1], sizeof(s));
		exit();
	}
	close(fd);
	printf("Success. New filesystem has %d blocks.\n", s.s_nblocks);
}
