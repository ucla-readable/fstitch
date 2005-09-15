#include <inc/lib.h>

static uint8_t buffer[4096];

static int copy(const char * from, const char * to)
{
	int size, to_fd, from_fd = open(from, O_RDONLY);
	if(from_fd < 0)
	{
		printf("%s: %e\n", from, from_fd);
		return from_fd;
	}
	
	to_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC);
	if(to_fd < 0)
	{
		close(from_fd);
		printf("%s: %e\n", to, to_fd);
		return to_fd;
	}
	
	size = read(from_fd, buffer, 4096);
	while(size > 0)
	{
		int wrote = write(to_fd, buffer, size);
		if(wrote != size)
		{
			if(wrote < 0)
				printf("%s: %e\n", to, wrote);
			else
			{
				printf("%s: short write\n", to);
				size = 0;
			}
			break;
		}
		size = read(from_fd, buffer, 4096);
	}
	
	if(size < 0 && size != -E_EOF)
		printf("%s: %e\n", from, size);
	
	close(to_fd);
	close(from_fd);
	
	return size;
}

void umain(int argc, char **argv)
{
	int r;
	
	if(argc != 3)
	{
		printf("Usage: %s source dest\n", argv[0]);
		return;
	}
	
	r = rename(argv[1], argv[2]);
	
	/* rename() maybe can't overwrite files... (depends on base LFS module) */
	if(r == -E_FILE_EXISTS)
	{
		r = remove(argv[2]);
		if(r < 0)
		{
			printf("%s: %e\n", argv[2], r);
			r = 0;
		}
		else
			/* try rename again */
			r = rename(argv[1], argv[2]);
	}
	
	if(r == -E_INVAL)
	{
		/* might just be on different filesystems */
		r = copy(argv[1], argv[2]);
		if(r >= 0)
		{
			r = remove(argv[1]);
			if(r < 0)
				printf("%s: %e\n", argv[1], r);
		}
	}
	else if(r < 0)
		printf("rename(%s, %s): %e\n", argv[1], argv[2], r);
}
