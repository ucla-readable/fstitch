#include <inc/lib.h>
#include <inc/kpl.h>

char buf[8192];


void
umain(int argc, char **argv)
{
	int r;
	char data[11];
	int fd;

	fd = kpl_open("/temp", 0);
	printf("f = kpl_open(\"/temp\", 0); = %d\n", fd);
	r = read(fd, data, sizeof(data)-1);
	printf("r = read(f, 0, 10, data); = %d\n", r);
	data[sizeof(data)-1] = '\0';
	r = close(fd);
	printf("r = close(f); = %d %e\n", r);

	printf("data: [%s]\n", data);
}

