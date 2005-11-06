#include <inc/lib.h>
#include <inc/kpl.h>
#include <inc/cfs_ipc_client.h>

static char data[2*PGSIZE];

void
umain(int argc, char **argv)
{
	char * filename = argv[1];
	char * write_data = argv[2];

	uint32_t length = 513;
	int r, fd;

	/* support debugging as a "hidden" feature... */
	if(argc == 2 && !strcmp(argv[1], "--debug"))
	{
		cfs_debug();
		exit(0);
	}
	if(argc == 2 && !strcmp(argv[1], "--shutdown"))
	{
		cfs_shutdown();
		exit(0);
	}

	if(argc != 3)
	{
		kdprintf(STDERR_FILENO, "Usage: %s <path> <text_to_write>\n", argv[0]);
		exit(0);
	}

	fd = kpl_open(filename, O_RDWR);
	printf("kpl_open(\"/%s\", 0) = %e\n", filename, fd);

	r = read(fd, data, length);
	printf("read(fd, data, 0x%x) = %e\n", length, r);
	printf("data: [%s]\n", data);

	r = seek(fd, 0);
	printf("seek(fd, 0) = %e\n", r);

	r = write(fd, write_data, strlen(write_data));
	printf("write(fd, \"%s\", 0x%x) = %e\n", write_data, strlen(write_data), r);

	r = seek(fd, 0);
	printf("seek(fd, 0) = %e\n", r);

	r = read(fd, data, length);
	printf("read(fd, data, 0x%x) = %e\n", length, r);
	printf("data: [%s]\n", data);

	r = read(fd, data, length);
	printf("read(fd, data, 0x%x) = %e\n", length, r);
	printf("data: [%s]\n", data);

	r = close(fd);
	printf("close(fd) = %e\n", r);
}
