#include <inc/lib.h>
#include <inc/kpl.h>

char data[2*PGSIZE];

void
umain(int argc, char **argv)
{
	uint32_t offset = 513;
	int r;
	int fd;

	assert(argc == 3);
	char * filename = argv[1];
	char * write_data = argv[2];

	fd = kpl_open(filename, O_RDWR);
	printf("f = kpl_open(\"/%s\", 0); = %e\n", filename, fd);

	r = read(fd, data, offset);
	if (r < 0)
		printf("r = read(f, , 0x%x, data); = %e\n", r);
	printf("data: [%s]\n", data);

	r = seek(fd, 0);
	printf("r = seek(f, 0); = %e\n", r);

	r = write(fd, write_data, strlen(write_data));
	printf("r = write(f, \"%s\", 0x%x); = %e\n", write_data, strlen(write_data), r);

	r = seek(fd, 0);
	printf("r = seek(f, 0); = %e\n", r);

	r = read(fd, data, offset);
	if (r < 0)
		printf("r = read(f, , 0x%x, data); = %e\n", r);
	printf("data: [%s]\n", data);

	r = read(fd, data, offset);
	printf("r = read(f, , 0x%x, data); = %e\n", offset, r);
	data[offset] = '\0';
	printf("data: [%s]\n", data);

	r = read(fd, data, offset);
	printf("r = read(f, , 0x%x, data); = %e\n", offset, r);
	data[offset] = '\0';
	printf("data: [%s]\n", data);

	r = close(fd);
	printf("r = close(f); = %e\n", r);
}

