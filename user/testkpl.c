#include <inc/lib.h>
#include <inc/kpl.h>

char data[2*PGSIZE];

void
umain(int argc, char **argv)
{
	uint32_t offset = 513;
	int r;
	int fd;

	if (argc != 3)
	{
		fprintf(STDERR_FILENO, "Usage: %s <path> <text_to_write>\n", argv[0]);
		exit();
	}

	char * filename = argv[1];
	char * write_data = argv[2];

	fd = kpl_open(filename, O_RDWR);
	printf("kpl_open(\"/%s\", 0) = %e\n", filename, fd);

	r = read(fd, data, offset);
	printf("read(fd, data, 0x%x) = %e\n", offset, r);
	printf("data: [%s]\n", data);

	r = seek(fd, 0);
	printf("seek(fd, 0) = %e\n", r);

	r = write(fd, write_data, strlen(write_data));
	printf("write(fd, \"%s\", 0x%x) = %e\n", write_data, strlen(write_data), r);

	r = seek(fd, 0);
	printf("seek(fd, 0) = %e\n", r);

	r = read(fd, data, offset);
	printf("read(fd, data, 0x%x) = %e\n", offset, r);
	printf("data: [%s]\n", data);

	r = read(fd, data, offset);
	printf("read(fd, data, 0x%x) = %e\n", offset, r);
	printf("data: [%s]\n", data);

	r = close(fd);
	printf("close(fd) = %e\n", r);
}

