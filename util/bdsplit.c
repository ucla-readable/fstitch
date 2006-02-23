#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
	int i, r, num = argc - 1;
	char data[512];
	int * fd;

	if (argc < 3)
	{
		printf("%s input output1 output2 ... outputN\n", argv[0]);
		return 1;
	}

	fd = malloc(sizeof(int) * (num));
	if (!fd)
	{
		perror("malloc");
		return 1;
	}

	fd[0] = open(argv[1], O_RDONLY);
	if (fd[0] < 0)
	{
		perror(argv[1]);
		free(fd);
		return 1;
	}

	for (i = 1; i < num; i++)
	{
		fd[i] = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
		if (fd[i] < 0)
		{
			perror(argv[i + 1]);
			free(fd);
			return -1;
		}
	}

	i = 1;
	for (;;)
	{
		r = read(fd[0], data, 512);
		if (r != 512)
			break;

		write(fd[i], data, 512);
		if (++i == num)
			i = 1;
	}

	free(fd);
	return 0;
}
