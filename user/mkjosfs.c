#include <inc/lib.h>
#include <inc/fs.h>

static void usage(char *s)
{
	printf("usage: %s [existing filename|devicename]\n", s);
	exit(0);
}

static uint32_t free_map[1024];

void umain(int argc, char *argv[])
{
	struct Super s;
	struct File *f;
	struct Stat st;
	int flen, r, fd, i;

	if (argc != 2) usage(argv[0]);
	fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		kdprintf(STDERR_FILENO, "Unable to open %s\n", argv[1]);
		exit(0);
	}
	r = fstat(fd, &st);
	if (r < 0) {
		kdprintf(STDERR_FILENO, "Unable to stat %s\n", argv[1]);
		exit(0);
	}
	if (st.st_isdir) {
		kdprintf(STDERR_FILENO, "Error: %s is a directory\n", argv[1]);
		exit(0);
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
	
	memset(free_map, 0, sizeof(free_map));
	for(i = 3; i < s.s_nblocks; i++)
		free_map[i / 32] |= 1 << (i % 32);

	seek(fd, 8192);
	r = write(fd, free_map, sizeof(free_map));
	if (r < sizeof(free_map)) {
		kdprintf(STDERR_FILENO, "Error: Only wrote %d bytes to %s, needed %d\n",
		                        r, argv[1], sizeof(free_map));
		exit(1);
	}
	seek(fd, 4096);
	r = write(fd, &s, sizeof(s));
	if (r < sizeof(s)) {
		kdprintf(STDERR_FILENO, "Error: Only wrote %d bytes to %s, needed %d\n",
		                        r, argv[1], sizeof(s));
		exit(1);
	}
	close(fd);
	printf("Success. New filesystem has %d blocks.\n", s.s_nblocks);
}
