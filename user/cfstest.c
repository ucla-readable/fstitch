#include <inc/lib.h>
#include <inc/cfs_ipc_client.h>

char buf[8192];

void
umain(int argc, char **argv)
{
	int f, r;
	char data[11];

	f = cfs_open("/temp", 0);
	printf("f = cfs_open(\"/temp\", 0); = %d\n", f);
	r = cfs_write(f, 0, 10, "0123456789");
	printf("r = cfs_write(f, 0, 10, \"0123456789\"); = %d\n", r);
	r = cfs_close(f);
	printf("r = cfs_close(f); = %d\n", r);
	
	f = cfs_open("/temp", 0);
	printf("f = cfs_open(\"/temp\", 0); = %d\n", f);
	r = cfs_read(f, 0, 10, data);
	printf("r = cfs_read(f, 0, 10, data); = %d\n", r);
	data[10] = '\0';
	r = cfs_close(f);
	printf("r = cfs_close(f); = %d\n", r);

	printf("data: [%s]\n", data);
}

