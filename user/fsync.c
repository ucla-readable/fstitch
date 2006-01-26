#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main(int argc, char ** argv)
{
	char * file;
	int fd;
	int r;

	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s <FILE>\n", argv[0]);
		exit(1);
	}
	file = argv[1];

	fd = open(file, O_RDONLY);
	if (fd == -1)
	{
		perror("open()");
		exit(1);
	}

	r = fsync(fd);
	if (r == -1)
	{
		perror("fsync()");
		exit(1);
	}

	r = close(fd);
	if (r == -1)
	{
		perror("close()");
		exit(1);
	}

	return 0;
}
