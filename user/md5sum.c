#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/md5.h>

void usage(char *);

static unsigned char buf[4096];

void
umain(int argc, char *argv[])
{
	int r, fd, i;
	unsigned char out[16];
	MD5_CTX context;

	if (argc != 2) usage(argv[0]);

	MD5Init(&context);

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(STDERR_FILENO, "Unable to open %s\n", argv[1]);
		exit();
	}
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		MD5Update(&context, buf, r);
	}
	close(fd);
	MD5Final(out, &context);
	for (i = 0; i < 16; i++)
		printf("%02x", out[i]);
	printf(" %s\n", argv[1]);
}

void
usage(char *s)
{
	printf("usage: %s filename\n", s);
	exit();
}
